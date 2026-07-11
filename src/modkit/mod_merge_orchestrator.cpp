#include "modkit/mod_merge_orchestrator.h"
#include "modkit/patch_config.h"
#include "modkit/scene_merge.h"
#include "modkit/tbl_codec.h"
#include "modkit/tbl_merge.h"
#include "modkit/fpac_reader.h"
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

namespace { // UTF-8 ↔ 宽字符(Windows 文件名走 UTF-16,需显式转 UTF-8 给 json/GUI;path::string() 走 ANSI 代码页会把中文变 ?/乱码)
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
} // namespace

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


// add_dat_ini.json:声明「图主脚本名 -> 追加加载的脚本 dat」。引擎进图加载 <map> 时,ScriptInject 追加加载 <script>。
struct InjectEntry {
    std::string map;     // 图主脚本名(触发,如 "mp4000")
    std::string script;  // 追加加载的脚本名(引擎 Open "script/scena/<script>.dat",如 "Test_point")
    std::string mod;     // mod 文件夹名(日志用)
    std::string lang;    // ""=语言中性(script);"sc/tc/kr"=语言专属(script_<L>)
    fs::path datPath;    // 源 dat 绝对路径(默认 <script>.dat,相对 add_dat_ini.json 所在目录)
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

// add_dat_ini.json 的可选 "mon_load":[{"map","table"}]:声明「进图加载怪物表(t_mon)」。
// ScriptInject 插件在该图主脚本加载时调引擎 map_load_mons_table(逆向入口),不动主 dat。
struct MonLoadEntry { std::string map, table, mod; };
static std::vector<MonLoadEntry> parseMonLoad(const fs::path& p, const std::string& mod, std::string& log) {
    std::vector<MonLoadEntry> out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    nlohmann::json j;
    try { f >> j; } catch (const std::exception&) { return out; }   // inject 解析已报过错
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

// add_dat_ini.json 的可选 "mon_event":[{"map","event"}]:声明「进图后把某注入脚本函数当事件线程跑一次」。
// ScriptInject 进该图后用引擎 FUN_140247550 spawn 该函数(如 MONS_INIT),让其内部 Cmd_map_02(=map_load_mons_table)
// 在真正脚本 VM ctx 里执行完整①加载表②创建宝箱怪char③绑定挂区(不动主 dat)。配了 mon_event 的图,插件不再直接预加载
// 怪物表(交给事件),只守卫区处理器(有区无表→跳帧不崩)。
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

// 顺序语义:列表靠后者「后套用、覆盖靠前者」(last-writer-wins,= GUI 里往下拖 = 优先级更高)。
// 现存但未登记的文件夹按名追加到末尾(默认启用);清单里文件夹已删的条目丢弃。
std::vector<ModInfo> ScanMods(const std::wstring& modsDir) {
    fs::path md(modsDir);
    std::error_code ec;
    std::vector<std::string> present;
    if (fs::is_directory(md, ec))
        for (const auto& e : fs::directory_iterator(md, ec))
            if (e.is_directory()) present.push_back(wtou8(e.path().filename().wstring()));  // UTF-8 名(支持中文)
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
            if (std::find(present.begin(), present.end(), name) == present.end()) continue; // 文件夹已删
            ModInfo m; m.name = name; m.enabled = it.value("enabled", true);
            if (it.contains("disabled") && it["disabled"].is_array())
                for (const auto& d : it["disabled"]) if (d.is_string()) m.disabled.push_back(d.get<std::string>());
            ordered.push_back(std::move(m)); taken.insert(name);
        }
    }
    for (const auto& n : present) if (!taken.count(n)) ordered.push_back(ModInfo{ n, true });
    return ordered;
}

