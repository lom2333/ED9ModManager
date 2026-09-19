#include "modkit/mod_merge_orchestrator.h"
#include "modkit/patch_config.h"
#include "modkit/dat_patch.h"
#include "modkit/scene_merge.h"
#include "modkit/tbl_codec.h"
#include "modkit/tbl_merge.h"
#include "modkit/fpac_reader.h"
#include "modkit/mod_archive.h"
#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
std::string wtou8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr); return s;
}
std::wstring u8tow(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n); return w;
}
}

namespace fs = std::filesystem;

namespace ed9loader {
namespace modkit {
namespace orchestrator {

static std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool writeAll(const fs::path& p, const std::vector<uint8_t>& d) {
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    if (!d.empty()) f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}
static void writeText(const fs::path& p, const std::string& s) {
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary); f << s;
}

struct InjectEntry {
    std::string map;
    std::string script;
    std::string mod;
    std::string lang;
    fs::path datPath;
};

static std::vector<InjectEntry> parseScriptInject(const fs::path& p, const std::string& mod, const std::string& lang, std::string& log) {
    std::vector<InjectEntry> out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    nlohmann::json j;
    try { f >> j; } catch (const std::exception& e) { log += "[modkit] add_dat_ini parse FAIL " + p.generic_string() + ": " + e.what() + "\n"; return out; }
    if (j.contains("inject") && j["inject"].is_array()) {
        for (const auto& it : j["inject"]) {
            InjectEntry e;
            e.map = it.value("map", std::string());
            e.script = it.value("script", std::string());
            e.mod = mod;
            e.lang = lang;
            std::string file = it.value("file", e.script + ".dat");
            e.datPath = p.parent_path() / file;
            if (e.map.empty() || e.script.empty()) { log += "[modkit] add_dat_ini skip (need map+script) in " + p.generic_string() + "\n"; continue; }
            out.push_back(std::move(e));
        }
    }
    return out;
}

struct MonLoadEntry { std::string map, table, mod; };
static std::vector<MonLoadEntry> parseMonLoad(const fs::path& p, const std::string& mod, std::string& log) {
    std::vector<MonLoadEntry> out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    nlohmann::json j;
    try { f >> j; } catch (const std::exception&) { return out; }
    if (j.contains("mon_load") && j["mon_load"].is_array()) {
        for (const auto& it : j["mon_load"]) {
            MonLoadEntry e;
            e.map = it.value("map", std::string());
            e.table = it.value("table", std::string());
            e.mod = mod;
            if (e.map.empty() || e.table.empty()) { log += "[modkit] mon_load skip (need map+table) in " + p.generic_string() + "\n"; continue; }
            out.push_back(std::move(e));
        }
    }
    return out;
}

struct MonEventEntry { std::string map, event, mod; };
static std::vector<MonEventEntry> parseMonEvent(const fs::path& p, const std::string& mod, std::string& log) {
    std::vector<MonEventEntry> out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    nlohmann::json j;
    try { f >> j; } catch (const std::exception&) { return out; }
    if (j.contains("mon_event") && j["mon_event"].is_array()) {
        for (const auto& it : j["mon_event"]) {
            MonEventEntry e;
            e.map = it.value("map", std::string());
            e.event = it.value("event", std::string());
            e.mod = mod;
            if (e.map.empty() || e.event.empty()) { log += "[modkit] mon_event skip (need map+event) in " + p.generic_string() + "\n"; continue; }
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::wstring ModRoot(const std::wstring& modsDir, const std::string& modName) {
    fs::path md(modsDir);
    std::wstring wname = u8tow(modName);
    std::error_code ec;
    fs::path folder = md / wname;
    if (fs::is_directory(folder, ec)) return folder.wstring();
    if (!archive::FindArchive(md, wname).empty())
        return (archive::StagingRoot(md) / wname).wstring();
    return folder.wstring();
}

std::vector<ModInfo> ScanMods(const std::wstring& modsDir) {
    fs::path md(modsDir);
    std::error_code ec;
    std::vector<std::string> present;
    if (fs::is_directory(md, ec))
        for (const auto& e : fs::directory_iterator(md, ec))
            if (e.is_directory()) present.push_back(wtou8(e.path().filename().wstring()));
    for (const auto& an : archive::ListArchiveMods(md))
        if (std::find(present.begin(), present.end(), an) == present.end()) present.push_back(an);
    std::sort(present.begin(), present.end());

    nlohmann::json j;
    { std::ifstream f(md / "mods.json", std::ios::binary); if (f) { try { f >> j; } catch (...) { j = nlohmann::json(); } } }

    std::vector<ModInfo> ordered;
    std::set<std::string> taken;
    if (j.contains("mods") && j["mods"].is_array()) {
        for (const auto& it : j["mods"]) {
            if (!it.is_object()) continue;
            std::string name = it.value("name", std::string());
            if (name.empty() || taken.count(name)) continue;
            if (std::find(present.begin(), present.end(), name) == present.end()) continue;
            ModInfo m; m.name = name; m.enabled = it.value("enabled", true);
            if (it.contains("disabled") && it["disabled"].is_array())
                for (const auto& d : it["disabled"]) if (d.is_string()) m.disabled.push_back(d.get<std::string>());
            ordered.push_back(std::move(m)); taken.insert(name);
        }
    }
    for (const auto& n : present) if (!taken.count(n)) ordered.push_back(ModInfo{ n, true });
    return ordered;
}

static std::string modsToJsonText(const std::vector<ModInfo>& mods) {
    nlohmann::json outj; outj["version"] = 1; outj["mods"] = nlohmann::json::array();
    for (const auto& m : mods) {
        nlohmann::json mj; mj["name"] = m.name; mj["enabled"] = m.enabled;
        if (!m.disabled.empty()) mj["disabled"] = m.disabled;
        outj["mods"].push_back(std::move(mj));
    }
    return outj.dump(2);
}

bool SaveMods(const std::wstring& modsDir, const std::vector<ModInfo>& mods) {
    std::error_code ec; fs::create_directories(modsDir, ec);
    std::ofstream o(fs::path(modsDir) / "mods.json", std::ios::binary);
    if (!o) return false;
    o << modsToJsonText(mods);
    return (bool)o;
}

static std::vector<ModInfo> LoadModManifest(const fs::path& modsDir, std::string& log) {
    std::vector<ModInfo> ordered = ScanMods(modsDir.wstring());
    if (readAll(modsDir / "mods.json") != modsToJsonText(ordered))
        if (SaveMods(modsDir.wstring(), ordered))
            log += "[modkit] mods.json reconciled (" + std::to_string(ordered.size()) + " mods)\n";
    return ordered;
}

Paths FromGameDir(const std::wstring& gameDir) {
    Paths p;
    p.modsDir = gameDir + L"\\Mod";
    p.pacSteamDir = gameDir + L"\\pac\\steam";
    p.cacheDir = gameDir + L"\\ED9Loader\\cache\\merged";
    p.schemasDir = gameDir + L"\\ED9Loader\\schemas";
    return p;
}

static bool entryOff(const std::set<std::string>& off, const char* sec, size_t idx) {
    return !off.empty() && off.count(std::string(sec) + "/" + std::to_string(idx)) > 0;
}

static void parseTblFile(const fs::path& p, std::string& outTable,
                         std::vector<nlohmann::json>& addRows,
                         std::vector<tbl_merge::EditOp>& edits,
                         std::vector<nlohmann::json>& cloneRows,
                         const std::string& modName, const std::set<std::string>& off, std::string& log) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return;
    nlohmann::json j;
    try { f >> j; } catch (const std::exception& e) { log += "[modkit] tbl file parse FAIL " + p.generic_string() + ": " + e.what() + "\n"; return; }
    if (outTable.empty()) outTable = j.value("table", std::string());
    const bool isLp = (outTable == "LookPointTableData" || p.stem() == "t_lookpoint");
    auto aliasLp = [isLp](nlohmann::json& obj) {
        if (!isLp || !obj.is_object()) return;
        static const std::pair<const char*, const char*> M[] = { { "map", "text1" }, { "name", "text2" }, { "label", "text3" } };
        for (const auto& m : M)
            if (obj.contains(m.first) && !obj.contains(m.second)) { obj[m.second] = obj[m.first]; obj.erase(m.first); }
    };
    if (j.contains("clone_rows") && j["clone_rows"].is_array()) {
        size_t i = 0;
        for (const auto& c : j["clone_rows"]) { if (c.is_object() && !entryOff(off, "clone_rows", i)) cloneRows.push_back(c); ++i; }
    }
    if (j.contains("add_npc") && j["add_npc"].is_array()) {
        size_t npcIdx = 0;
        for (const auto& e : j["add_npc"]) {
            bool skip = entryOff(off, "add_npc", npcIdx); ++npcIdx;
            if (!e.is_object() || skip) continue;
            auto alias = [](const std::string& k) -> std::string {
                return tbl_merge::AliasNpcParamField(k);
            };
            std::set<std::string> provided;
            for (auto it = e.begin(); it != e.end(); ++it) if (it.key() != "behavior_from") provided.insert(alias(it.key()));
            nlohmann::json set;
            set["flags_18"] = 1; set["type_28"] = 1;
            for (const char* z : { "flags_04", "flags_1C", "param_float_3C", "param_float_40", "param_44",
                                   "float_48", "float_4C", "param_50", "param_54", "param_70", "param_74",
                                   "param_88", "param_8C", "param_98", "param_9C" })
                if (!provided.count(z)) set[z] = 0;
            for (const char* rr : { "resource_ref_60", "resource_ref_68", "resource_ref_78", "resource_ref_80", "resource_ref_90" })
                if (!provided.count(rr)) set[rr] = "";
            for (auto it = e.begin(); it != e.end(); ++it)
                if (it.key() != "behavior_from") set[it.key()] = it.value();
            nlohmann::json op;
            op["from_index"] = e.value("behavior_from", 0);
            op["set"] = std::move(set);
            cloneRows.push_back(std::move(op));
        }
    }
    if (j.contains("add_rows") && j["add_rows"].is_array()) {
        size_t i = 0;
        for (const auto& row : j["add_rows"]) {
            if (row.is_object() && !entryOff(off, "add_rows", i)) { nlohmann::json r = row; aliasLp(r); addRows.push_back(std::move(r)); }
            ++i;
        }
    }
    if (j.contains("edit_rows") && j["edit_rows"].is_array()) {
        size_t i = 0;
        for (const auto& e : j["edit_rows"]) {
            bool skip = entryOff(off, "edit_rows", i); ++i;
            if (!e.is_object() || skip) continue;
            tbl_merge::EditOp op;
            op.match = e.value("match", nlohmann::json::object());
            op.set = e.value("set", nlohmann::json::object());
            aliasLp(op.match); aliasLp(op.set);
            op.source = modName;
            edits.push_back(std::move(op));
        }
    }
}

struct TblSrc { fs::path path; std::set<std::string> off; std::string lang; std::string mod; };
struct AssetSrc { fs::path src; std::string rel; std::string mod; };
struct PathSrc { fs::path path; std::string mod; std::string lang; };

RunResult Run(const Paths& paths, bool force) {
    RunResult r;
    std::string log;

    std::error_code ec;
    archive::EnsureAllStaged(fs::path(paths.modsDir), log);
    std::vector<ModInfo> manifest = LoadModManifest(paths.modsDir, log);
    std::vector<fs::path> datFiles;
    std::vector<PathSrc> patchFiles, injectFiles, datReplaceFiles, tableReplaceFiles;
    std::vector<PathSrc> datPatchFiles;
    std::vector<TblSrc> tblDirFiles;
    std::vector<AssetSrc> assetFiles;
    int enabledMods = 0;
    for (const ModInfo& m : manifest) {
        if (!m.enabled) continue;
        ++enabledMods;
        fs::path dir = ModRoot(paths.modsDir, m.name);
        auto off = [&](const std::string& rel) {
            return std::find(m.disabled.begin(), m.disabled.end(), rel) != m.disabled.end();
        };
        auto entryOffSet = [&](const std::string& fileRel) {
            std::set<std::string> s; std::string pre = fileRel + "#";
            for (const auto& d : m.disabled) if (d.rfind(pre, 0) == 0) s.insert(d.substr(pre.size()));
            return s;
        };
        { fs::path sceneDir = dir / "scene_add_json";
          if (fs::is_directory(sceneDir, ec)) {
              std::vector<fs::path> sfs;
              for (const auto& sf : fs::directory_iterator(sceneDir, ec))
                  if (sf.is_regular_file() && sf.path().extension() == ".json"
                      && !off("scene_add_json/" + sf.path().filename().generic_string())) sfs.push_back(sf.path());
              std::sort(sfs.begin(), sfs.end());
              for (auto& s : sfs) patchFiles.push_back({ std::move(s), m.name, "" }); } }
        { fs::path ij = dir / "add_dat_ini.json";
          if (fs::exists(ij, ec) && !off("add_dat_ini.json")) injectFiles.push_back({ ij, m.name, "" }); }
        { fs::path dp = dir / "ListExtraLoad";
          if (fs::is_directory(dp, ec)) {
              std::vector<fs::path> pfs;
              for (const auto& pf : fs::directory_iterator(dp, ec))
                  if (pf.is_regular_file() && pf.path().extension() == ".json"
                      && !off("ListExtraLoad/" + pf.path().filename().generic_string())) pfs.push_back(pf.path());
              std::sort(pfs.begin(), pfs.end());
              for (auto& p : pfs) datPatchFiles.push_back({ std::move(p), m.name, "" }); } }
        { std::vector<fs::path> dats;
          for (const auto& df : fs::directory_iterator(dir, ec))
              if (df.is_regular_file() && df.path().extension() == ".dat"
                  && !off(df.path().filename().generic_string())) dats.push_back(df.path());
          std::sort(dats.begin(), dats.end());
          for (auto& d : dats) datFiles.push_back(std::move(d)); }
        { fs::path assetDir = dir / "asset";
          if (fs::is_directory(assetDir, ec)) {
              std::vector<AssetSrc> as;
              for (auto it = fs::recursive_directory_iterator(assetDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                  if (ec) break;
                  if (!it->is_regular_file(ec)) continue;
                  std::string rel = "asset/" + fs::relative(it->path(), assetDir, ec).generic_string();
                  if (off(rel)) continue;
                  as.push_back({ it->path(), rel, m.name });
              }
              std::sort(as.begin(), as.end(), [](const AssetSrc& a, const AssetSrc& b){ return a.rel < b.rel; });
              for (auto& a : as) assetFiles.push_back(std::move(a)); } }
        for (const char* ns : { "voice", "se", "bgm1", "bgm2", "bgm3" }) {
            fs::path nsDir = dir / ns;
            if (!fs::is_directory(nsDir, ec)) continue;
            std::vector<AssetSrc> rs;
            for (auto de = fs::directory_iterator(nsDir, ec); de != fs::directory_iterator(); de.increment(ec)) {
                if (ec) break;
                std::error_code e2;
                if (de->is_directory(e2)) {
                    const std::string gname = wtou8(de->path().filename().wstring());
                    const bool isGroup = (gname != "wav");
                    if (isGroup && off(std::string(ns) + "/" + gname)) continue;
                    for (auto it = fs::recursive_directory_iterator(de->path(), ec);
                         it != fs::recursive_directory_iterator(); it.increment(ec)) {
                        if (ec) break;
                        if (!it->is_regular_file(e2)) continue;
                        std::string rel = isGroup
                            ? std::string(ns) + "/wav/" + wtou8(it->path().filename().wstring())
                            : std::string(ns) + "/" + fs::relative(it->path(), nsDir, ec).generic_string();
                        if (off(rel)) continue;
                        rs.push_back({ it->path(), rel, m.name });
                    }
                } else if (de->is_regular_file(e2)) {
                    std::string rel = std::string(ns) + "/wav/" + wtou8(de->path().filename().wstring());
                    if (off(rel)) continue;
                    rs.push_back({ de->path(), rel, m.name });
                }
            }
            std::sort(rs.begin(), rs.end(), [](const AssetSrc& a, const AssetSrc& b){ return a.rel < b.rel; });
            for (auto& r : rs) assetFiles.push_back(std::move(r));
        }
        { fs::path sceneRawDir = dir / "scene_raw";
          if (fs::is_directory(sceneRawDir, ec)) {
              std::vector<AssetSrc> srs;
              for (const auto& sf : fs::directory_iterator(sceneRawDir, ec)) {
                  if (!sf.is_regular_file()) continue;
                  std::string rel = "scene/" + sf.path().filename().generic_string();
                  if (off(rel)) continue;
                  srs.push_back({ sf.path(), rel, m.name });
              }
              std::sort(srs.begin(), srs.end(), [](const AssetSrc& a, const AssetSrc& b){ return a.rel < b.rel; });
              for (auto& s : srs) assetFiles.push_back(std::move(s)); } }
        const char* kLangSub[] = { "", "sc", "tc", "kr" };
        for (const char* Lc : kLangSub) {
            std::string lang = Lc;
            std::string pre = lang.empty() ? std::string() : ("/" + lang);
            { fs::path d = lang.empty() ? (dir / "tbl") : (dir / "tbl" / lang);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> tfs;
                  for (const auto& tf : fs::directory_iterator(d, ec))
                      if (tf.is_regular_file() && tf.path().extension() == ".json"
                          && !off("tbl" + pre + "/" + tf.path().filename().generic_string())) tfs.push_back(tf.path());
                  std::sort(tfs.begin(), tfs.end());
                  for (auto& t : tfs) { std::string rel = "tbl" + pre + "/" + t.filename().generic_string();
                      tblDirFiles.push_back({ t, entryOffSet(rel), lang, m.name }); } } }
            { fs::path d = lang.empty() ? (dir / "dat") : (dir / "dat" / lang);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> dfs;
                  for (const auto& df : fs::directory_iterator(d, ec))
                      if (df.is_regular_file() && df.path().extension() == ".dat"
                          && !off("dat" + pre + "/" + df.path().filename().generic_string())) dfs.push_back(df.path());
                  std::sort(dfs.begin(), dfs.end());
                  for (auto& dd : dfs) datReplaceFiles.push_back({ std::move(dd), m.name, lang }); } }
            { fs::path d = lang.empty() ? (dir / "table") : (dir / "table" / lang);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> tfs;
                  for (const auto& tf : fs::directory_iterator(d, ec))
                      if (tf.is_regular_file() && tf.path().extension() == ".tbl"
                          && !off("table" + pre + "/" + tf.path().filename().generic_string())) tfs.push_back(tf.path());
                  std::sort(tfs.begin(), tfs.end());
                  for (auto& t : tfs) tableReplaceFiles.push_back({ std::move(t), m.name, lang }); } }
        }
    }
    r.mods = (int)manifest.size();
    log += "[modkit] manifest: " + std::to_string(manifest.size()) + " mods (" + std::to_string(enabledMods) + " enabled)\n";
    if (patchFiles.empty() && injectFiles.empty() && tblDirFiles.empty() && datReplaceFiles.empty() && tableReplaceFiles.empty() && assetFiles.empty() && datPatchFiles.empty())
        log += "[modkit] (no enabled mod content -> producing clean/vanilla cache)\n";

    std::vector<std::string> langs;
    for (const char* L : { "sc", "tc", "kr" })
        if (fs::exists(fs::path(paths.pacSteamDir) / (std::string("table_") + L + ".pac"), ec)) langs.push_back(L);
    if (langs.empty()) langs.push_back("sc");
    std::string primaryLang = (std::find(langs.begin(), langs.end(), "sc") != langs.end()) ? "sc" : langs[0];

    std::string fpInput;
    { fpInput += "LANGS:"; for (const auto& L : langs) { fpInput += L; fpInput += ","; } fpInput += "\n"; }
    for (const auto& m : manifest) {
        fpInput += "MOD:"; fpInput += m.name; fpInput += m.enabled ? "=1" : "=0";
        for (const auto& d : m.disabled) { fpInput += ";off:"; fpInput += d; }
        fpInput += "\n";
    }
    for (const auto& pf : patchFiles) { fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : injectFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : tblDirFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : datReplaceFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : tableReplaceFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : datPatchFiles)     { fpInput += "listextraload|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& df : datFiles) { fpInput += df.generic_string(); fpInput += "\n"; fpInput += readAll(df); fpInput += "\n"; }
    for (const auto& a : assetFiles) {
        std::error_code fe; auto sz = fs::file_size(a.src, fe); auto mt = fs::last_write_time(a.src, fe);
        fpInput += a.rel; fpInput += ":"; fpInput += std::to_string((uint64_t)sz);
        fpInput += ":"; fpInput += std::to_string((long long)mt.time_since_epoch().count()); fpInput += "\n";
    }
    std::string fp = std::to_string(std::hash<std::string>{}(fpInput));
    fs::path fpFile = fs::path(paths.cacheDir) / ".fingerprint";
    if (!force && readAll(fpFile) == fp && !fp.empty()) {
        r.skipped = r.mods;
        r.log = log + "[modkit] cache up-to-date (fingerprint match), skipped " + std::to_string(r.mods) + " mod(s)";
        return r;
    }

    { std::error_code ce;
      std::set<std::string> clearDirs = { "table", "table_sc", "table_tc", "table_kr",
        "scene", "script", "script_sc", "script_tc", "script_kr", "asset" };
      for (const auto& a : assetFiles) {
          auto pos = a.rel.find('/');
          if (pos != std::string::npos) clearDirs.insert(a.rel.substr(0, pos));
      }
      for (const auto& sub : clearDirs) fs::remove_all(fs::path(paths.cacheDir) / sub, ce); }

    nlohmann::json report;
    report["version"] = 1;
    report["mods"] = nlohmann::json::array();
    report["errors"] = nlohmann::json::array();
    for (const ModInfo& m : manifest) report["mods"].push_back({ {"name", m.name}, {"enabled", m.enabled} });
    auto pushErr = [&](const std::string& where, const std::vector<std::string>& mods, const std::string& detail) {
        nlohmann::json e; e["where"] = where; e["detail"] = detail; e["mods"] = nlohmann::json::array();
        for (const auto& m : mods) e["mods"].push_back(m);
        report["errors"].push_back(std::move(e));
    };

    std::map<std::string, PatchConfig> byTarget;
    std::map<std::string, std::set<std::string>> targetMods;
    for (const auto& pf : patchFiles) {
        const std::string& mod = pf.mod;
        PatchConfig cfg; std::string err;
        if (!LoadPatchConfig(pf.path.wstring(), cfg, err)) {
            ++r.failed; log += "[modkit] load FAIL " + pf.path.generic_string() + ": " + err + "\n";
            pushErr("scene_add_json/" + pf.path.filename().generic_string(), { mod }, err); continue;
        }
        if (cfg.target.empty()) cfg.target = pf.path.stem().string();
        PatchConfig& m = byTarget[cfg.target];
        m.target = cfg.target;
        if (m.map.empty()) m.map = cfg.map;
        for (auto& a : cfg.addActors) m.addActors.push_back(a);
        targetMods[cfg.target].insert(mod);
    }

    FpacReader pacScene;
    bool sceneOk = true;
    if (!byTarget.empty() && !pacScene.Open(fs::path(paths.pacSteamDir) / "scene.pac")) {
        sceneOk = false; ++r.failed; log += "[modkit] cannot open scene.pac\n";
        pushErr("scene.pac", {}, "无法打开 scene.pac(检查游戏 pac\\steam)");
    }

    std::vector<LpRow> allHooks;
    for (auto& kv : byTarget) {
        if (!sceneOk) break;
        const std::string& target = kv.first;
        std::vector<std::string> tmods(targetMods[target].begin(), targetMods[target].end());
        const std::string internal = "scene/" + target + ".json";
        std::vector<uint8_t> orig;
        if (!pacScene.ReadEntry(internal, orig)) { ++r.failed; log += "[modkit] no pac entry " + internal + "\n"; pushErr("scene/" + target, tmods, "pac 内无此 scene(target 名错?)"); continue; }
        SceneMergeResult mr = MergeScene(orig, kv.second);
        if (!mr.ok) { ++r.failed; log += "[modkit] merge FAIL " + target + ": " + mr.err + "\n"; pushErr("scene/" + target, tmods, mr.err); continue; }
        if (!writeAll(fs::path(paths.cacheDir) / "scene" / (target + ".json"), mr.sceneBytes)) { ++r.failed; log += "[modkit] write FAIL " + target + "\n"; pushErr("scene/" + target, tmods, "写缓存失败"); continue; }
        for (auto& h : mr.tblHooks) allHooks.push_back(h);
        ++r.merged;
        log += "[modkit] merged scene/" + target + ".json (+" + std::to_string(mr.tblHooks.size()) + " lookpoint hooks)\n";
    }

    struct TblAccum { std::string table; std::vector<nlohmann::json> addRows; std::vector<tbl_merge::EditOp> edits; std::vector<nlohmann::json> cloneRows; std::set<std::string> mods; };
    report["tables"] = nlohmann::json::array();

    if (!allHooks.empty() || !tblDirFiles.empty()) {
        for (const std::string& L : langs) {
            const bool isPrimary = (L == primaryLang);
            std::map<std::string, TblAccum> tblAcc;
            for (const LpRow& h : allHooks) {
                nlohmann::json row;
                row["text1"] = h.text1; row["text2"] = h.text2; row["text3"] = h.text3; row["empty"] = h.empty;
                row["arr1"] = h.arr1; row["uint1"] = h.uint1; row["arr2"] = h.arr2; row["uint2"] = h.uint2;
                tblAcc["t_lookpoint"].addRows.push_back(std::move(row));
            }
            for (const auto& tf : tblDirFiles) {
                if (!(tf.lang.empty() || tf.lang == L)) continue;
                std::string stem = tf.path.stem().string();
                const std::string& modName = tf.mod;
                TblAccum& a = tblAcc[stem];
                parseTblFile(tf.path, a.table, a.addRows, a.edits, a.cloneRows, modName, tf.off, log);
                a.mods.insert(modName);
            }
            if (tblAcc.empty()) continue;

            const std::string pacName  = "table_" + L + ".pac";
            const std::string entryPre = "table_" + L + "/";
            const std::string outSub   = "table_" + L;

            FpacReader pacTable;
            if (!pacTable.Open(fs::path(paths.pacSteamDir) / pacName)) {
                ++r.failed; log += "[modkit] cannot open " + pacName + "\n";
                if (isPrimary) pushErr(pacName, {}, "无法打开 " + pacName + "(检查游戏 pac\\steam)");
                continue;
            }
            for (auto& kv : tblAcc) {
                const std::string& stem = kv.first;
                std::vector<std::string> tmods(kv.second.mods.begin(), kv.second.mods.end());
                std::vector<uint8_t> origTbl;
                if (!pacTable.ReadEntry(entryPre + stem + ".tbl", origTbl)) {
                    ++r.failed; log += "[modkit] no pac entry " + entryPre + stem + ".tbl\n";
                    if (isPrimary) pushErr(stem + ".tbl", tmods, "pac 内无此表(表名/文件名错?)"); continue;
                }
                std::vector<uint8_t> outBytes; std::string e2;
                std::vector<tbl_merge::TblConflict> conflicts;
                bool okTbl;
                if (!kv.second.cloneRows.empty()) {
                    if (!kv.second.addRows.empty() || !kv.second.edits.empty()) {
                        ++r.failed; log += "[modkit] tbl FAIL " + stem + ": clone_rows 不能与 add_rows/edit_rows 混用\n"; if (isPrimary) pushErr(stem + ".tbl", tmods, "clone_rows 不能与 add_rows/edit_rows 混用"); continue;
                    }
                    okTbl = tbl_merge::CloneRowsPoolTable(origTbl, paths.schemasDir, "Sora1", kv.second.table, kv.second.cloneRows, outBytes, e2);
                    if (!okTbl) { ++r.failed; log += "[modkit] tbl clone FAIL " + stem + ": " + e2 + "\n"; if (isPrimary) pushErr(stem + ".tbl", tmods, e2); continue; }
                } else {
                    if (tbl_merge::TblHasUnmodeledPool(origTbl, paths.schemasDir, "Sora1")) {
                        ++r.failed; log += "[modkit] tbl FAIL " + stem + ": 该表带数据池,add_rows/edit_rows 会损坏;请用 clone_rows 或 table\\ 整文件替换\n";
                        if (isPrimary) pushErr(stem + ".tbl", tmods, "带数据池的表,请用 clone_rows 或 table\\ 整文件替换(add_rows/edit_rows 会损坏)"); continue;
                    }
                    okTbl = tbl_merge::ApplyTblPatch(origTbl, paths.schemasDir, "Sora1", kv.second.table, kv.second.addRows, kv.second.edits, outBytes, e2, &conflicts);
                    if (!okTbl) { ++r.failed; log += "[modkit] tbl FAIL " + stem + ": " + e2 + "\n"; if (isPrimary) pushErr(stem + ".tbl", tmods, e2); continue; }
                }
                if (writeAll(fs::path(paths.cacheDir) / outSub / (stem + ".tbl"), outBytes)) {
                    if (isPrimary) ++r.tbls;
                    log += "[modkit] merged " + outSub + "/" + stem + ".tbl (+" + std::to_string(kv.second.addRows.size()) + " rows, " + std::to_string(kv.second.edits.size()) + " edits, " + std::to_string(kv.second.cloneRows.size()) + " clones)\n";
                } else { ++r.failed; log += "[modkit] write FAIL " + outSub + "/" + stem + "\n"; if (isPrimary) pushErr(stem + ".tbl", tmods, "写缓存失败"); continue; }

                if (!isPrimary) continue;
                nlohmann::json tj;
                tj["table"] = stem;
                tj["mods"] = nlohmann::json::array();
                for (const auto& mname : kv.second.mods) tj["mods"].push_back(mname);
                tj["addRows"] = (int)kv.second.addRows.size();
                tj["editRows"] = (int)kv.second.edits.size();
                tj["cloneRows"] = (int)kv.second.cloneRows.size();
                tj["conflicts"] = nlohmann::json::array();
                for (const auto& c : conflicts) {
                    ++r.conflicts;
                    tj["conflicts"].push_back({ {"row", c.row}, {"field", c.field},
                        {"old", c.oldValue}, {"new", c.newValue}, {"fromMod", c.fromMod}, {"byMod", c.byMod} });
                    log += "[modkit] CONFLICT " + stem + "[" + c.row + "]." + c.field + ": "
                         + c.oldValue + " (" + c.fromMod + ") -> " + c.newValue + " (" + c.byMod + ") [winner]\n";
                }
                report["tables"].push_back(std::move(tj));
            }
        }
    }

    {
        std::string table = "# auto-generated by modkit: <map>\\t<script>\n";
        std::set<std::string> listSeen;
        report["inject"] = nlohmann::json::array();
        for (const auto& jf : injectFiles) {
            fs::path moddir = jf.path.parent_path();
            for (const auto& e : parseScriptInject(jf.path, jf.mod, jf.lang, log)) {
                bool have = false;
                std::string s = readAll(e.datPath);
                if (!s.empty()) {
                    std::vector<uint8_t> bytes(s.begin(), s.end());
                    if (writeAll(fs::path(paths.cacheDir) / "script" / (e.script + ".dat"), bytes)) have = true;
                }
                if (fs::exists(moddir / "dat" / (e.script + ".dat"))) have = true;
                for (const char* L : { "sc", "tc", "kr" })
                    if (fs::exists(moddir / "dat" / L / (e.script + ".dat"))) have = true;
                if (!have) {
                    log += "[modkit] add_dat_ini WARN: \"" + e.script + "\" 无对话 dat(放 Mod\\" + e.mod + "\\dat\\<lang>\\" + e.script + ".dat)\n";
                    pushErr("add_dat_ini \"" + e.script + "\"", { e.mod }, "缺对话 dat:" + e.script + ".dat");
                    continue;
                }
                ++r.injected;
                std::string key = e.map + "\t" + e.script;
                if (listSeen.insert(key).second) { table += key; table += "\n"; }
                report["inject"].push_back({ {"map", e.map}, {"script", e.script}, {"mod", e.mod} });
                log += "[modkit] add_dat_ini " + e.mod + ": map \"" + e.map + "\" -> \"" + e.script + ".dat\"\n";
            }
        }
        fs::path tablePath = fs::path(paths.cacheDir).parent_path() / "script_inject.list";
        writeText(tablePath, table);
    }

    {
        std::string table = "# auto-generated by modkit: <map>\\t<t_mon table>\n";
        std::set<std::string> seen;
        report["monLoad"] = nlohmann::json::array();
        for (const auto& jf : injectFiles) {
            for (const auto& e : parseMonLoad(jf.path, jf.mod, log)) {
                std::string key = e.map + "\t" + e.table;
                if (seen.insert(key).second) { table += key; table += "\n"; }
                report["monLoad"].push_back({ {"map", e.map}, {"table", e.table}, {"mod", e.mod} });
                log += "[modkit] mon_load " + e.mod + ": map \"" + e.map + "\" -> 载入怪物表 \"" + e.table + "\"\n";
            }
        }
        writeText(fs::path(paths.cacheDir).parent_path() / "mon_load.list", table);
    }

    {
        std::string table = "# auto-generated by modkit: <map>\\t<event function>\n";
        std::set<std::string> seen;
        report["monEvent"] = nlohmann::json::array();
        for (const auto& jf : injectFiles) {
            for (const auto& e : parseMonEvent(jf.path, jf.mod, log)) {
                std::string key = e.map + "\t" + e.event;
                if (seen.insert(key).second) { table += key; table += "\n"; }
                report["monEvent"].push_back({ {"map", e.map}, {"event", e.event}, {"mod", e.mod} });
                log += "[modkit] mon_event " + e.mod + ": map \"" + e.map + "\" -> spawn event \"" + e.event + "\"\n";
            }
        }
        writeText(fs::path(paths.cacheDir).parent_path() / "mon_event.list", table);
    }

    {
        report["datReplace"] = nlohmann::json::array();
        report["datConflicts"] = nlohmann::json::array();
        struct Prov { std::string mod; fs::path path; };
        std::map<std::string, std::vector<Prov>> byKey;
        std::vector<std::string> order;
        for (const auto& df : datReplaceFiles) {
            std::string stem = df.path.stem().string();
            std::string key = df.lang + "/" + stem;
            if (byKey.find(key) == byKey.end()) order.push_back(key);
            byKey[key].push_back({ df.mod, df.path });
        }
        for (const std::string& key : order) {
            auto& provs = byKey[key];
            std::string lang = key.substr(0, key.find('/'));
            std::string stem = key.substr(key.find('/') + 1);
            std::string sub = lang.empty() ? "script" : ("script_" + lang);
            std::string tag = lang.empty() ? "" : ("[" + lang + "]");
            const auto& win = provs.back();
            std::string s = readAll(win.path);
            if (s.empty()) { ++r.failed; log += "[modkit] dat_replace empty/missing: " + win.path.generic_string() + "\n"; pushErr("dat 替换 \"" + stem + "\"" + tag, { win.mod }, "dat 为空/读取失败"); continue; }
            std::vector<uint8_t> bytes(s.begin(), s.end());
            fs::path dst = fs::path(paths.cacheDir) / sub / (stem + ".dat");
            if (!writeAll(dst, bytes)) { ++r.failed; log += "[modkit] dat_replace copy FAIL " + dst.generic_string() + "\n"; pushErr("dat 替换 \"" + stem + "\"" + tag, { win.mod }, "写缓存失败"); continue; }
            nlohmann::json di; di["name"] = stem; di["lang"] = lang; di["winner"] = win.mod; di["mods"] = nlohmann::json::array();
            for (auto& p : provs) di["mods"].push_back(p.mod);
            report["datReplace"].push_back(std::move(di));
            log += "[modkit] dat_replace " + sub + "/" + stem + ".dat <- " + win.mod + (provs.size() > 1 ? " (覆盖其它)" : "") + "\n";
            for (size_t i = 0; i + 1 < provs.size(); ++i) {
                ++r.conflicts;
                report["datConflicts"].push_back({ {"name", stem}, {"lang", lang}, {"fromMod", provs[i].mod}, {"byMod", win.mod} });
                log += "[modkit] CONFLICT dat " + sub + "/" + stem + ".dat: " + provs[i].mod + " -> " + win.mod + " [winner,整文件]\n";
            }
        }
    }

    {
        report["listExtraLoad"] = nlohmann::json::array();
        struct Acc { std::vector<DatInsertCall> inserts; std::set<std::string> mods; };
        std::map<std::string, Acc> byTarget;
        std::vector<std::string> targets;
        for (const auto& pf : datPatchFiles) {
            DatPatchConfig cfg; std::string e;
            if (!LoadDatPatchConfig(pf.path.wstring(), cfg, e)) {
                ++r.failed;
                log += "[modkit] ListExtraLoad 读取失败 " + pf.path.filename().generic_string() + ": " + e + "\n";
                pushErr("ListExtraLoad \"" + pf.path.stem().string() + "\"", { pf.mod }, e);
                continue;
            }
            std::string target = cfg.target.empty() ? pf.path.stem().string() : cfg.target;
            if (byTarget.find(target) == byTarget.end()) targets.push_back(target);
            Acc& a = byTarget[target];
            for (auto& ic : cfg.inserts) a.inserts.push_back(std::move(ic));
            a.mods.insert(pf.mod);
        }
        for (const std::string& target : targets) {
            Acc& acc = byTarget[target];
            std::vector<std::string> tmods(acc.mods.begin(), acc.mods.end());
            int done = 0; std::string lastErr;
            for (const std::string& L : langs) {
                FpacReader pacScript;
                if (!pacScript.Open(fs::path(paths.pacSteamDir) / ("script_" + L + ".pac"))) continue;
                fs::path dst = fs::path(paths.cacheDir) / ("script_" + L) / (target + ".dat");
                std::vector<uint8_t> orig;
                std::string cached = readAll(dst);
                if (!cached.empty()) orig.assign(cached.begin(), cached.end());
                else if (!pacScript.ReadEntry("script_" + L + "/scena/" + target + ".dat", orig)) continue;
                std::vector<uint8_t> outBytes;
                if (!ApplyDatPatch(orig, acc.inserts, outBytes, lastErr)) break;
                if (!writeAll(dst, outBytes)) { lastErr = "写缓存失败"; break; }
                ++done;
            }
            if (done == 0) {
                ++r.failed;
                if (lastErr.empty()) lastErr = "任何语言的 script pac 里都没有 " + target + ".dat";
                log += "[modkit] ListExtraLoad FAIL " + target + ": " + lastErr + "\n";
                pushErr("ListExtraLoad \"" + target + "\"", tmods, lastErr);
                continue;
            }
            ++r.merged;
            report["listExtraLoad"].push_back({ {"name", target}, {"inserts", acc.inserts.size()},
                                           {"langs", done}, {"mods", tmods} });
            log += "[modkit] ListExtraLoad " + target + ".dat (+" + std::to_string(acc.inserts.size())
                 + " 处插入, " + std::to_string(done) + " 个语言)\n";
        }
    }

    {
        report["tableReplace"] = nlohmann::json::array();
        struct Prov { std::string mod; fs::path path; };
        std::map<std::string, std::vector<Prov>> byKey;
        std::vector<std::string> order;
        for (const auto& tf : tableReplaceFiles) {
            std::string stem = tf.path.stem().string();
            std::string key = tf.lang + "/" + stem;
            if (byKey.find(key) == byKey.end()) order.push_back(key);
            byKey[key].push_back({ tf.mod, tf.path });
        }
        for (const std::string& key : order) {
            auto& provs = byKey[key];
            std::string lang = key.substr(0, key.find('/'));
            std::string stem = key.substr(key.find('/') + 1);
            std::string sub = lang.empty() ? "table" : ("table_" + lang);
            std::string tag = lang.empty() ? "" : ("[" + lang + "]");
            const auto& win = provs.back();
            std::string s = readAll(win.path);
            if (s.empty()) { ++r.failed; log += "[modkit] table_replace empty/missing: " + win.path.generic_string() + "\n"; pushErr("table 替换 \"" + stem + "\"" + tag, { win.mod }, "tbl 为空/读取失败"); continue; }
            std::vector<uint8_t> bytes(s.begin(), s.end());
            if (!writeAll(fs::path(paths.cacheDir) / sub / (stem + ".tbl"), bytes)) { ++r.failed; log += "[modkit] table_replace copy FAIL " + stem + "\n"; pushErr("table 替换 \"" + stem + "\"" + tag, { win.mod }, "写缓存失败"); continue; }
            ++r.tbls;
            report["tableReplace"].push_back({ {"name", stem}, {"lang", lang}, {"winner", win.mod} });
            log += "[modkit] table_replace " + sub + "/" + stem + ".tbl <- " + win.mod + (provs.size() > 1 ? " (覆盖其它)" : "") + "\n";
            for (size_t i = 0; i + 1 < provs.size(); ++i) {
                ++r.conflicts;
                report["datConflicts"].push_back({ {"name", stem + ".tbl"}, {"lang", lang}, {"fromMod", provs[i].mod}, {"byMod", win.mod} });
                log += "[modkit] CONFLICT table " + sub + "/" + stem + ".tbl: " + provs[i].mod + " -> " + win.mod + " [winner,整文件]\n";
            }
        }
    }

    {
        report["assets"] = nlohmann::json::array();
        report["assetConflicts"] = nlohmann::json::array();
        std::map<std::string, std::vector<std::pair<std::string, fs::path>>> byRel;
        std::vector<std::string> order;
        for (const auto& a : assetFiles) {
            if (byRel.find(a.rel) == byRel.end()) order.push_back(a.rel);
            byRel[a.rel].push_back({ a.mod, a.src });
        }
        for (const std::string& rel : order) {
            auto& provs = byRel[rel];
            const auto& win = provs.back();
            fs::path dst = fs::path(paths.cacheDir) / fs::path(rel);
            std::error_code ce; fs::create_directories(dst.parent_path(), ce);
            fs::copy_file(win.second, dst, fs::copy_options::overwrite_existing, ce);
            if (ce) { ++r.failed; log += "[modkit] asset copy FAIL " + rel + ": " + ce.message() + "\n"; pushErr("asset \"" + rel + "\"", { win.first }, "复制失败: " + ce.message()); continue; }
            ++r.assets;
            report["assets"].push_back({ {"name", rel}, {"winner", win.first} });
            log += "[modkit] asset " + rel + " <- " + win.first + (provs.size() > 1 ? " (覆盖其它)" : "") + "\n";
            for (size_t i = 0; i + 1 < provs.size(); ++i) {
                ++r.conflicts;
                report["assetConflicts"].push_back({ {"name", rel}, {"fromMod", provs[i].first}, {"byMod", win.first} });
                log += "[modkit] CONFLICT asset " + rel + ": " + provs[i].first + " -> " + win.first + " [winner,整文件]\n";
            }
        }
    }

    report["summary"] = { {"merged", r.merged}, {"tbls", r.tbls}, {"injected", r.injected},
                          {"assets", r.assets}, {"failed", r.failed}, {"conflicts", r.conflicts} };
    writeText(fs::path(paths.cacheDir).parent_path() / "merge_report.json",
              report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
    writeText(fpFile, fp);
    log += "[modkit] done: merged=" + std::to_string(r.merged) + " tbls=" + std::to_string(r.tbls)
        + " injected=" + std::to_string(r.injected) + " assets=" + std::to_string(r.assets)
        + " failed=" + std::to_string(r.failed)
        + " conflicts=" + std::to_string(r.conflicts) + " mods=" + std::to_string(r.mods);
    r.log = log;
    return r;
}

}
}
}
