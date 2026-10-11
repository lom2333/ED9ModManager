#include "modkit/mod_merge_orchestrator.h"
#include "modkit/patch_config.h"
#include "modkit/dat_patch.h"
#include "modkit/scene_merge.h"
#include "modkit/tbl_codec.h"
#include "modkit/tbl_merge.h"
#include "modkit/generic_tbl.h"
#include "modkit/fpac_reader.h"
#include "modkit/mod_archive.h"
#include "modkit/dds_crypt.h"
#include "json.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>
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

static bool composeDds(const std::wstring& pacSteamDir, const std::string& rel,
                       const std::vector<std::pair<std::string, fs::path>>& provs,
                       std::vector<uint8_t>& out, std::string& why) {
    if (rel.rfind("asset/dx11/image/", 0) != 0) { why = "不在 asset/dx11/image 下"; return false; }
    FpacReader pac;
    std::vector<uint8_t> raw, base;
    if (!pac.Open(fs::path(pacSteamDir) / "image.pac") || !pac.ReadEntry(rel, raw)) { why = "image.pac 里没有原版"; return false; }
    std::string err;
    if (!ddscrypt::Decrypt(raw, base, err)) { why = "原版解不开(" + err + ")"; return false; }
    const ddscrypt::DdsInfo bi = ddscrypt::Inspect(base.data(), base.size());
    const size_t head = (bi.fourcc == 0x30315844u) ? 148 : 128;
    if (!bi.ok || base.size() <= head || (base.size() - head) % 16 != 0) { why = "原版的头 / 数据长度不认识"; return false; }
    const size_t units = (base.size() - head) / 16;
    std::vector<std::vector<uint8_t>> mods;
    for (const auto& p : provs) {
        const std::string s = readAll(p.second);
        std::vector<uint8_t> in(s.begin(), s.end()), d;
        if (!ddscrypt::Decrypt(in, d, err)) { why = p.first + " 那份解不开(" + err + ")"; return false; }
        if (d.size() != base.size() || std::memcmp(d.data(), base.data(), head) != 0) { why = p.first + " 那份和原版尺寸 / 格式不同"; return false; }
        mods.push_back(std::move(d));
    }
    std::vector<int> owner(units, -1);
    for (size_t i = 0; i < mods.size(); ++i) {
        const uint8_t* m = mods[i].data() + head;
        const uint8_t* b = base.data() + head;
        for (size_t u = 0; u < units; ++u) {
            if (std::memcmp(m + u * 16, b + u * 16, 16) == 0) continue;
            if (owner[u] >= 0 && std::memcmp(m + u * 16, mods[owner[u]].data() + head + u * 16, 16) != 0) {
                why = provs[owner[u]].first + " 和 " + provs[i].first + " 改了同一处";
                return false;
            }
            owner[u] = static_cast<int>(i);
        }
    }
    std::vector<uint8_t> plain = base;
    for (size_t u = 0; u < units; ++u) {
        if (owner[u] >= 0) std::memcpy(plain.data() + head + u * 16, mods[owner[u]].data() + head + u * 16, 16);
    }
    if (!ddscrypt::Encrypt(plain, out, err)) { why = "合成后套 LZ4 帧失败(" + err + ")"; return false; }
    return true;
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

static std::string tblTargetOf(const fs::path& p) {
    const std::string s = p.stem().string();
    const size_t dot = s.find('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

static bool entryOff(const std::set<std::string>& off, const char* sec, size_t idx) {
    return !off.empty() && off.count(std::string(sec) + "/" + std::to_string(idx)) > 0;
}

static void parseTblFile(const fs::path& p, std::string& outTable,
                         std::vector<nlohmann::json>& addRows,
                         std::vector<tbl_merge::EditOp>& edits,
                         std::vector<nlohmann::json>& cloneRows,
                         const std::string& modName, const std::set<std::string>& off, std::string& log,
                         std::vector<std::string>* addMods = nullptr) {
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
            if (row.is_object() && !entryOff(off, "add_rows", i)) {
                nlohmann::json r = row; aliasLp(r); addRows.push_back(std::move(r));
                if (addMods) addMods->push_back(modName);
            }
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

struct GameLang { std::string code, tablePac, tablePre, scriptPac, scriptPre; };
static std::vector<GameLang> DetectGameLangs(const fs::path& pacDir) {
    std::vector<GameLang> out;
    std::error_code ec;
    for (const char* c : { "sc", "tc", "jp", "ko", "kr", "en", "de", "fr", "es" }) {
        const std::string code = c;
        if (code == "jp") {
            if (fs::exists(pacDir / "table.pac", ec) && fs::exists(pacDir / "script.pac", ec))
                out.push_back({ "jp", "table.pac", "table/", "script.pac", "script/" });
        } else if (fs::exists(pacDir / ("table_" + code + ".pac"), ec)) {
            out.push_back({ code, "table_" + code + ".pac", "table_" + code + "/", "script_" + code + ".pac", "script_" + code + "/" });
        }
    }
    return out;
}
static const char* const kModLangDirs[] = { "jp", "sc", "tc", "kr", "ko", "en", "de", "fr", "es" };
static std::string ModLangCode(const std::string& dir, const std::vector<GameLang>& langs) {
    auto has = [&](const char* c) { for (const auto& l : langs) if (l.code == c) return true; return false; };
    if (dir == "kr" || dir == "ko") return has("ko") ? "ko" : (has("kr") ? "kr" : "");
    return has(dir.c_str()) ? dir : "";
}

RunResult Run(const Paths& paths, bool force) {
    RunResult r;
    std::string log;

    std::error_code ec;
    archive::EnsureAllStaged(fs::path(paths.modsDir), log);
    std::vector<ModInfo> manifest = LoadModManifest(paths.modsDir, log);
    std::vector<fs::path> datFiles;
    std::vector<PathSrc> patchFiles, injectFiles, datReplaceFiles, tableReplaceFiles;
    std::vector<PathSrc> aiFiles;
    std::vector<PathSrc> aniFiles;
    std::vector<PathSrc> datPatchFiles;
    std::vector<PathSrc> modJsons;
    std::vector<TblSrc> tblDirFiles;
    std::vector<AssetSrc> assetFiles;
    std::vector<GameLang> langs = DetectGameLangs(paths.pacSteamDir);
    if (langs.empty()) langs.push_back({ "sc", "table_sc.pac", "table_sc/", "script_sc.pac", "script_sc/" });
    const std::string primaryLang = langs[0].code;
    std::map<std::string, std::set<std::string>> modLangs;
    auto modHas = [&](const std::string& mod, const std::string& code) {
        auto it = modLangs.find(mod);
        return it != modLangs.end() && it->second.count(code) > 0;
    };
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
        { fs::path mj = dir / "mod.json";
          if (fs::exists(mj, ec)) modJsons.push_back({ mj, m.name, "" }); }
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
        { fs::path vfxDir = dir / "vfx";
          if (fs::is_directory(vfxDir, ec)) {
              std::vector<AssetSrc> vs;
              for (auto it = fs::recursive_directory_iterator(vfxDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                  if (ec) break;
                  if (!it->is_regular_file(ec) || _wcsicmp(it->path().extension().c_str(), L".vfx") != 0) continue;
                  std::string rel = "vfx/" + wtou8(fs::relative(it->path(), vfxDir, ec).generic_wstring());
                  if (off(rel)) continue;
                  vs.push_back({ it->path(), rel, m.name });
              }
              std::sort(vs.begin(), vs.end(), [](const AssetSrc& a, const AssetSrc& b){ return a.rel < b.rel; });
              for (auto& v : vs) assetFiles.push_back(std::move(v)); } }
        {
            static const std::pair<const char*, const wchar_t*> kLangChannels[] = {
                { "tbl", L".json" }, { "dat", L".dat" }, { "ai", L".dat" }, { "ani", L".dat" }, { "table", L".tbl" } };
            bool hasLangDir = false;
            std::set<std::string> ml;
            for (const auto& chn : kLangChannels) {
                const fs::path chDir = dir / chn.first;
                if (!fs::is_directory(chDir, ec)) continue;
                for (const auto& sd : fs::directory_iterator(chDir, ec)) {
                    std::error_code e2;
                    if (!sd.is_directory(e2)) continue;
                    bool anyFile = false;
                    for (const auto& f : fs::directory_iterator(sd.path(), e2))
                        if (f.is_regular_file(e2) && f.path().extension() == chn.second) { anyFile = true; break; }
                    if (!anyFile) continue;
                    const std::string dn = wtou8(sd.path().filename().wstring());
                    if (std::find(std::begin(kModLangDirs), std::end(kModLangDirs), dn) == std::end(kModLangDirs)) {
                        log += "[modkit] " + m.name + ":" + chn.first + "\\" + dn + "\\ 不是认得的语言子目录(jp / sc / tc / ko / kr / en / de / fr / es),没用上\n";
                        continue;
                    }
                    hasLangDir = true;
                    const std::string code = ModLangCode(dn, langs);
                    if (code.empty()) { log += "[modkit] " + m.name + ":" + chn.first + "\\" + dn + "\\ 本作没有这种语言,没用上\n"; continue; }
                    ml.insert(code);
                }
            }
            if (!hasLangDir) for (const auto& GL : langs) ml.insert(GL.code);
            modLangs[m.name] = std::move(ml);
        }
        std::vector<std::string> langFolders = { "" };
        for (const char* d : kModLangDirs) langFolders.push_back(d);
        for (const std::string& folder : langFolders) {
            const std::string lang = folder.empty() ? std::string() : ModLangCode(folder, langs);
            if (!folder.empty() && lang.empty()) continue;
            const std::string pre = folder.empty() ? std::string() : ("/" + folder);
            { fs::path d = folder.empty() ? (dir / "tbl") : (dir / "tbl" / folder);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> tfs;
                  for (const auto& tf : fs::directory_iterator(d, ec))
                      if (tf.is_regular_file() && tf.path().extension() == ".json"
                          && !off("tbl" + pre + "/" + tf.path().filename().generic_string())) tfs.push_back(tf.path());
                  std::sort(tfs.begin(), tfs.end());
                  for (auto& t : tfs) { std::string rel = "tbl" + pre + "/" + t.filename().generic_string();
                      tblDirFiles.push_back({ t, entryOffSet(rel), lang, m.name }); } } }
            { fs::path d = folder.empty() ? (dir / "dat") : (dir / "dat" / folder);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> dfs;
                  for (const auto& df : fs::directory_iterator(d, ec))
                      if (df.is_regular_file() && df.path().extension() == ".dat"
                          && !off("dat" + pre + "/" + df.path().filename().generic_string())) dfs.push_back(df.path());
                  std::sort(dfs.begin(), dfs.end());
                  for (auto& dd : dfs) datReplaceFiles.push_back({ std::move(dd), m.name, lang }); } }
            { fs::path d = folder.empty() ? (dir / "ai") : (dir / "ai" / folder);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> afs;
                  for (const auto& af : fs::directory_iterator(d, ec))
                      if (af.is_regular_file() && af.path().extension() == ".dat"
                          && !off("ai" + pre + "/" + af.path().filename().generic_string())) afs.push_back(af.path());
                  std::sort(afs.begin(), afs.end());
                  for (auto& a : afs) aiFiles.push_back({ std::move(a), m.name, lang }); } }
            { fs::path d = folder.empty() ? (dir / "ani") : (dir / "ani" / folder);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> nfs;
                  for (const auto& nf : fs::directory_iterator(d, ec))
                      if (nf.is_regular_file() && nf.path().extension() == ".dat"
                          && !off("ani" + pre + "/" + nf.path().filename().generic_string())) nfs.push_back(nf.path());
                  std::sort(nfs.begin(), nfs.end());
                  for (auto& n : nfs) aniFiles.push_back({ std::move(n), m.name, lang }); } }
            { fs::path d = folder.empty() ? (dir / "table") : (dir / "table" / folder);
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
    if (patchFiles.empty() && injectFiles.empty() && tblDirFiles.empty() && datReplaceFiles.empty() && tableReplaceFiles.empty() && assetFiles.empty() && datPatchFiles.empty() && aiFiles.empty() && aniFiles.empty())
        log += "[modkit] (no enabled mod content -> producing clean/vanilla cache)\n";

    std::string fpInput;
    fpInput += "OUTPUTS:ai,mod_chars,mod_chars_json,all_langs\n";
    fpInput += "SCHEMA:" + std::to_string(TblSchemaFingerprint()) + "\n";
    { fpInput += "LANGS:"; for (const auto& GL : langs) { fpInput += GL.code; fpInput += ","; } fpInput += "\n"; }
    for (const auto& m : manifest) {
        fpInput += "MOD:"; fpInput += m.name; fpInput += m.enabled ? "=1" : "=0";
        for (const auto& d : m.disabled) { fpInput += ";off:"; fpInput += d; }
        fpInput += "\n";
    }
    for (const auto& pf : patchFiles) { fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : injectFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : tblDirFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : datReplaceFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : aiFiles)         { fpInput += "ai|"; fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : aniFiles)        { fpInput += "ani|"; fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : tableReplaceFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : datPatchFiles)     { fpInput += "listextraload|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : modJsons)          { fpInput += "modjson|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
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
      std::set<std::string> clearDirs = { "scene", "asset", "vfx", "voice", "se", "bgm1", "bgm2", "bgm3" };
      for (const auto& e : fs::directory_iterator(paths.cacheDir, ce)) {
          std::error_code e2;
          if (!e.is_directory(e2)) continue;
          const std::string n = wtou8(e.path().filename().wstring());
          if (n == "table" || n == "script" || n.rfind("table_", 0) == 0 || n.rfind("script_", 0) == 0) clearDirs.insert(n);
      }
      ce.clear();
      for (const auto& a : assetFiles) {
          auto pos = a.rel.find('/');
          if (pos != std::string::npos) clearDirs.insert(a.rel.substr(0, pos));
      }
      for (const auto& sub : clearDirs) fs::remove_all(fs::path(paths.cacheDir) / u8tow(sub), ce); }

    nlohmann::json report;
    report["version"] = 1;
    report["mods"] = nlohmann::json::array();
    report["errors"] = nlohmann::json::array();
    for (const ModInfo& m : manifest) report["mods"].push_back({ {"name", m.name}, {"enabled", m.enabled} });
    auto pushErr = [&](const std::string& where, const std::vector<std::string>& mods, const std::string& detail,
                       const std::set<std::string>& langsOf = {}) {
        nlohmann::json e; e["where"] = where; e["detail"] = detail; e["mods"] = nlohmann::json::array();
        for (const auto& m : mods) e["mods"].push_back(m);
        if (!langsOf.empty() && langsOf.size() < langs.size()) {
            e["langs"] = nlohmann::json::array();
            for (const auto& GL : langs) if (langsOf.count(GL.code)) e["langs"].push_back(GL.code);
        }
        report["errors"].push_back(std::move(e));
    };
    struct LangErr { std::string where, detail; std::vector<std::string> mods; std::set<std::string> langs; };
    auto addLangErr = [&](std::vector<LangErr>& errs, const std::string& where, const std::vector<std::string>& mods,
                          const std::string& detail, const std::string& L) {
        for (auto& e : errs) {
            if (e.where != where || e.detail != detail) continue;
            e.langs.insert(L);
            for (const auto& m : mods) if (std::find(e.mods.begin(), e.mods.end(), m) == e.mods.end()) e.mods.push_back(m);
            return;
        }
        errs.push_back({ where, detail, mods, { L } });
        ++r.failed;
    };
    auto flushLangErrs = [&](std::vector<LangErr>& errs) {
        for (const auto& e : errs) pushErr(e.where, e.mods, e.detail, e.langs);
        errs.clear();
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

    struct TblAccum { std::string table; std::vector<nlohmann::json> addRows; std::vector<std::string> addMods; std::vector<tbl_merge::EditOp> edits; std::vector<nlohmann::json> cloneRows; };
    struct TblGroup { std::vector<TblAccum> subs; std::set<std::string> mods; };
    auto addToGroup = [](TblGroup& g, TblAccum&& one) {
        TblAccum* a = nullptr;
        if (one.table.empty()) {
            if (!g.subs.empty()) a = &g.subs[0];
        } else {
            for (auto& s : g.subs) if (s.table == one.table) { a = &s; break; }
            if (!a) for (auto& s : g.subs) if (s.table.empty()) { s.table = one.table; a = &s; break; }
        }
        if (!a) { g.subs.push_back(std::move(one)); return; }
        for (auto& x : one.addRows) a->addRows.push_back(std::move(x));
        for (auto& x : one.addMods) a->addMods.push_back(std::move(x));
        for (auto& x : one.edits) a->edits.push_back(std::move(x));
        for (auto& x : one.cloneRows) a->cloneRows.push_back(std::move(x));
    };
    report["tables"] = nlohmann::json::array();
    std::vector<nlohmann::json> charAdds;
    std::vector<std::string> charAddMods;
    std::vector<uint8_t> charNameTbl, charStatusTbl;
    std::vector<nlohmann::json> itemAdds;
    std::vector<std::string> itemAddMods;
    std::vector<uint8_t> itemTbl;

    std::vector<LangErr> tblErrs;
    std::set<std::string> reportedStems;
    if (!allHooks.empty() || !tblDirFiles.empty()) {
        for (const GameLang& GL : langs) {
            const std::string& L = GL.code;
            const bool isPrimary = (L == primaryLang);
            std::map<std::string, TblGroup> tblAcc;
            for (const LpRow& h : allHooks) {
                nlohmann::json row;
                row["text1"] = h.text1; row["text2"] = h.text2; row["text3"] = h.text3; row["empty"] = h.empty;
                row["arr1"] = h.arr1; row["uint1"] = h.uint1; row["arr2"] = h.arr2; row["uint2"] = h.uint2;
                TblAccum one;
                one.addRows.push_back(std::move(row));
                one.addMods.push_back(std::string());
                addToGroup(tblAcc["t_lookpoint"], std::move(one));
            }
            for (const auto& tf : tblDirFiles) {
                if (tf.lang.empty() ? !modHas(tf.mod, L) : tf.lang != L) continue;
                const std::string stem = tblTargetOf(tf.path);
                const std::string& modName = tf.mod;
                TblAccum one;
                parseTblFile(tf.path, one.table, one.addRows, one.edits, one.cloneRows, modName, tf.off, log, &one.addMods);
                TblGroup& g = tblAcc[stem];
                g.mods.insert(modName);
                addToGroup(g, std::move(one));
            }
            if (tblAcc.empty()) continue;

            const std::string& pacName  = GL.tablePac;
            const std::string& entryPre = GL.tablePre;
            const std::string outSub    = "table_" + L;

            FpacReader pacTable;
            if (!pacTable.Open(fs::path(paths.pacSteamDir) / pacName)) {
                log += "[modkit] cannot open " + pacName + "\n";
                addLangErr(tblErrs, pacName, {}, "无法打开 " + pacName + "(检查游戏 pac\\steam)", L);
                continue;
            }
            for (auto& kv : tblAcc) {
                const std::string& stem = kv.first;
                const TblGroup& grp = kv.second;
                std::vector<std::string> tmods(grp.mods.begin(), grp.mods.end());
                std::vector<uint8_t> origTbl;
                if (!pacTable.ReadEntry(entryPre + stem + ".tbl", origTbl)) {
                    log += "[modkit] no pac entry " + entryPre + stem + ".tbl\n";
                    addLangErr(tblErrs, stem + ".tbl", tmods, "pac 内无此表(表名/文件名错?)", L); continue;
                }
                std::vector<uint8_t> outBytes = origTbl;
                std::vector<tbl_merge::TblConflict> conflicts;
                size_t nAdd = 0, nEdit = 0, nClone = 0;
                std::string subLog;
                bool okTbl = true;
                for (const TblAccum& a : grp.subs) {
                    const std::string what = grp.subs.size() > 1 ? stem + "(" + a.table + ")" : stem;
                    std::vector<uint8_t> next; std::string e2;
                    if (!a.cloneRows.empty()) {
                        if (!a.addRows.empty() || !a.edits.empty()) {
                            log += "[modkit] tbl FAIL " + outSub + "/" + what + ": clone_rows 不能与 add_rows/edit_rows 混用\n"; addLangErr(tblErrs, stem + ".tbl", tmods, "clone_rows 不能与 add_rows/edit_rows 混用", L); okTbl = false; break;
                        }
                        if (!tbl_merge::CloneRowsPoolTable(outBytes, paths.schemasDir, "Sora1", a.table, a.cloneRows, next, e2)) {
                            log += "[modkit] tbl clone FAIL " + outSub + "/" + what + ": " + e2 + "\n"; addLangErr(tblErrs, stem + ".tbl", tmods, e2, L); okTbl = false; break;
                        }
                    } else {
                        if (tbl_merge::TblHasUnmodeledPool(outBytes, paths.schemasDir, "Sora1")) {
                            log += "[modkit] tbl FAIL " + outSub + "/" + what + ": 该表带数据池,add_rows/edit_rows 会损坏;请用 clone_rows 或 table\\ 整文件替换\n";
                            addLangErr(tblErrs, stem + ".tbl", tmods, "带数据池的表,请用 clone_rows 或 table\\ 整文件替换(add_rows/edit_rows 会损坏)", L); okTbl = false; break;
                        }
                        if (!tbl_merge::ApplyTblPatch(outBytes, paths.schemasDir, "Sora1", a.table, a.addRows, a.edits, next, e2, &conflicts)) {
                            log += "[modkit] tbl FAIL " + outSub + "/" + what + ": " + e2 + "\n"; addLangErr(tblErrs, stem + ".tbl", tmods, e2, L); okTbl = false; break;
                        }
                    }
                    outBytes.swap(next);
                    nAdd += a.addRows.size(); nEdit += a.edits.size(); nClone += a.cloneRows.size();
                    if (grp.subs.size() > 1)
                        subLog += (subLog.empty() ? " [" : ", ") + a.table + " +" + std::to_string(a.addRows.size()) + "/" + std::to_string(a.edits.size()) + "/" + std::to_string(a.cloneRows.size());
                }
                if (!okTbl) continue;
                if (!subLog.empty()) subLog += "]";
                if (writeAll(fs::path(paths.cacheDir) / outSub / (stem + ".tbl"), outBytes)) {
                    log += "[modkit] merged " + outSub + "/" + stem + ".tbl (+" + std::to_string(nAdd) + " rows, " + std::to_string(nEdit) + " edits, " + std::to_string(nClone) + " clones)" + subLog + "\n";
                } else { log += "[modkit] write FAIL " + outSub + "/" + stem + "\n"; addLangErr(tblErrs, stem + ".tbl", tmods, "写缓存失败", L); continue; }
                if (isPrimary && stem == "t_name") {
                    for (const TblAccum& a : grp.subs) {
                        if (a.addRows.empty()) continue;
                        charAdds = a.addRows; charAddMods = a.addMods; charNameTbl = outBytes;
                    }
                }
                if (isPrimary && stem == "t_status") charStatusTbl = outBytes;
                if (isPrimary && stem == "t_item") {
                    for (const TblAccum& a : grp.subs) {
                        if (a.table != "ItemTableData" || a.addRows.empty()) continue;
                        itemAdds = a.addRows; itemAddMods = a.addMods; itemTbl = outBytes;
                    }
                }

                if (!reportedStems.insert(stem).second) continue;
                ++r.tbls;
                nlohmann::json tj;
                tj["table"] = stem;
                tj["mods"] = nlohmann::json::array();
                for (const auto& mname : grp.mods) tj["mods"].push_back(mname);
                tj["addRows"] = (int)nAdd;
                tj["editRows"] = (int)nEdit;
                tj["cloneRows"] = (int)nClone;
                if (grp.subs.size() > 1) {
                    tj["subTables"] = nlohmann::json::array();
                    for (const TblAccum& a : grp.subs)
                        tj["subTables"].push_back({ {"table", a.table}, {"addRows", (int)a.addRows.size()},
                                                    {"editRows", (int)a.edits.size()}, {"cloneRows", (int)a.cloneRows.size()} });
                }
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
    flushLangErrs(tblErrs);

    struct ModCharRow { int64_t id; int slot; std::string name, face, mod; int64_t level; double expCoef; };
    std::vector<ModCharRow> modCharRows;
    {
        std::string list ="# auto-generated by modkit: <character_id>\t<slot>\t<name>\t<face>\t<level>\t<exp_coef>\t<mod>\n";
        auto clean = [](std::string s) { for (char& c : s) if (c == '\t' || c == '\r' || c == '\n') c = ' '; return s; };
        if (!charAdds.empty() && !charNameTbl.empty()) {
            std::vector<uint8_t> statBytes = charStatusTbl;
            if (statBytes.empty()) {
                FpacReader pr;
                if (pr.Open(fs::path(paths.pacSteamDir) / langs[0].tablePac))
                    pr.ReadEntry(langs[0].tablePre + "t_status.tbl", statBytes);
            }
            TblFileG nameF, statF;
            std::string e1, e2;
            const bool okN = DecodeTblG(charNameTbl, paths.schemasDir, "Sora1", nameF, e1);
            const bool okS = !statBytes.empty() && DecodeTblG(statBytes, paths.schemasDir, "Sora1", statF, e2);
            const TblTableG* nt = nullptr;
            const TblTableG* stt = nullptr;
            if (okN) for (const auto& t : nameF.tables) if (t.name == "NameTableData") { nt = &t; break; }
            if (okS) for (const auto& t : statF.tables) if (t.name == "StatusParam") { stt = &t; break; }
            if (nt == nullptr || stt == nullptr || nt->rows.size() < charAdds.size()) {
                ++r.failed;
                log += "[modkit] mod_chars: 解 t_name / t_status 失败(" + e1 + e2 + "),自定义角色清单没生成\n";
                pushErr("自定义角色", {}, "解 t_name / t_status 失败,自定义角色清单没生成");
            } else {
                auto I = [](const TblRowG& row, const char* k) -> int64_t { const TblValue* v = row.find(k); return v ? v->i : 0; };
                auto F = [](const TblRowG& row, const char* k) -> double { const TblValue* v = row.find(k); return v ? v->f : 0.0; };
                auto S = [](const TblRowG& row, const char* k) -> std::string { const TblValue* v = row.find(k); return v ? v->s : std::string(); };
                std::map<std::string, const TblRowG*> statByKey;
                for (const auto& row : stt->rows) statByKey[S(row, "ai_file")] = &row;
                const size_t n = nt->rows.size(), first = n - charAdds.size();
                for (size_t j = 0; j < charAdds.size(); ++j) {
                    const TblRowG& row = nt->rows[first + j];
                    const int64_t id = I(row, "character_id");
                    const int slot = static_cast<int>(I(row, "long2") & 0xFF);
                    const std::string mod = j < charAddMods.size() ? charAddMods[j] : std::string();
                    const std::string name = clean(S(row, "name"));
                    if (charAdds[j].value("character_id", static_cast<int64_t>(0)) != id) {
                        log += "[modkit] mod_chars: t_name 表尾和 add_rows 对不上,后面的不再列\n";
                        break;
                    }
                    if (slot < 1 || slot > 99) continue;
                    const std::string who = "自定义角色 " + std::to_string(id) + (name.empty() ? std::string() : "「" + name + "」");
                    std::string why;
                    for (size_t k = 0; k < n && why.empty(); ++k) {
                        if (k == first + j) continue;
                        const TblRowG& o = nt->rows[k];
                        if (I(o, "character_id") == id) why = "角色编号 " + std::to_string(id) + " 已被 t_name 另一行占用";
                        else if ((I(o, "long2") & 0xFF) == slot)
                            why = "记录槽 " + std::to_string(slot) + " 已被角色 " + std::to_string(I(o, "character_id")) + " 占用";
                    }
                    const auto st = statByKey.find(S(row, "ref_name"));
                    if (why.empty() && st == statByKey.end()) why = "状态行「" + S(row, "ref_name") + "」在 t_status 里查不到";
                    if (!why.empty()) {
                        ++r.failed;
                        log += "[modkit] mod_chars: " + who + " 没列入:" + why + "\n";
                        pushErr(who, { mod }, why);
                        continue;
                    }
                    const TblRowG& sr = *st->second;
                    const std::string file5 = clean(S(sr, "file5"));
                    const std::string face = file5.empty() ? std::string() : "image/fc_" + file5 + ".dds";
                    const int64_t level = I(sr, "level");
                    const double expCoef = F(sr, "exp_growth");
                    char coef[32];
                    snprintf(coef, sizeof coef, "%.4f", expCoef);
                    list += std::to_string(id) + "\t" + std::to_string(slot) + "\t" + name + "\t" + face + "\t"
                          + std::to_string(level) + "\t" + coef + "\t" + clean(mod) + "\n";
                    modCharRows.push_back({ id, slot, name, face, mod, level, expCoef });
                    log += "[modkit] mod_chars: " + who + " 记录槽 " + std::to_string(slot) + " <- " + mod + "\n";
                }
            }
        }
        writeText(fs::path(paths.cacheDir).parent_path() / "mod_chars.list", list);
    }

    {
        struct CharKey { const char* key; bool def; };
        static const CharKey kCharKeys[] = {
            { "player_control", true },
            { "opening_by_ai", false },
            { "opening_scraft_by_player", false },
            { "brave_attack", false },
            { "sub_menu", false },
            { "hide_attack_in_crafts", true },
            { "hide_ai_only_crafts", true },
            { "camp_orbment", true },
            { "field_leader", false },
            { "camp_equip", false },
            { "camp_status", false },
            { "camp_costume", false },
            { "camp_top", false },
            { "break_stat_cap", false },
            { "clone_arts", false },
            { "party_support", false },
            { "overdrive", false },
            { "battle_as_member", false },
        };
        struct CharInt { const char* key; int64_t def, lo, hi; };
        static const CharInt kCharInts[] = {
            { "weapon_icon", 0, 0, 440 },
        };
        struct CharSet { bool v; std::string mod; };
        std::map<int64_t, std::map<std::string, CharSet>> sets;
        struct CharIntSet { int64_t v; std::string mod; };
        std::map<int64_t, std::map<std::string, CharIntSet>> isets;
        for (const auto& pf : modJsons) {
            const std::string& mod = pf.mod;
            auto bad = [&](const std::string& why) {
                ++r.failed;
                log += "[modkit] mod.json " + mod + ": " + why + "\n";
                pushErr("mod.json", { mod }, why);
            };
            const std::string raw = readAll(pf.path);
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(raw);
            } catch (const nlohmann::json::parse_error& e) {
                const size_t at = std::min<size_t>(e.byte, raw.size());
                const long line = 1 + static_cast<long>(std::count(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
                bad("mod.json 格式不对(第 " + std::to_string(line) + " 行附近),里面的角色设置没用上");
                continue;
            }
            if (!j.is_object() || !j.contains("characters")) continue;
            const nlohmann::json& cs = j["characters"];
            if (!cs.is_array()) {
                bad("characters 要写成 [ … ](一个角色一段 { … }),没用上");
                continue;
            }
            for (size_t k = 0; k < cs.size(); ++k) {
                const nlohmann::json& c = cs[k];
                const std::string nth = "characters 第 " + std::to_string(k + 1) + " 段";
                if (!c.is_object()) { bad(nth + "要写成 { … },没用上"); continue; }
                const auto idIt = c.find("character_id");
                if (idIt == c.end() || !idIt->is_number_integer()) { bad(nth + "缺整数的角色编号 character_id,没用上"); continue; }
                const int64_t id = idIt->get<int64_t>();
                const std::string who = "角色 " + std::to_string(id);
                if (std::none_of(modCharRows.begin(), modCharRows.end(), [id](const ModCharRow& x) { return x.id == id; })) {
                    bad(who + " 不是 MOD 新加的队伍角色(t_name 里没有它带记录槽的那一行),这段设置没用上");
                    continue;
                }
                std::map<std::string, CharSet>& got = sets[id];
                std::map<std::string, CharIntSet>& igot = isets[id];
                for (auto it = c.begin(); it != c.end(); ++it) {
                    if (it.key() == "character_id") continue;
                    const CharKey* ck = nullptr;
                    for (const auto& x : kCharKeys) if (it.key() == x.key) { ck = &x; break; }
                    const CharInt* ci = nullptr;
                    for (const auto& x : kCharInts) if (it.key() == x.key) { ci = &x; break; }
                    if (ci != nullptr) {
                        if (!it->is_number_integer()) { bad(who + " 的「" + it.key() + "」要写整数,没用上"); continue; }
                        const int64_t v = it->get<int64_t>();
                        if (v < ci->lo || v > ci->hi) {
                            bad(who + " 的「" + it.key() + "」要在 " + std::to_string(ci->lo) + "~" + std::to_string(ci->hi) + " 之间(写的是 "
                                + std::to_string(v) + "),没用上");
                            continue;
                        }
                        const auto prev = igot.find(ci->key);
                        if (prev != igot.end() && prev->second.v != v && prev->second.mod != mod) {
                            ++r.conflicts;
                            log += "[modkit] CONFLICT mod.json characters[" + std::to_string(id) + "]." + ci->key + ": "
                                 + std::to_string(prev->second.v) + " (" + prev->second.mod + ") -> " + std::to_string(v) + " (" + mod + ") [winner]\n";
                        }
                        igot[ci->key] = { v, mod };
                        continue;
                    }
                    if (ck == nullptr) { bad(who + " 的「" + it.key() + "」不认识(拼错了?),没用上"); continue; }
                    if (!it->is_boolean()) { bad(who + " 的「" + it.key() + "」要写 true 或 false,没用上"); continue; }
                    const bool v = it->get<bool>();
                    const auto prev = got.find(ck->key);
                    if (prev != got.end() && prev->second.v != v && prev->second.mod != mod) {
                        ++r.conflicts;
                        log += "[modkit] CONFLICT mod.json characters[" + std::to_string(id) + "]." + ck->key + ": "
                             + (prev->second.v ? "true" : "false") + " (" + prev->second.mod + ") -> "
                             + (v ? "true" : "false") + " (" + mod + ") [winner]\n";
                    }
                    got[ck->key] = { v, mod };
                }
            }
        }
        nlohmann::json out;
        out["version"] = 1;
        out["fingerprint"] = fp;
        out["characters"] = nlohmann::json::array();
        report["modChars"] = nlohmann::json::array();
        for (const ModCharRow& row : modCharRows) {
            nlohmann::json c = { {"character_id", row.id}, {"slot", row.slot}, {"name", row.name}, {"face", row.face},
                                 {"level", row.level}, {"exp_coef", row.expCoef}, {"mod", row.mod} };
            nlohmann::json st = nlohmann::json::object();
            const auto sit = sets.find(row.id);
            for (const auto& x : kCharKeys) {
                bool v = x.def;
                if (sit != sets.end()) {
                    const auto f = sit->second.find(x.key);
                    if (f != sit->second.end()) v = f->second.v;
                }
                c[x.key] = v;
                st[x.key] = v;
            }
            const auto iit = isets.find(row.id);
            for (const auto& x : kCharInts) {
                int64_t v = x.def;
                if (iit != isets.end()) {
                    const auto f = iit->second.find(x.key);
                    if (f != iit->second.end()) v = f->second.v;
                }
                c[x.key] = v;
                st[x.key] = v;
            }
            out["characters"].push_back(std::move(c));
            report["modChars"].push_back({ {"id", row.id}, {"slot", row.slot}, {"name", row.name}, {"face", row.face},
                                           {"level", row.level}, {"expCoef", row.expCoef}, {"mod", row.mod}, {"settings", st} });
        }
        writeText(fs::path(paths.cacheDir).parent_path() / "mod_chars.json",
                  out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    }

    {
        nlohmann::json out;
        out["version"] = 1;
        out["fingerprint"] = fp;
        out["items"] = nlohmann::json::array();
        if (!itemAdds.empty() && !itemTbl.empty()) {
            TblFileG itemF;
            std::string e1;
            const TblTableG* it = nullptr;
            if (DecodeTblG(itemTbl, paths.schemasDir, "Sora1", itemF, e1))
                for (const auto& t : itemF.tables) if (t.name == "ItemTableData") { it = &t; break; }
            if (it == nullptr || it->rows.size() < itemAdds.size()) {
                ++r.failed;
                log += "[modkit] mod_items: 解 t_item 失败(" + e1 + "),MOD 新增道具清单没生成\n";
                pushErr("MOD 新增道具", {}, "解 t_item 失败,MOD 新增道具清单没生成");
            } else {
                auto I = [](const TblRowG& row, const char* k) -> int64_t { const TblValue* v = row.find(k); return v ? v->i : 0; };
                auto S = [](const TblRowG& row, const char* k) -> std::string { const TblValue* v = row.find(k); return v ? v->s : std::string(); };
                const size_t n = it->rows.size(), first = n - itemAdds.size();
                for (size_t j = 0; j < itemAdds.size(); ++j) {
                    const TblRowG& row = it->rows[first + j];
                    const int64_t id = I(row, "id");
                    const std::string mod = j < itemAddMods.size() ? itemAddMods[j] : std::string();
                    const std::string name = S(row, "name");
                    if (itemAdds[j].value("id", static_cast<int64_t>(-1)) != id) {
                        log += "[modkit] mod_items: t_item 表尾和 add_rows 对不上,后面的不再列\n";
                        break;
                    }
                    const std::string who = "道具 " + std::to_string(id) + (name.empty() ? std::string() : "「" + name + "」");
                    std::string why;
                    if (id < 1 || id > 4999) why = "编号要在 1~4999(背包数量按编号存,只到 4999)";
                    for (size_t k = 0; k < n && why.empty(); ++k)
                        if (k != first + j && I(it->rows[k], "id") == id) why = "编号 " + std::to_string(id) + " 已被 t_item 另一行占用";
                    if (!why.empty()) {
                        ++r.failed;
                        log += "[modkit] mod_items: " + who + " 没列入:" + why + "\n";
                        pushErr(who, { mod }, why);
                        continue;
                    }
                    out["items"].push_back({ {"id", id}, {"category", I(row, "category")}, {"subcategory", I(row, "subcategory")},
                                             {"name", name}, {"icon", I(row, "item_icon")}, {"mod", mod} });
                    log += "[modkit] mod_items: " + who + " <- " + mod + "\n";
                }
            }
        }
        writeText(fs::path(paths.cacheDir).parent_path() / "mod_items.json",
                  out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    }

    {
        std::string table = "# auto-generated by modkit: <map>\\t<script>\n";
        std::set<std::string> listSeen;
        report["inject"] = nlohmann::json::array();
        for (const auto& jf : injectFiles) {
            fs::path moddir = jf.path.parent_path();
            for (const auto& e : parseScriptInject(jf.path, jf.mod, jf.lang, log)) {
                std::set<std::string> covered;
                std::error_code fe;
                std::string s = readAll(e.datPath);
                if (!s.empty()) {
                    std::vector<uint8_t> bytes(s.begin(), s.end());
                    for (const auto& GL : langs)
                        if (modHas(e.mod, GL.code) && writeAll(fs::path(paths.cacheDir) / ("script_" + GL.code) / (e.script + ".dat"), bytes))
                            covered.insert(GL.code);
                }
                if (fs::exists(moddir / "dat" / (e.script + ".dat"), fe))
                    for (const auto& GL : langs) if (modHas(e.mod, GL.code)) covered.insert(GL.code);
                for (const char* d : kModLangDirs) {
                    const std::string code = ModLangCode(d, langs);
                    if (!code.empty() && fs::exists(moddir / "dat" / d / (e.script + ".dat"), fe)) covered.insert(code);
                }
                if (covered.empty()) {
                    log += "[modkit] add_dat_ini WARN: \"" + e.script + "\" 无对话 dat(放 Mod\\" + e.mod + "\\dat\\<lang>\\" + e.script + ".dat)\n";
                    pushErr("add_dat_ini \"" + e.script + "\"", { e.mod }, "缺对话 dat:" + e.script + ".dat");
                    continue;
                }
                if (covered.size() < langs.size()) {
                    std::string miss;
                    for (const auto& GL : langs) if (!covered.count(GL.code)) miss += (miss.empty() ? "" : " / ") + GL.code;
                    log += "[modkit] add_dat_ini \"" + e.script + "\":这几种语言没有对话 dat,进图加载不到:" + miss + "\n";
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

    struct WholeProv { std::string mod; fs::path path; };
    struct WholeOut { std::string lang, name; std::vector<WholeProv> provs; };
    auto expandWhole = [&](const std::vector<PathSrc>& files, auto nameOf) {
        std::map<std::string, size_t> idx;
        std::vector<WholeOut> outs;
        for (const auto& f : files) {
            const std::string name = nameOf(f);
            std::vector<std::string> ls;
            if (!f.lang.empty()) ls.push_back(f.lang);
            else for (const auto& GL : langs) if (modHas(f.mod, GL.code)) ls.push_back(GL.code);
            for (const auto& L : ls) {
                const std::string key = L + "/" + name;
                auto it = idx.find(key);
                if (it == idx.end()) { it = idx.emplace(key, outs.size()).first; outs.push_back({ L, name, {} }); }
                outs[it->second].provs.push_back({ f.mod, f.path });
            }
        }
        return outs;
    };
    report["datConflicts"] = nlohmann::json::array();
    std::map<std::string, size_t> wholeConfIdx;
    auto noteWholeConflicts = [&](const WholeOut& o, const std::string& repName, const std::string& logWhat) {
        const std::string& by = o.provs.back().mod;
        bool any = false;
        std::set<std::string> seen;
        for (size_t i = 0; i + 1 < o.provs.size(); ++i) {
            const std::string& from = o.provs[i].mod;
            if (from == by || !seen.insert(from).second) continue;
            any = true;
            log += "[modkit] CONFLICT " + logWhat + ": " + from + " -> " + by + " [winner,整文件]\n";
            const std::string key = repName + "\x1f" + from + "\x1f" + by;
            auto it = wholeConfIdx.find(key);
            if (it == wholeConfIdx.end()) {
                ++r.conflicts;
                wholeConfIdx[key] = report["datConflicts"].size();
                report["datConflicts"].push_back({ {"name", repName}, {"lang", o.lang}, {"langs", nlohmann::json::array({ o.lang })},
                                                   {"fromMod", from}, {"byMod", by} });
            } else {
                report["datConflicts"][it->second]["langs"].push_back(o.lang);
            }
        }
        return any;
    };
    auto noteWholeOut = [&](nlohmann::json& arr, std::map<std::string, size_t>& index, const WholeOut& o) {
        const std::string key = o.name + "\x1f" + o.provs.back().mod;
        auto it = index.find(key);
        const bool first = (it == index.end());
        if (first) {
            nlohmann::json one; one["name"] = o.name; one["winner"] = o.provs.back().mod;
            one["mods"] = nlohmann::json::array();
            std::set<std::string> seen;
            for (const auto& p : o.provs) if (seen.insert(p.mod).second) one["mods"].push_back(p.mod);
            one["langs"] = nlohmann::json::array();
            it = index.emplace(key, arr.size()).first;
            arr.push_back(std::move(one));
        }
        arr[it->second]["langs"].push_back(o.lang);
        return first;
    };

    {
        report["datReplace"] = nlohmann::json::array();
        std::map<std::string, size_t> repIdx;
        std::vector<LangErr> errs;
        for (const WholeOut& o : expandWhole(datReplaceFiles, [](const PathSrc& f) { return f.path.stem().string(); })) {
            const std::string sub = "script_" + o.lang;
            const auto& win = o.provs.back();
            std::string s = readAll(win.path);
            if (s.empty()) { log += "[modkit] dat_replace empty/missing: " + win.path.generic_string() + "\n"; addLangErr(errs, "dat 替换 \"" + o.name + "\"", { win.mod }, "dat 为空/读取失败", o.lang); continue; }
            std::vector<uint8_t> bytes(s.begin(), s.end());
            fs::path dst = fs::path(paths.cacheDir) / sub / (o.name + ".dat");
            if (!writeAll(dst, bytes)) { log += "[modkit] dat_replace copy FAIL " + dst.generic_string() + "\n"; addLangErr(errs, "dat 替换 \"" + o.name + "\"", { win.mod }, "写缓存失败", o.lang); continue; }
            noteWholeOut(report["datReplace"], repIdx, o);
            const bool over = noteWholeConflicts(o, o.name, "dat " + sub + "/" + o.name + ".dat");
            log += "[modkit] dat_replace " + sub + "/" + o.name + ".dat <- " + win.mod + (over ? " (覆盖其它)" : "") + "\n";
        }
        flushLangErrs(errs);
    }

    auto deployScripts = [&](const std::vector<PathSrc>& files, const std::string& dir, const std::string& what) {
        report[dir] = nlohmann::json::array();
        std::map<std::string, size_t> repIdx;
        std::vector<LangErr> errs;
        for (const WholeOut& o : expandWhole(files, [](const PathSrc& f) { return wtou8(f.path.filename().wstring()); })) {
            const std::string sub = "script_" + o.lang;
            const auto& win = o.provs.back();
            std::string s = readAll(win.path);
            if (s.size() < 4 || s.compare(0, 4, "#scp") != 0) {
                log += "[modkit] " + dir + " 不是 #scp 脚本(或读不到): " + wtou8(win.path.wstring()) + "\n";
                addLangErr(errs, what + " \"" + o.name + "\"", { win.mod }, "不是 #scp 脚本(或读取失败)", o.lang); continue;
            }
            std::vector<uint8_t> bytes(s.begin(), s.end());
            fs::path dst = fs::path(paths.cacheDir) / sub / dir / win.path.filename();
            if (!writeAll(dst, bytes)) { log += "[modkit] " + dir + " copy FAIL " + sub + "/" + dir + "/" + o.name + "\n"; addLangErr(errs, what + " \"" + o.name + "\"", { win.mod }, "写缓存失败", o.lang); continue; }
            noteWholeOut(report[dir], repIdx, o);
            const bool over = noteWholeConflicts(o, dir + "/" + o.name, dir + " " + sub + "/" + dir + "/" + o.name);
            log += "[modkit] " + dir + " " + sub + "/" + dir + "/" + o.name + " <- " + win.mod + (over ? " (覆盖其它)" : "") + "\n";
        }
        flushLangErrs(errs);
    };
    deployScripts(aiFiles, "ai", "AI");
    deployScripts(aniFiles, "ani", "动作脚本");

    {
        report["listExtraLoad"] = nlohmann::json::array();
        struct Part { std::string mod; std::vector<DatInsertCall> inserts; };
        std::map<std::string, std::vector<Part>> byTarget;
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
            byTarget[target].push_back({ pf.mod, std::move(cfg.inserts) });
        }
        std::map<std::string, FpacReader> scriptPacs;
        std::map<std::string, bool> scriptPacOk;
        std::vector<LangErr> errs;
        for (const std::string& target : targets) {
            const auto& parts = byTarget[target];
            std::set<std::string> modSet;
            for (const auto& p : parts) modSet.insert(p.mod);
            std::vector<std::string> tmods(modSet.begin(), modSet.end());
            std::vector<std::string> doneLangs;
            size_t maxInserts = 0;
            std::set<std::string> missing;
            for (const GameLang& GL : langs) {
                std::vector<DatInsertCall> inserts;
                for (const auto& p : parts) if (modHas(p.mod, GL.code)) inserts.insert(inserts.end(), p.inserts.begin(), p.inserts.end());
                if (inserts.empty()) continue;
                if (!scriptPacOk.count(GL.code)) scriptPacOk[GL.code] = scriptPacs[GL.code].Open(fs::path(paths.pacSteamDir) / GL.scriptPac);
                if (!scriptPacOk[GL.code]) { addLangErr(errs, GL.scriptPac, {}, "无法打开 " + GL.scriptPac + "(检查游戏 pac\\steam)", GL.code); continue; }
                fs::path dst = fs::path(paths.cacheDir) / ("script_" + GL.code) / (target + ".dat");
                std::vector<uint8_t> orig;
                std::string cached = readAll(dst);
                if (!cached.empty()) orig.assign(cached.begin(), cached.end());
                else if (!scriptPacs[GL.code].ReadEntry(GL.scriptPre + "scena/" + target + ".dat", orig)) { missing.insert(GL.code); continue; }
                std::vector<uint8_t> outBytes;
                std::string err;
                if (!ApplyDatPatch(orig, inserts, outBytes, err)) {
                    log += "[modkit] ListExtraLoad FAIL script_" + GL.code + "/" + target + ".dat: " + err + "\n";
                    addLangErr(errs, "ListExtraLoad \"" + target + "\"", tmods, err, GL.code); continue;
                }
                if (!writeAll(dst, outBytes)) { addLangErr(errs, "ListExtraLoad \"" + target + "\"", tmods, "写缓存失败", GL.code); continue; }
                doneLangs.push_back(GL.code);
                maxInserts = std::max(maxInserts, inserts.size());
            }
            if (!missing.empty() && doneLangs.empty()) {
                const std::string why = "任何语言的 script pac 里都没有 " + target + ".dat";
                log += "[modkit] ListExtraLoad FAIL " + target + ": " + why + "\n";
                for (const auto& L : missing) addLangErr(errs, "ListExtraLoad \"" + target + "\"", tmods, why, L);
            } else if (!missing.empty()) {
                std::string ml;
                for (const auto& L : missing) ml += (ml.empty() ? "" : " / ") + L;
                log += "[modkit] ListExtraLoad " + target + ".dat:这几种语言的 script pac 里没有它,没打:" + ml + "\n";
            }
            if (doneLangs.empty()) continue;
            ++r.merged;
            report["listExtraLoad"].push_back({ {"name", target}, {"inserts", maxInserts},
                                                {"langs", doneLangs}, {"mods", tmods} });
            log += "[modkit] ListExtraLoad " + target + ".dat (+" + std::to_string(maxInserts)
                 + " 处插入, " + std::to_string(doneLangs.size()) + " 个语言)\n";
        }
        flushLangErrs(errs);
    }

    {
        report["tableReplace"] = nlohmann::json::array();
        std::map<std::string, size_t> repIdx;
        std::vector<LangErr> errs;
        std::set<std::string> counted;
        for (const WholeOut& o : expandWhole(tableReplaceFiles, [](const PathSrc& f) { return f.path.stem().string(); })) {
            const std::string sub = "table_" + o.lang;
            const auto& win = o.provs.back();
            std::string s = readAll(win.path);
            if (s.empty()) { log += "[modkit] table_replace empty/missing: " + win.path.generic_string() + "\n"; addLangErr(errs, "table 替换 \"" + o.name + "\"", { win.mod }, "tbl 为空/读取失败", o.lang); continue; }
            std::vector<uint8_t> bytes(s.begin(), s.end());
            if (!writeAll(fs::path(paths.cacheDir) / sub / (o.name + ".tbl"), bytes)) { log += "[modkit] table_replace copy FAIL " + sub + "/" + o.name + "\n"; addLangErr(errs, "table 替换 \"" + o.name + "\"", { win.mod }, "写缓存失败", o.lang); continue; }
            if (counted.insert(o.name).second) ++r.tbls;
            noteWholeOut(report["tableReplace"], repIdx, o);
            const bool over = noteWholeConflicts(o, o.name + ".tbl", "table " + sub + "/" + o.name + ".tbl");
            log += "[modkit] table_replace " + sub + "/" + o.name + ".tbl <- " + win.mod + (over ? " (覆盖其它)" : "") + "\n";
        }
        flushLangErrs(errs);
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
            if (rel.rfind("vfx/", 0) == 0) {
                std::vector<std::pair<std::string, fs::path>> good;
                for (const auto& p : provs) {
                    char head[4] = {};
                    { std::ifstream f(p.second, std::ios::binary); f.read(head, 4); }
                    if (std::memcmp(head, "JSON", 4) == 0) { good.push_back(p); continue; }
                    ++r.failed;
                    log += "[modkit] vfx REJECT " + rel + " <- " + p.first + "(开头不是 JSON)\n";
                    pushErr("vfx \"" + rel + "\"", { p.first }, "不是特效文件(开头应为 JSON,即 Falcom 二进制 JSON),没有铺");
                }
                provs = std::move(good);
                if (provs.empty()) continue;
            }
            if (provs.size() > 1 && rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".dds") == 0) {
                std::vector<uint8_t> composed;
                std::string why;
                if (composeDds(paths.pacSteamDir, rel, provs, composed, why)) {
                    if (writeAll(fs::path(paths.cacheDir) / fs::path(rel), composed)) {
                        ++r.assets;
                        nlohmann::json mods = nlohmann::json::array();
                        std::string names;
                        for (const auto& p : provs) {
                            mods.push_back(p.first);
                            names += (names.empty() ? "" : " + ") + p.first;
                        }
                        report["assets"].push_back({ {"name", rel}, {"winner", provs.back().first}, {"composed", true}, {"mods", mods} });
                        log += "[modkit] asset " + rel + " <- " + names + "(按块合成)\n";
                        continue;
                    }
                    why = "写缓存失败";
                }
                log += "[modkit] asset " + rel + " 不能按块合成(" + why + "),照旧整文件\n";
            }
            const auto& win = provs.back();
            fs::path dst = fs::path(paths.cacheDir) / (rel.rfind("vfx/", 0) == 0 ? fs::path(u8tow(rel)) : fs::path(rel));
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