// 统一序列化:mods 列表 → mods.json 文本(SaveMods 与 LoadModManifest 共用,避免后者把 disabled 洗掉)。
static std::string modsToJsonText(const std::vector<ModInfo>& mods) {
    nlohmann::json outj; outj["version"] = 1; outj["mods"] = nlohmann::json::array();
    for (const auto& m : mods) {
        nlohmann::json mj; mj["name"] = m.name; mj["enabled"] = m.enabled;
        if (!m.disabled.empty()) mj["disabled"] = m.disabled;   // 无禁用项则不写,保持 mods.json 简洁
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

// 内部:扫描 + 若与磁盘不同则写回(自动登记新 mod / 清理已删),使下次运行 / GUI 看到最新清单。
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

// 解析 Mod\<mod>\tbl\<表>.json → tableName(可空) + add_rows + edit_rows。每条 edit 打上 modName 来源。
// 单条配置是否被关闭:off 含 "<section>/<index>"(index = 该数组里的 0 基下标,与 GUI 一致)
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
    // LookPointTableData 友好字段别名(map→text1 / name→text2 / label→text3;raw schema 名也可用)。
    // t_lookpoint 统一走标准 add_rows/edit_rows;旧的 {"t_lookpoint":[...]} 特殊结构已不再支持。
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
    // add_npc:从零构造(NPCParam)——借 behavior_from 行的 flag-tag 块头(无法合成),把用户没填的资源 ref 清空
    // (不继承模板的对话/语音/动作),再按配置填(model/pos/talk/anim/...)。转成一个 clone op 复用 clone 引擎。
    if (j.contains("add_npc") && j["add_npc"].is_array()) {
        size_t npcIdx = 0;
        for (const auto& e : j["add_npc"]) {
            bool skip = entryOff(off, "add_npc", npcIdx); ++npcIdx;
            if (!e.is_object() || skip) continue;
            auto alias = [](const std::string& k) -> std::string {
                return tbl_merge::AliasNpcParamField(k);  // 与 aliasField 共用唯一别名来源(中文/英文/raw)
            };
            std::set<std::string> provided;
            for (auto it = e.begin(); it != e.end(); ++it) if (it.key() != "behavior_from") provided.insert(alias(it.key()));
            nlohmann::json set;
            set["flags_18"] = 1; set["type_28"] = 1;        // 启用 + 普通交互类型
            // 清空模板继承的 param/flag(否则会带上源 NPC 的特殊交互,如 param_98=1 商人/2 商店)
            for (const char* z : { "flags_04", "flags_1C", "param_float_3C", "param_float_40", "param_44",
                                   "float_48", "float_4C", "param_50", "param_54", "param_70", "param_74",
                                   "param_88", "param_8C", "param_98", "param_9C" })
                if (!provided.count(z)) set[z] = 0;
            for (const char* rr : { "resource_ref_60", "resource_ref_68", "resource_ref_78", "resource_ref_80", "resource_ref_90" })
                if (!provided.count(rr)) set[rr] = "";       // 仅清空没填的
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

// tbl 类来源:文件路径 + 被单独关闭的「单条配置」key 集合 + 语言 + 来源 mod。
// lang="" 共享层(套到每个语言);"sc"/"tc"/"kr"=该语言文字覆盖。
struct TblSrc { fs::path path; std::set<std::string> off; std::string lang; std::string mod; };
// asset 透传来源:源文件 + pac 相对名(如 "asset/common/model/chr0001_c61.mdl")+ 来源 mod
struct AssetSrc { fs::path src; std::string rel; std::string mod; };
// 通用脚本/替换来源:路径 + 来源 mod + 语言(""=语言中性 → script\;"sc/tc/kr" → script_<L>\ / table_<L>\)
struct PathSrc { fs::path path; std::string mod; std::string lang; };

RunResult Run(const Paths& paths, bool force) {
    RunResult r;
    std::string log;

    // 1) 读 mods.json 加载清单(顺序 + 启停),按清单顺序收集「启用」mod 的文件。
    //    不再按路径全局排序——保留 mod 顺序 = tbl edit 的套用顺序(靠后者覆盖靠前者)。
    std::error_code ec;
    std::vector<ModInfo> manifest = LoadModManifest(paths.modsDir, log);
    std::vector<fs::path> datFiles;                      // inject 源 dat(仅指纹用)
    std::vector<PathSrc> patchFiles, injectFiles, datReplaceFiles, tableReplaceFiles;  // 带 mod;patchFiles=scene_add_json
    std::vector<TblSrc> tblDirFiles;   // tbl:Mod\<mod>\[<lang>\]tbl\*.json,逐条可关
    std::vector<AssetSrc> assetFiles;  // asset 透传:Mod\<mod>\asset\** 原样铺到 cache\merged\asset\**
    int enabledMods = 0;
    for (const ModInfo& m : manifest) {
        if (!m.enabled) continue;
        ++enabledMods;
        fs::path dir = fs::path(paths.modsDir) / u8tow(m.name);   // m.name 是 UTF-8,转宽字符再拼路径(支持中文名)
        // 整组件是否被关闭:rel = 相对 mod 目录的正斜杠路径(与 GUI mod_manager 计算一致)
        auto off = [&](const std::string& rel) {
            return std::find(m.disabled.begin(), m.disabled.end(), rel) != m.disabled.end();
        };
        // 某个 tbl json 文件里被关闭的「单条」key 集合(disabled 项形如 "<fileRel>#<section>/<idx>")
        auto entryOffSet = [&](const std::string& fileRel) {
            std::set<std::string> s; std::string pre = fileRel + "#";
            for (const auto& d : m.disabled) if (d.rfind(pre, 0) == 0) s.insert(d.substr(pre.size()));
            return s;
        };
        // 语言无关(只在 mod 根):scene_add_json\ + asset + 中性脚本(script_inject + 其引用的 *.dat)
        // scene 补丁:一场景一 json,文件名=目标场景(target 从文件名推断,json 里可不写 target)。
        { fs::path sceneDir = dir / "scene_add_json";
          if (fs::is_directory(sceneDir, ec)) {
              std::vector<fs::path> sfs;
              for (const auto& sf : fs::directory_iterator(sceneDir, ec))
                  if (sf.is_regular_file() && sf.path().extension() == ".json"
                      && !off("scene_add_json/" + sf.path().filename().generic_string())) sfs.push_back(sf.path());
              std::sort(sfs.begin(), sfs.end());
              for (auto& s : sfs) patchFiles.push_back({ std::move(s), m.name, "" }); } }
        { fs::path ij = dir / "add_dat_ini.json";   // 兼容旧名 script_inject.json
          if (fs::exists(ij, ec) && !off("add_dat_ini.json")) injectFiles.push_back({ ij, m.name, "" }); }
        { std::vector<fs::path> dats;                              // 根级 *.dat(inject 源,进指纹)
          for (const auto& df : fs::directory_iterator(dir, ec))
              if (df.is_regular_file() && df.path().extension() == ".dat"
                  && !off(df.path().filename().generic_string())) dats.push_back(df.path());
          std::sort(dats.begin(), dats.end());
          for (auto& d : dats) datFiles.push_back(std::move(d)); }
        { fs::path assetDir = dir / "asset";                      // 资源透传(语言无关)
          if (fs::is_directory(assetDir, ec)) {
              std::vector<AssetSrc> as;
              for (auto it = fs::recursive_directory_iterator(assetDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                  if (ec) break;
                  if (!it->is_regular_file(ec)) continue;
                  std::string rel = "asset/" + fs::relative(it->path(), assetDir, ec).generic_string();  // 引擎查找键
                  if (off(rel)) continue;
                  as.push_back({ it->path(), rel, m.name });
              }
              std::sort(as.begin(), as.end(), [](const AssetSrc& a, const AssetSrc& b){ return a.rel < b.rel; });
              for (auto& a : as) assetFiles.push_back(std::move(a)); } }
        // 音频资源透传(语言无关):Mod\<mod>\{voice,se,bgm1,bgm2,bgm3}\** 按 pac 相对名原样铺到 cache\merged\**。
        //   如 Mod\<mod>\voice\wav\X.wav → 引擎查找键 "voice/wav/X.wav"(未压缩 WAV,由 SceneRedirect 提供,不改原 pac)。
        for (const char* ns : { "voice", "se", "bgm1", "bgm2", "bgm3" }) {
            fs::path nsDir = dir / ns;
            if (!fs::is_directory(nsDir, ec)) continue;
            std::vector<AssetSrc> rs;
            for (auto it = fs::recursive_directory_iterator(nsDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string rel = std::string(ns) + "/" + fs::relative(it->path(), nsDir, ec).generic_string();  // 引擎查找键
                if (off(rel)) continue;
                rs.push_back({ it->path(), rel, m.name });
            }
            std::sort(rs.begin(), rs.end(), [](const AssetSrc& a, const AssetSrc& b){ return a.rel < b.rel; });
            for (auto& r : rs) assetFiles.push_back(std::move(r));
        }
        // scene 二进制原样透传:Mod\<mod>\scene_raw\*.json(预合并好的 bjson scene,如室内图加 MonsterArea)
        //   → cache\merged\scene\<名>。复用 assetFiles(rel="scene/<名>")=部署/指纹/冲突全走 asset 通道。
        //   ⚠用于 scene_add_json 合并不了的场景(目标图无同类型 actor 模板/名表缺字段名)。
        { fs::path sceneRawDir = dir / "scene_raw";
          if (fs::is_directory(sceneRawDir, ec)) {
              std::vector<AssetSrc> srs;
              for (const auto& sf : fs::directory_iterator(sceneRawDir, ec)) {
                  if (!sf.is_regular_file()) continue;
                  std::string rel = "scene/" + sf.path().filename().generic_string();   // 引擎查找键
                  if (off(rel)) continue;
                  srs.push_back({ sf.path(), rel, m.name });
              }
              std::sort(srs.begin(), srs.end(), [](const AssetSrc& a, const AssetSrc& b){ return a.rel < b.rel; });
              for (auto& s : srs) assetFiles.push_back(std::move(s)); } }
        // 按语言:语言无关放内容文件夹根(tbl\、dat\、table\);语言专属放其语言子文件夹(tbl\sc\、tbl\tc\、tbl\kr\ 等)。
        // 同一内容内顺序:先共享(lang="")后各语言;mod 顺序仍是首要优先级。
        const char* kLangSub[] = { "", "sc", "tc", "kr" };
        for (const char* Lc : kLangSub) {
            std::string lang = Lc;
            std::string pre = lang.empty() ? std::string() : ("/" + lang);   // off/相对名前缀(""或"/sc")
            // tbl[/<lang>]/*.json(lang="" 迭代 tbl\ 时,子目录 sc/tc/kr 因非普通文件被跳过)
            { fs::path d = lang.empty() ? (dir / "tbl") : (dir / "tbl" / lang);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> tfs;
                  for (const auto& tf : fs::directory_iterator(d, ec))
                      if (tf.is_regular_file() && tf.path().extension() == ".json"
                          && !off("tbl" + pre + "/" + tf.path().filename().generic_string())) tfs.push_back(tf.path());
                  std::sort(tfs.begin(), tfs.end());
                  for (auto& t : tfs) { std::string rel = "tbl" + pre + "/" + t.filename().generic_string();
                      tblDirFiles.push_back({ t, entryOffSet(rel), lang, m.name }); } } }
            // dat[/<lang>]/*.dat 整文件替换原脚本(对话):中性→script、语言→script_<L>
            { fs::path d = lang.empty() ? (dir / "dat") : (dir / "dat" / lang);
              if (fs::is_directory(d, ec)) {
                  std::vector<fs::path> dfs;
                  for (const auto& df : fs::directory_iterator(d, ec))
                      if (df.is_regular_file() && df.path().extension() == ".dat"
                          && !off("dat" + pre + "/" + df.path().filename().generic_string())) dfs.push_back(df.path());
                  std::sort(dfs.begin(), dfs.end());
                  for (auto& dd : dfs) datReplaceFiles.push_back({ std::move(dd), m.name, lang }); } }
            // table[/<lang>]/*.tbl 整文件替换:中性→table、语言→table_<L>
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
    // 注:即使无启用内容也继续往下——以便清理上次残留 + 重写空注入表/报告/指纹(= 回到原版状态)。
    if (patchFiles.empty() && injectFiles.empty() && tblDirFiles.empty() && datReplaceFiles.empty() && tableReplaceFiles.empty() && assetFiles.empty())
        log += "[modkit] (no enabled mod content -> producing clean/vanilla cache)\n";

    // 多语言:探测游戏存在哪些语言 tbl pac(table_sc/tc/kr.pac),对每个语言各产一份合并表。
    std::vector<std::string> langs;
    for (const char* L : { "sc", "tc", "kr" })
        if (fs::exists(fs::path(paths.pacSteamDir) / (std::string("table_") + L + ".pac"), ec)) langs.push_back(L);
    if (langs.empty()) langs.push_back("sc");   // 兜底(理论上至少有一个)
    std::string primaryLang = (std::find(langs.begin(), langs.end(), "sc") != langs.end()) ? "sc" : langs[0];

    // 2) 指纹(清单顺序+启停 + 所有 patch/inject/tbl 路径+内容 + 所有 mod dat 内容 + 语言集)
    std::string fpInput;
    { fpInput += "LANGS:"; for (const auto& L : langs) { fpInput += L; fpInput += ","; } fpInput += "\n"; }
    for (const auto& m : manifest) {
        fpInput += "MOD:"; fpInput += m.name; fpInput += m.enabled ? "=1" : "=0";
        for (const auto& d : m.disabled) { fpInput += ";off:"; fpInput += d; }   // 逐组件开关进指纹
        fpInput += "\n";
    }
    for (const auto& pf : patchFiles) { fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : injectFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : tblDirFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : datReplaceFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& pf : tableReplaceFiles) { fpInput += pf.lang; fpInput += "|"; fpInput += pf.path.generic_string(); fpInput += "\n"; fpInput += readAll(pf.path); fpInput += "\n"; }
    for (const auto& df : datFiles) { fpInput += df.generic_string(); fpInput += "\n"; fpInput += readAll(df); fpInput += "\n"; }
    // asset 透传:用 路径+大小+修改时间 入指纹(资源可能很大,不读内容)
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

    // 清理上次合并产物(禁用/删除/改名 mod 后避免残留 → 始终从 pac + 当前启用 mod 全量重建)。
    { std::error_code ce;
      std::set<std::string> clearDirs = { "table", "table_sc", "table_tc", "table_kr",
        "scene", "script", "script_sc", "script_tc", "script_kr", "asset" };
      // redirect\ 透传可写入任意顶层命名空间(voice/se/bgm/...);把本次涉及的顶层目录也纳入清理。
      for (const auto& a : assetFiles) {
          auto pos = a.rel.find('/');
          if (pos != std::string::npos) clearDirs.insert(a.rel.substr(0, pos));
      }
      for (const auto& sub : clearDirs) fs::remove_all(fs::path(paths.cacheDir) / sub, ce); }

    // 机器可读报告(供 GUI / mod 管理器);随各步骤填充,末尾写 cache\merge_report.json。
    nlohmann::json report;
    report["version"] = 1;
    report["mods"] = nlohmann::json::array();
    report["errors"] = nlohmann::json::array();
    for (const ModInfo& m : manifest) report["mods"].push_back({ {"name", m.name}, {"enabled", m.enabled} });
    // 记一条错误(带 mod 归属,供 GUI 显示「哪个 mod 出错」);mods 空 = 全局错误。
    auto pushErr = [&](const std::string& where, const std::vector<std::string>& mods, const std::string& detail) {
        nlohmann::json e; e["where"] = where; e["detail"] = detail; e["mods"] = nlohmann::json::array();
        for (const auto& m : mods) e["mods"].push_back(m);
        report["errors"].push_back(std::move(e));
    };

    // 3) 加载配置,按 scene target 分组(累加 add_actors);记录每个 target 来自哪些 mod(错误归属)
    std::map<std::string, PatchConfig> byTarget;
    std::map<std::string, std::set<std::string>> targetMods;
    for (const auto& pf : patchFiles) {
        const std::string& mod = pf.mod;
        PatchConfig cfg; std::string err;
        if (!LoadPatchConfig(pf.path.wstring(), cfg, err)) {
            ++r.failed; log += "[modkit] load FAIL " + pf.path.generic_string() + ": " + err + "\n";
            pushErr("scene_add_json/" + pf.path.filename().generic_string(), { mod }, err); continue;
        }
        if (cfg.target.empty()) cfg.target = pf.path.stem().string();   // 文件名=目标场景(json 可不写 target)
        PatchConfig& m = byTarget[cfg.target];
        m.target = cfg.target;
        if (m.map.empty()) m.map = cfg.map;
        for (auto& a : cfg.addActors) m.addActors.push_back(a);
        targetMods[cfg.target].insert(mod);
    }

    // 4) 打开 scene.pac(仅当有 scene target;纯 script_inject mod 无需 pac)。失败不再中止整轮,tbl/inject 仍继续。
    FpacReader pacScene;
    bool sceneOk = true;
    if (!byTarget.empty() && !pacScene.Open(fs::path(paths.pacSteamDir) / "scene.pac")) {
        sceneOk = false; ++r.failed; log += "[modkit] cannot open scene.pac\n";
        pushErr("scene.pac", {}, "无法打开 scene.pac(检查游戏 pac\\steam)");
    }

    // 5) 逐 target 合并 scene + 累加 tbl 钩子
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

    // (旧 tbl_patch.json 直配钩子已弃用;t_lookpoint 改用 Mod\<mod>\tbl\t_lookpoint.json)

    // 6) 通用 tbl 合并(全走 schema 驱动引擎;t_lookpoint 与专用编码器字节一致)。
    //    多语言:对每个存在的语言 L,从 table_<L>.pac 读原表 → 套(共享层 tbl\ + 语言层 tbl_<L>\)→
    //    写 cache\merged\table_<L>\<表>.tbl。运行时 SceneRedirect 按游戏当前语言取对应那份。
    //    累积器 key = 目标 tbl 名(如 "t_lookpoint"),值 = 表头名 + add_rows + edit_rows。
    struct TblAccum { std::string table; std::vector<nlohmann::json> addRows; std::vector<tbl_merge::EditOp> edits; std::vector<nlohmann::json> cloneRows; std::set<std::string> mods; };
    report["tables"] = nlohmann::json::array();

    if (!allHooks.empty() || !tblDirFiles.empty()) {
        for (const std::string& L : langs) {
            const bool isPrimary = (L == primaryLang);   // 报告/冲突/计数只在主语言记一次,避免 ×语言数
            // 为语言 L 累积:scene 钩子(共享)+ tbl\(共享)+ tbl_<L>\(本语言),按收集顺序(mod 顺序;组内先共享后语言)
            std::map<std::string, TblAccum> tblAcc;
            for (const LpRow& h : allHooks) {
                nlohmann::json row;
                row["text1"] = h.text1; row["text2"] = h.text2; row["text3"] = h.text3; row["empty"] = h.empty;
                row["arr1"] = h.arr1; row["uint1"] = h.uint1; row["arr2"] = h.arr2; row["uint2"] = h.uint2;
                tblAcc["t_lookpoint"].addRows.push_back(std::move(row));
            }
            for (const auto& tf : tblDirFiles) {
                if (!(tf.lang.empty() || tf.lang == L)) continue;   // 只取共享层 + 本语言层
                std::string stem = tf.path.stem().string();
                const std::string& modName = tf.mod;                // 显式标签(路径深度因 <lang>\ 子层不可靠)
                TblAccum& a = tblAcc[stem];
                parseTblFile(tf.path, a.table, a.addRows, a.edits, a.cloneRows, modName, tf.off, log);
                a.mods.insert(modName);
            }
            if (tblAcc.empty()) continue;

            const std::string pacName  = "table_" + L + ".pac";
            const std::string entryPre = "table_" + L + "/";
            const std::string outSub   = "table_" + L;         // cache\merged\table_<L>\<表>.tbl

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
                    // 池感知克隆(t_npc 等带池表):不解码,保池 + 偏移平移
                    if (!kv.second.addRows.empty() || !kv.second.edits.empty()) {
                        ++r.failed; log += "[modkit] tbl FAIL " + stem + ": clone_rows 不能与 add_rows/edit_rows 混用\n"; if (isPrimary) pushErr(stem + ".tbl", tmods, "clone_rows 不能与 add_rows/edit_rows 混用"); continue;
                    }
                    okTbl = tbl_merge::CloneRowsPoolTable(origTbl, paths.schemasDir, "Sora1", kv.second.table, kv.second.cloneRows, outBytes, e2);
                    if (!okTbl) { ++r.failed; log += "[modkit] tbl clone FAIL " + stem + ": " + e2 + "\n"; if (isPrimary) pushErr(stem + ".tbl", tmods, e2); continue; }
                } else {
                    // 护栏:带未建模池的表(如 t_npc)走通用合并会丢池→损坏,直接拒绝
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

                if (!isPrimary) continue;   // 报告条目/冲突只在主语言记一次
                // 报告条目 + 冲突写入 log(同行同字段被多 mod 写成不同值)
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

    // 6b) script_inject:生成「map -> script」注入表(供 ScriptInject 读)。对话 dat 双源:
    //     ① 根 Mod\<mod>\<script>.dat → 语言中性 cache\script\<script>.dat(向后兼容,全语言通用);
    //     ② Mod\<mod>\dat\[<lang>\]<script>.dat → 由 6c 拷到 cache\script[_<lang>]\<script>.dat(分语言,运行时按语言取)。
    //     任一来源存在即登记;都没有就不登记(避免引擎去加载缺失脚本)。
    {
        std::string table = "# auto-generated by modkit: <map>\\t<script>\n";  // 始终重写(清除已移除 mod 的残留)
        std::set<std::string> listSeen;   // (map\tscript)去重
        report["inject"] = nlohmann::json::array();
        for (const auto& jf : injectFiles) {
            fs::path moddir = jf.path.parent_path();
            for (const auto& e : parseScriptInject(jf.path, jf.mod, jf.lang, log)) {
                bool have = false;
                // ① 根 <script>.dat(e.datPath=默认 <script>.dat)→ 语言中性 script 目录
                std::string s = readAll(e.datPath);
                if (!s.empty()) {
                    std::vector<uint8_t> bytes(s.begin(), s.end());
                    if (writeAll(fs::path(paths.cacheDir) / "script" / (e.script + ".dat"), bytes)) have = true;
                }
                // ② dat\[<lang>\]<script>.dat(语言版,内容由 6c 拷)
                if (fs::exists(moddir / "dat" / (e.script + ".dat"))) have = true;
                for (const char* L : { "sc", "tc", "kr" })
                    if (fs::exists(moddir / "dat" / L / (e.script + ".dat"))) have = true;
                if (!have) {
                    log += "[modkit] add_dat_ini WARN: \"" + e.script + "\" 无对话 dat(放 Mod\\" + e.mod + "\\dat\\<lang>\\" + e.script + ".dat)\n";
                    pushErr("add_dat_ini \"" + e.script + "\"", { e.mod }, "缺对话 dat:" + e.script + ".dat");
                    continue;   // 不登记 → 引擎不会加载缺失脚本
                }
                ++r.injected;
                std::string key = e.map + "\t" + e.script;
                if (listSeen.insert(key).second) { table += key; table += "\n"; }
                report["inject"].push_back({ {"map", e.map}, {"script", e.script}, {"mod", e.mod} });
                log += "[modkit] add_dat_ini " + e.mod + ": map \"" + e.map + "\" -> \"" + e.script + ".dat\"\n";
            }
        }
        // 表放 cache\ 根(merged 的上一级),不被 SceneRedirect 当资源提供
        fs::path tablePath = fs::path(paths.cacheDir).parent_path() / "script_inject.list";
        writeText(tablePath, table);
    }

    // 6b-2) mon_load:生成「map -> 怪物表名」表(供 ScriptInject 进图同步载入 t_mon,不动主 dat)。
    //       怪物表本体仍由 table\ 通道提供(整文件替换→cache\merged\table_<L>);此处只登记加载规则。
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

    // 6b-3) mon_event:生成「map -> 事件函数名」表(供 ScriptInject 进图后 spawn 事件线程跑 map_load_mons_table)。
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

    // 6c) dat 替换:Mod\<mod>\[<lang>\]dat\<原名>.dat 整文件替换原版脚本(引擎本就 Open 原名,故无需注入)。
    //     中性→cache\script\;语言→cache\script_<L>\(运行时按语言取)。冲突按 (语言,名) 分桶,加载顺序靠后者胜。
    {
        report["datReplace"] = nlohmann::json::array();
        report["datConflicts"] = nlohmann::json::array();
        struct Prov { std::string mod; fs::path path; };
        std::map<std::string, std::vector<Prov>> byKey;  // "<lang>/<stem>" -> [(mod,path)] 按加载顺序
        std::vector<std::string> order;
        for (const auto& df : datReplaceFiles) {
            std::string stem = df.path.stem().string();
            std::string key = df.lang + "/" + stem;        // 不同语言 = 不同输出,不互相冲突
            if (byKey.find(key) == byKey.end()) order.push_back(key);
            byKey[key].push_back({ df.mod, df.path });
        }
        for (const std::string& key : order) {
            auto& provs = byKey[key];
            std::string lang = key.substr(0, key.find('/'));
            std::string stem = key.substr(key.find('/') + 1);
            std::string sub = lang.empty() ? "script" : ("script_" + lang);
            std::string tag = lang.empty() ? "" : ("[" + lang + "]");
            const auto& win = provs.back();  // 最后 = 胜
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

    // 6d) 整文件替换 tbl:Mod\<mod>\[<lang>\]table\<表>.tbl 直接覆盖 cache\merged\table[_<lang>]\<表>.tbl(不解码,适合带未建模池的表如 t_npc)。
    //     中性→table\(运行时回退命中);语言→table_<L>\。冲突按 (语言,名) 分桶。
    {
        report["tableReplace"] = nlohmann::json::array();
        struct Prov { std::string mod; fs::path path; };
        std::map<std::string, std::vector<Prov>> byKey;  // "<lang>/<stem>"
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

    // 6e) asset 透传:Mod\<mod>\asset\** 原样复制到 cache\merged\asset\**(模型/贴图/.mi 覆盖或新增)。
    //     同一 pac 相对名被多 mod 提供 = 整文件覆盖,加载顺序靠后者胜;SceneRedirect 按名加载。
    {
        report["assets"] = nlohmann::json::array();
        report["assetConflicts"] = nlohmann::json::array();
        std::map<std::string, std::vector<std::pair<std::string, fs::path>>> byRel;  // rel -> [(mod,src)] 按加载顺序
        std::vector<std::string> order;
        for (const auto& a : assetFiles) {
            if (byRel.find(a.rel) == byRel.end()) order.push_back(a.rel);
            byRel[a.rel].push_back({ a.mod, a.src });
        }
        for (const std::string& rel : order) {
            auto& provs = byRel[rel];
            const auto& win = provs.back();  // 最后 = 胜
            fs::path dst = fs::path(paths.cacheDir) / fs::path(rel);   // rel 用 '/',fs::path 自动适配
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

    // 7) 写报告(机器可读,供 GUI / mod 管理器)+ 指纹
    report["summary"] = { {"merged", r.merged}, {"tbls", r.tbls}, {"injected", r.injected},
                          {"assets", r.assets}, {"failed", r.failed}, {"conflicts", r.conflicts} };
    writeText(fs::path(paths.cacheDir).parent_path() / "merge_report.json", report.dump(2));
    writeText(fpFile, fp);
    log += "[modkit] done: merged=" + std::to_string(r.merged) + " tbls=" + std::to_string(r.tbls)
        + " injected=" + std::to_string(r.injected) + " assets=" + std::to_string(r.assets)
        + " failed=" + std::to_string(r.failed)
        + " conflicts=" + std::to_string(r.conflicts) + " mods=" + std::to_string(r.mods);
    r.log = log;
    return r;
}

} // namespace orchestrator
} // namespace modkit
} // namespace ed9loader
