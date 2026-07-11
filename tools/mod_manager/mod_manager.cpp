#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "modkit/mod_merge_orchestrator.h"
#include "modkit/generic_tbl.h"
#include "ed9_dat.hpp"
#include "json.hpp"

#include <d3d11.h>
#include <wincodec.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tchar.h>
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace orch = ed9loader::modkit::orchestrator;
namespace mk = ed9loader::modkit;
using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

static float g_dpiScale = 1.0f;

static std::string ws2utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring utf82ws(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static bool readJson(const fs::path& p, json& j) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    try { f >> j; } catch (...) { return false; }
    return true;
}

struct ModComp {
    std::string file;
    std::string rel;
    std::string kind;
    std::string label;
    std::string detail;
    std::string disp;
};

struct AssetRow {
    std::string rel;
    std::vector<std::string> providers;
    bool conflict = false;
    std::string winner;
};

struct App {
    char gameDir[1024] = {};
    std::vector<orch::ModInfo> mods;
    json report;
    bool hasReport = false;
    std::string status = "就绪。设置游戏目录后会自动扫描,并持续监听 Mod 目录变动。";
    std::string mergeLog;
    bool busy = false;
    int funcView = 0;
    bool showSettings = false;
    int settingsCat = 0;
    int selMod = -1;
    int rightView = 1;
    float viewToggleAnim = 0.0f;
    bool  viewToggleDragging = false;
    float funcSegAnim = 0.0f;  bool funcSegDrag = false;
    float convSegAnim = 0.0f;  bool convSegDrag = false;
    float datSegAnim  = 0.0f;  bool datSegDrag  = false;
    std::map<std::string, float> btnAnim;
    std::vector<ModComp> comps;
    std::string compsFor;
    std::string cfgFile;
    int cfgHoverIdx = -1;
    std::vector<float> cfgAnim;
    float cfgBackAnim = 0.0f;
    float cfgAllOnAnim = 0.0f, cfgAllOffAnim = 0.0f;
    float modAllOnAnim = 0.0f, modAllOffAnim = 0.0f;
    int   cfgLastFrame = -1;
    std::vector<AssetRow> selAssets;
    std::set<std::string> conflictMods;
    std::set<std::string> errorMods;
    std::string conflictSig;
    int convMode = 0;
    std::vector<std::wstring> convTbls;
    char convOutDir[1024] = {};
    std::string convStatus;
    int datConvMode = 0;
    std::vector<std::wstring> datFiles;
    char datOutDir[1024] = {};
    std::string datStatus;
    HANDLE watchH = INVALID_HANDLE_VALUE;
    std::wstring watchDir;
    unsigned long long watchRetryAt = 0;
    unsigned long long rescanAt = 0;
};

static fs::path exeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}
static fs::path iniPath() { return exeDir() / "mod_manager.ini"; }

static std::map<std::string, std::string> g_tr;
static std::string g_langCode = "zh_CN";
static fs::path langDir() { return exeDir() / "language"; }
static const char* T(const char* s) {
    auto it = g_tr.find(s);
    return it != g_tr.end() ? it->second.c_str() : s;
}
static void loadLanguage(const std::string& code) {
    g_tr.clear(); g_langCode = code.empty() ? "zh_CN" : code;
    json j;
    if (readJson(langDir() / (g_langCode + ".json"), j) && j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!it.key().empty() && it.key()[0] != '_' && it.value().is_string())
                g_tr[it.key()] = it.value().get<std::string>();
}
static std::vector<std::pair<std::string, std::string>> scanLanguages() {
    std::vector<std::pair<std::string, std::string>> out;
    std::error_code ec;
    if (fs::is_directory(langDir(), ec))
        for (const auto& e : fs::directory_iterator(langDir(), ec))
            if (e.is_regular_file() && e.path().extension() == L".json") {
                std::string code = ws2utf8(e.path().stem().wstring());
                std::string name = code; json j;
                if (readJson(e.path(), j) && j.contains("_name") && j["_name"].is_string()) name = j["_name"].get<std::string>();
                out.push_back({ code, name });
            }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    return out;
}

static std::string trErr(const std::string& s) {
    if (s.empty()) return s;
    const char* t = T(s.c_str());
    if (t != s.c_str()) return t;
    static const char* frags[] = {
        "缺对话 dat:", "无法打开 ", "复制失败: ", "dat 替换 ", "table 替换 ", "(检查游戏 pac\\steam)"
    };
    std::string out = s;
    for (const char* fr : frags) {
        const char* tr = T(fr);
        if (tr == fr) continue;
        size_t flen = std::strlen(fr), p = 0;
        while ((p = out.find(fr, p)) != std::string::npos) { out.replace(p, flen, tr); p += std::strlen(tr); }
    }
    return out;
}

static void loadIniGameDir(App& a) {
    std::ifstream f(iniPath(), std::ios::binary);
    std::string line, lang;
    bool gotDir = false;
    while (f && std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("game_dir=", 0) == 0) { std::string v = line.substr(9); if (!v.empty()) { strncpy_s(a.gameDir, v.c_str(), _TRUNCATE); gotDir = true; } }
        else if (line.rfind("lang=", 0) == 0) { lang = line.substr(5); }
    }
    if (!gotDir) {
        fs::path ed = exeDir();
        if (ed.filename() == L"ED9Loader") { std::string g = ws2utf8(ed.parent_path().wstring()); strncpy_s(a.gameDir, g.c_str(), _TRUNCATE); }
    }
    loadLanguage(lang);
}
static void saveIniGameDir(const App& a) {
    std::ofstream f(iniPath(), std::ios::binary);
    if (f) { f << "game_dir=" << a.gameDir << "\n"; f << "lang=" << g_langCode << "\n"; }
}

static fs::path modsDirOf(const App& a) { return fs::path(utf82ws(a.gameDir)) / L"Mod"; }
static fs::path reportPathOf(const App& a) { return fs::path(utf82ws(a.gameDir)) / L"ED9Loader" / L"cache" / L"merge_report.json"; }

static void loadReport(App& a) {
    a.hasReport = readJson(reportPathOf(a), a.report);
}
static void refresh(App& a) {
    if (a.gameDir[0] == 0) { a.status = "请先填游戏目录。"; return; }
    a.mods = orch::ScanMods(modsDirOf(a).wstring());
    a.selMod = -1; a.compsFor.clear(); a.comps.clear();
    a.conflictSig.clear();
    loadReport(a);
    a.status = "已扫描:" + std::to_string(a.mods.size()) + " 个 mod";
}

static void closeWatch(App& a) {
    if (a.watchH != INVALID_HANDLE_VALUE) { FindCloseChangeNotification(a.watchH); a.watchH = INVALID_HANDLE_VALUE; }
}

static void autoRescan(App& a) {
    if (a.gameDir[0] == 0) return;
    std::string selName = (a.selMod >= 0 && a.selMod < (int)a.mods.size()) ? a.mods[a.selMod].name : "";

    std::vector<orch::ModInfo> disk = orch::ScanMods(modsDirOf(a).wstring());
    std::set<std::string> diskNames; for (auto& d : disk) diskNames.insert(d.name);

    std::vector<orch::ModInfo> merged; std::set<std::string> kept;
    for (auto& m : a.mods) if (diskNames.count(m.name)) { merged.push_back(m); kept.insert(m.name); }
    for (auto& d : disk)  if (!kept.count(d.name)) merged.push_back(d);

    bool changed = merged.size() != a.mods.size();
    if (!changed) for (size_t i = 0; i < merged.size(); ++i) if (merged[i].name != a.mods[i].name) { changed = true; break; }

    a.mods.swap(merged);
    a.selMod = -1;
    if (!selName.empty())
        for (int i = 0; i < (int)a.mods.size(); ++i) if (a.mods[i].name == selName) { a.selMod = i; break; }

    a.compsFor.clear();
    a.conflictSig.clear();
    loadReport(a);
    if (changed) a.status = "检测到 Mod 目录变动,已自动重扫:" + std::to_string(a.mods.size()) + " 个 mod";
}

static void pollWatch(App& a) {
    if (a.gameDir[0]) {
        std::wstring want = modsDirOf(a).wstring();
        if (want != a.watchDir) { closeWatch(a); a.watchDir = want; a.watchRetryAt = 0; }
        if (a.watchH == INVALID_HANDLE_VALUE && GetTickCount64() >= a.watchRetryAt) {
            std::error_code ec;
            if (fs::is_directory(a.watchDir, ec)) {
                a.watchH = FindFirstChangeNotificationW(
                    a.watchDir.c_str(), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
            }
            a.watchRetryAt = GetTickCount64() + 2000;
        }
    } else if (a.watchH != INVALID_HANDLE_VALUE || !a.watchDir.empty()) {
        closeWatch(a); a.watchDir.clear();
    }

    if (a.watchH != INVALID_HANDLE_VALUE && WaitForSingleObject(a.watchH, 0) == WAIT_OBJECT_0) {
        a.rescanAt = GetTickCount64() + 400;
        FindNextChangeNotification(a.watchH);
    }
    if (a.rescanAt && GetTickCount64() >= a.rescanAt) { a.rescanAt = 0; autoRescan(a); }
}

static bool compDisabled(const orch::ModInfo& m, const std::string& rel) {
    return std::find(m.disabled.begin(), m.disabled.end(), rel) != m.disabled.end();
}
static void setCompEnabled(orch::ModInfo& m, const std::string& rel, bool on) {
    auto it = std::find(m.disabled.begin(), m.disabled.end(), rel);
    if (on) { if (it != m.disabled.end()) m.disabled.erase(it); }
    else    { if (it == m.disabled.end()) m.disabled.push_back(rel); }
}

static std::string briefJson(const json& j, int maxItems = 3) {
    if (j.is_string()) return j.get<std::string>();
    if (!j.is_object()) return j.dump();
    std::string s = "{"; int n = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.key().empty() && it.key()[0] == '_') continue;
        if (n >= maxItems) { s += ", …"; break; }
        if (n++) s += ", ";
        s += it.key() + "=" + (it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
    }
    return s + "}";
}
static std::string entryNote(const json& e) {
    if (e.is_object() && e.contains("_note") && e["_note"].is_string()) return e["_note"].get<std::string>();
    return {};
}

static std::string friendlyTableName(const std::string& schema) {
    static const std::map<std::string, std::string> kMap = {
        {"VoiceTableData","语音配置"}, {"ActiveVoiceTableData","主动语音配置"}, {"BTLVoiceTable","战斗语音配置"}, {"QuestReportVoice","任务汇报语音"},
        {"BGMTableData","BGM配置"}, {"MapBGM","地图BGM"}, {"BattleBGM","战斗BGM"}, {"SETableData","音效配置"},
        {"StatusParam","单位配置"}, {"NameTableData","单位名称"}, {"MonsterSettingParam","怪物配置"}, {"ChrDataParam","角色数据配置"},
        {"CharaSettingInfo","角色设置"}, {"TalkChrData","对话角色配置"}, {"NPCParam","NPC配置"},
        {"SkillParam","技能配置"}, {"ArtsParam","魔法配置"}, {"SkillRangeData","技能范围配置"}, {"SkillGetParam","技能习得配置"},
        {"SupportAbilityParam","支援技能配置"}, {"AITypeList","AI类型配置"}, {"OrbmentSlotParam","导力盘配置"}, {"QuartzParam","导力器配置"},
        {"AttrData","属性配置"}, {"ConditionInfoTableData","状态异常配置"},
        {"ItemTableData","物品配置"}, {"ItemKindParam2","物品种类配置"}, {"CostumeParam","服装配置"}, {"TradeItem","交易物品"},
        {"ShopInfo","商店信息"}, {"ShopItem","商店物品"}, {"ShopConv","商店兑换"},
        {"LookPointTableData","视点配置"}, {"BreakObjectTableData","可破坏物配置"}, {"TBoxParam","宝箱配置"}, {"FieldItemTableData","场景物品配置"},
        {"EventBoxTableData","事件区配置"}, {"PlaceTableData","地点配置"}, {"MapJumpAreaData","地图跳转区"}, {"MapJumpSpotData","地图跳转点"},
        {"MarkerTableData","地图标记"}, {"SceneCommonSetting","场景通用设置"}, {"SceneSkySetting","场景天空设置"},
        {"EventTableData","事件配置"}, {"EventGroupData","事件组配置"}, {"EventSubGroupData","事件子组配置"}, {"ChapterParam","章节配置"},
        {"TextTableData","文本配置"}, {"NaviText","导航文本"}, {"LogText","日志文本"}, {"TipsTableData","提示配置"},
        {"QuestText","任务文本"}, {"QuestTitle","任务标题"}, {"QuestRank","任务评级"}, {"QuestChapterRank","任务章节评级"},
        {"EffectTableData","特效配置"}, {"FaceAnimeData","表情动画配置"}, {"PortraitDataParam","头像配置"}, {"PopupFaceParam","弹出头像配置"},
        {"GraphicsPresetTableData","画质预设"}, {"AniParam","动画配置"}, {"TitleTableData","称号配置"}, {"HelpPage","帮助页配置"}, {"HelpTitle","帮助标题"},
        {"AchievementTableData","成就配置"}, {"AchievementCategoryData","成就分类"}, {"BooksTitle","书籍标题"}, {"BooksText","书籍内容"}, {"BooksCategory","书籍分类"},
        {"ConstantValue","常量配置"}, {"DLCTableData","DLC配置"},
    };
    auto it = kMap.find(schema);
    return it != kMap.end() ? it->second : schema;
}

static void expandTblJson(const fs::path& p, const std::string& relFile, const std::string& groupPrefix, std::vector<ModComp>& out) {
    json j; if (!readJson(p, j)) {
        out.push_back({ groupPrefix + ws2utf8(p.stem().wstring()) + " (解析失败)", relFile, "改", "(JSON 解析失败)", "", groupPrefix + ws2utf8(p.stem().wstring()) + " (解析失败)" });
        return;
    }
    std::string table = j.value("table", std::string());
    std::string stem = ws2utf8(p.stem().wstring());
    std::string fr = friendlyTableName(table);
    std::string file = groupPrefix + stem + (table.empty() ? "" : " · " + fr);
    std::string disp = groupPrefix + (table.empty() ? stem : fr);
    struct Sec { const char* key; const char* kind; };
    for (const Sec& s : { Sec{"edit_rows","改"}, Sec{"add_rows","加"}, Sec{"clone_rows","克隆"}, Sec{"add_npc","NPC"} }) {
        if (!j.contains(s.key) || !j[s.key].is_array()) continue;
        const auto& arr = j[s.key];
        for (size_t i = 0; i < arr.size(); ++i) {
            const json& e = arr[i];
            std::string note = entryNote(e), label, detail;
            if (std::string(s.key) == "edit_rows") {
                label = note.empty() ? briefJson(e.value("match", json::object())) : note;
                detail = briefJson(e.value("match", json::object())) + " -> " + briefJson(e.value("set", json::object()));
            } else if (std::string(s.key) == "add_npc") {
                label = note.empty() ? (e.is_object() && e.contains("name") ? briefJson(e["name"]) : "新 NPC") : note;
                detail = briefJson(e);
            } else {
                label = note.empty() ? briefJson(e) : note;
                detail = note.empty() ? "" : briefJson(e);
            }
            out.push_back({ file, relFile + "#" + s.key + "/" + std::to_string(i), s.kind, label, detail, disp });
        }
    }
}

static std::vector<ModComp> scanModComponents(const App& a, const std::string& modName) {
    std::vector<ModComp> out;
    std::error_code ec;
    struct Folder { const wchar_t* sub; const char* rel; const char* label; };
    static const Folder kFolders[] = {
        { L"tbl",    "tbl",    "" },
        { L"tbl/sc", "tbl/sc", "[简中] " },
        { L"tbl/tc", "tbl/tc", "[繁中] " },
        { L"tbl/kr", "tbl/kr", "[韩] " },
    };
    for (const Folder& f : kFolders) {
        fs::path d = modsDirOf(a) / utf82ws(modName) / f.sub;
        if (!fs::is_directory(d, ec)) continue;
        std::vector<fs::path> tfs;
        for (const auto& e : fs::directory_iterator(d, ec))
            if (e.is_regular_file() && e.path().extension() == L".json") tfs.push_back(e.path());
        std::sort(tfs.begin(), tfs.end());
        for (auto& p : tfs)
            expandTblJson(p, std::string(f.rel) + "/" + p.filename().generic_string(), T(f.label), out);
    }
    return out;
}

static std::vector<std::string> scanModAssetRels(const App& a, const std::string& modName) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path assetDir = modsDirOf(a) / utf82ws(modName) / L"asset";
    if (!fs::is_directory(assetDir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(assetDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        out.push_back("asset/" + fs::relative(it->path(), assetDir, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static void computeSelAssets(App& a, const std::string& modName) {
    a.selAssets.clear();
    std::map<std::string, std::vector<std::string>> prov;
    for (const auto& mm : a.mods) {
        if (!mm.enabled) continue;
        for (const auto& rel : scanModAssetRels(a, mm.name)) prov[rel].push_back(mm.name);
    }
    for (const auto& rel : scanModAssetRels(a, modName)) {
        AssetRow ar; ar.rel = rel;
        auto it = prov.find(rel);
        if (it != prov.end()) ar.providers = it->second;
        ar.conflict = ar.providers.size() > 1;
        ar.winner = ar.providers.empty() ? std::string() : ar.providers.back();
        a.selAssets.push_back(std::move(ar));
    }
}

static void updateLeftIndicators(App& a) {
    a.errorMods.clear();
    if (a.hasReport)
        for (const auto& e : a.report.value("errors", json::array()))
            for (const auto& mn : e.value("mods", json::array()))
                if (mn.is_string()) a.errorMods.insert(mn.get<std::string>());
    std::string sig;
    for (const auto& m : a.mods) { sig += m.name; sig += m.enabled ? "1" : "0"; sig += ";"; }
    if (sig != a.conflictSig) {
        a.conflictSig = sig;
        a.conflictMods.clear();
        std::map<std::string, std::vector<std::string>> prov;
        for (const auto& m : a.mods) {
            if (!m.enabled) continue;
            for (const auto& rel : scanModAssetRels(a, m.name)) prov[rel].push_back(m.name);
        }
        for (const auto& kv : prov)
            if (kv.second.size() > 1) for (const auto& mn : kv.second) a.conflictMods.insert(mn);
    }
}

static void doMerge(App& a) {
    if (a.gameDir[0] == 0) { a.status = "请先填游戏目录。"; return; }
    if (!orch::SaveMods(modsDirOf(a).wstring(), a.mods)) { a.status = "写 mods.json 失败(目录不可写?)"; return; }
    orch::Paths paths = orch::FromGameDir(utf82ws(a.gameDir));
    orch::RunResult r = orch::Run(paths, true);
    a.mergeLog = r.log;
    loadReport(a);
    a.status = "保存成功";
}

static bool browseFolder(std::wstring& out, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0; dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (title) dlg->SetTitle(title);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { out = path; CoTaskMemFree(path); ok = true; }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

static bool browseFilesMulti(std::vector<std::wstring>& out,
                             const wchar_t* filter = L"tbl 文件 (*.tbl)\0*.tbl\0所有文件 (*.*)\0*.*\0",
                             const wchar_t* title = L"选择要转换的 tbl(可多选)") {
    static wchar_t buf[16384];
    buf[0] = 0;
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(wchar_t));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out.clear();
    std::wstring dir = buf;
    wchar_t* p = buf + dir.size() + 1;
    if (*p == 0) out.push_back(dir);
    else for (; *p; p += wcslen(p) + 1) out.push_back(dir + L"\\" + p);
    return !out.empty();
}

static ojson tblValueToJson(const mk::TblValue& v) {
    using K = mk::TblValue::K;
    switch (v.kind) {
        case K::Int: return v.i;
        case K::Flt: return v.f;
        case K::Str: return v.s;
        case K::Arr: { ojson a = ojson::array(); for (auto x : v.arr) a.push_back(x); return a; }
        default:     { ojson a = ojson::array(); for (auto b : v.raw) a.push_back((int)b); return a; }
    }
}

static ojson tblFileToJson(const mk::TblFileG& g, const std::string& stem) {
    ojson j;
    j["file"] = stem;
    j["tables"] = ojson::array();
    for (const auto& t : g.tables) {
        ojson tj;
        tj["table"] = t.name;
        tj["rows"] = ojson::array();
        for (const auto& row : t.rows) {
            ojson rj = ojson::object();
            for (const auto& kv : row.fields) rj[kv.first] = tblValueToJson(kv.second);
            tj["rows"].push_back(std::move(rj));
        }
        j["tables"].push_back(std::move(tj));
    }
    return j;
}

static void doConvert(App& a) {
    if (a.gameDir[0] == 0) { a.convStatus = T("请先设置游戏目录。"); return; }
    if (a.convTbls.empty()) { a.convStatus = T("请先选择要转换的 tbl。"); return; }
    if (a.convOutDir[0] == 0) { a.convStatus = T("请先选择导出目录。"); return; }
    std::wstring schemas = orch::FromGameDir(utf82ws(a.gameDir)).schemasDir;
    fs::path outDir = utf82ws(a.convOutDir);
    std::error_code ec; fs::create_directories(outDir, ec);
    int ok = 0, failN = 0; std::string firstErr;
    for (const auto& p : a.convTbls) {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) { ++failN; if (firstErr.empty()) firstErr = ws2utf8(fs::path(p).filename().wstring()) + ": 打不开"; continue; }
        auto sz = f.tellg(); f.seekg(0);
        std::vector<uint8_t> bytes((size_t)sz);
        if (sz > 0) f.read(reinterpret_cast<char*>(bytes.data()), sz);
        mk::TblFileG g; std::string err;
        if (!mk::DecodeTblG(bytes, schemas, "Sora1", g, err)) {
            ++failN; if (firstErr.empty()) firstErr = ws2utf8(fs::path(p).filename().wstring()) + ": " + err; continue;
        }
        ojson j = tblFileToJson(g, fs::path(p).stem().string());
        fs::path outP = outDir / (fs::path(p).stem().wstring() + L".json");
        std::ofstream o(outP, std::ios::binary);
        if (!o) { ++failN; continue; }
        o << j.dump(2);
        ++ok;
    }
    { char b[160]; snprintf(b, sizeof b, T("导出完成:成功 %d,失败 %d"), ok, failN);
      a.convStatus = std::string(b) + (firstErr.empty() ? "" : ("\n(" + firstErr + ")")); }
}

static mk::TblValue jsonToValueByType(const std::string& type, const ojson& jv) {
    using K = mk::TblValue::K;
    mk::TblValue v;
    if (mk::TblTypeIsToffset(type)) {
        v.kind = K::Str; if (jv.is_string()) v.s = jv.get<std::string>();
    } else if (mk::TblTypeIsArray(type)) {
        v.kind = K::Arr; if (jv.is_array()) for (const auto& e : jv) if (e.is_number()) v.arr.push_back((uint64_t)e.get<int64_t>());
    } else if (mk::TblTypeIsData(type)) {
        v.kind = K::Raw; if (jv.is_array()) for (const auto& e : jv) if (e.is_number()) v.raw.push_back((uint8_t)(e.get<int>() & 0xff));
    } else if (mk::TblTypeIsFloat(type)) {
        v.kind = K::Flt; if (jv.is_number()) v.f = jv.get<double>();
    } else {
        v.kind = K::Int;
        if (jv.is_number()) v.i = jv.get<int64_t>();
        else if (jv.is_boolean()) v.i = jv.get<bool>() ? 1 : 0;
    }
    return v;
}

static bool TblTypeHasUnmodeledPool(const std::string& name) {
    static const std::set<std::string> kPoolTables = {
        "NPCParam",
        "BreakObjectTableData",
        "EventBoxTableData",
        "TBoxParam",
        "CollisionFootStepInfo",
        "FieldItemTableData",
        "QuartzParam",
        "PortraitDataParam",
        "EyeAttachData",
        "EyeModifyAttachData",
        "TitleTableData",
        "GraphicsPresetTableData",
    };
    return kPoolTables.count(name) != 0;
}

static bool jsonToTblFileG(const ojson& j, const std::wstring& schemasDir, mk::TblFileG& out, std::string& err) {
    if (!j.is_object() || !j.contains("tables") || !j["tables"].is_array()) { err = "JSON 缺 tables 数组(需 TBL→JSON 导出的格式)"; return false; }
    for (const auto& tj : j["tables"]) {
        if (!tj.is_object() || !tj.contains("table") || !tj["table"].is_string()) { err = "表项缺 table 名"; return false; }
        std::string name = tj["table"].get<std::string>();
        static const ojson kEmptyArr = ojson::array();
        const ojson& rows = (tj.contains("rows") && tj["rows"].is_array()) ? tj["rows"] : kEmptyArr;
        if (TblTypeHasUnmodeledPool(name)) {
            err = "表 '" + name + "' 暂不支持回编";
            return false;
        }
        for (const auto& rj : rows) {
            if (!rj.is_object()) continue;
            for (auto it = rj.begin(); it != rj.end(); ++it) {
                const std::string& k = it.key();
                if (k.size() > 3 && k.compare(k.size() - 3, 3, "__s") == 0) {
                    err = "表 '" + name + "' 暂不支持回编";
                    return false;
                }
            }
        }
        mk::TblSchemaDef schema;
        if (!mk::ResolveTblSchema(schemasDir, name, 0, "Sora1", schema, err)) return false;
        mk::TblTableG t; t.name = name; t.schema = schema;
        for (const auto& rj : rows) {
            if (!rj.is_object()) continue;
            mk::TblRowG row;
            static const ojson kNull;
            for (const mk::TblField& f : schema.fields) {
                auto fit = rj.find(f.name);
                row.fields.emplace_back(f.name, jsonToValueByType(f.type, fit != rj.end() ? fit.value() : kNull));
            }
            t.rows.push_back(std::move(row));
        }
        out.tables.push_back(std::move(t));
    }
    if (out.tables.empty()) { err = "JSON 无有效表"; return false; }
    return true;
}

static void doConvertJsonToTbl(App& a) {
    if (a.gameDir[0] == 0) { a.convStatus = T("请先设置游戏目录。"); return; }
    if (a.convTbls.empty()) { a.convStatus = T("请先选择要转换的 json。"); return; }
    if (a.convOutDir[0] == 0) { a.convStatus = T("请先选择导出目录。"); return; }
    std::wstring schemas = orch::FromGameDir(utf82ws(a.gameDir)).schemasDir;
    fs::path outDir = utf82ws(a.convOutDir);
    std::error_code ec; fs::create_directories(outDir, ec);
    int ok = 0, failN = 0; std::string firstErr;
    for (const auto& p : a.convTbls) {
        std::string fn = ws2utf8(fs::path(p).filename().wstring());
        std::ifstream f(p, std::ios::binary);
        if (!f) { ++failN; if (firstErr.empty()) firstErr = fn + ": 打不开"; continue; }
        ojson j;
        try { j = ojson::parse(f); }
        catch (const std::exception& e) { ++failN; if (firstErr.empty()) firstErr = fn + ": JSON 解析失败(" + e.what() + ")"; continue; }
        mk::TblFileG g; std::string err;
        if (!jsonToTblFileG(j, schemas, g, err)) { ++failN; if (firstErr.empty()) firstErr = fn + ": " + err; continue; }
        std::vector<uint8_t> bytes = mk::EncodeTblG(g);
        if (bytes.empty()) { ++failN; if (firstErr.empty()) firstErr = fn + ": 编码结果为空"; continue; }
        fs::path outP = outDir / (fs::path(p).stem().wstring() + L".tbl");
        std::ofstream o(outP, std::ios::binary);
        if (!o) { ++failN; if (firstErr.empty()) firstErr = fn + ": 写出失败"; continue; }
        o.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        ++ok;
    }
    { char b[160]; snprintf(b, sizeof b, T("导出完成:成功 %d,失败 %d"), ok, failN);
      a.convStatus = std::string(b) + (firstErr.empty() ? "" : ("\n(" + firstErr + ")")); }
}

namespace datjson {
static ojson slotToJson(const ed9::Slot& s) { ojson j; if (s.isStr) j["str"] = s.str; else j["raw"] = s.raw; return j; }
static ed9::Slot jsonToSlot(const ojson& j) { ed9::Slot s; if (j.contains("str")) { s.isStr = true; s.str = j["str"].get<std::string>(); } else { s.isStr = false; s.raw = j.value("raw", 0u); } return s; }
static ojson slotsToJson(const std::vector<ed9::Slot>& v) { ojson a = ojson::array(); for (auto& s : v) a.push_back(slotToJson(s)); return a; }
static std::vector<ed9::Slot> jsonToSlots(const ojson& j) { std::vector<ed9::Slot> v; if (j.is_array()) for (auto& e : j) v.push_back(jsonToSlot(e)); return v; }

static ojson instrToJson(const ed9::Instr& in) {
    ojson j; j["op"] = in.op;
    if (in.codeOff) j["codeOff"] = in.codeOff;
    if (in.op == 0x00) { if (in.pushSize != 4) j["pushSize"] = in.pushSize; j["push"] = slotToJson(in.push); }
    if (in.i32) j["i32"] = in.i32;
    if (in.u8) j["u8"] = in.u8;
    if (in.jumpTargetOff) j["jumpTargetOff"] = in.jumpTargetOff;
    if (in.isRetAddr) { j["isRetAddr"] = true; j["retTarget"] = in.retTarget; }
    if (in.u16) j["u16"] = in.u16;
    if (!in.sA.empty()) j["sA"] = in.sA;
    if (!in.sB.empty()) j["sB"] = in.sB;
    if (in.cfsVar) j["cfsVar"] = in.cfsVar;
    if (in.op == 0x24) { j["cmdStruct"] = in.cmdStruct; j["cmdOp"] = in.cmdOp; j["cmdNArgs"] = in.cmdNArgs; }
    return j;
}
static ed9::Instr jsonToInstr(const ojson& j) {
    ed9::Instr in;
    in.op = (uint8_t)j.value("op", 0);
    in.codeOff = j.value("codeOff", 0u);
    in.pushSize = (uint8_t)j.value("pushSize", 4);
    if (j.contains("push")) in.push = jsonToSlot(j["push"]);
    in.i32 = j.value("i32", 0);
    in.u8 = (uint8_t)j.value("u8", 0);
    in.jumpTargetOff = j.value("jumpTargetOff", 0u);
    in.isRetAddr = j.value("isRetAddr", false);
    in.retTarget = j.value("retTarget", 0u);
    in.u16 = (uint16_t)j.value("u16", 0);
    in.sA = j.value("sA", std::string());
    in.sB = j.value("sB", std::string());
    in.cfsVar = (uint8_t)j.value("cfsVar", 0);
    in.cmdStruct = (uint8_t)j.value("cmdStruct", 0);
    in.cmdOp = (uint8_t)j.value("cmdOp", 0);
    in.cmdNArgs = (uint8_t)j.value("cmdNArgs", 0);
    return in;
}
static ojson structToJson(const ed9::StructDef& s) { ojson j; j["id"] = s.id; j["nb_sth1"] = s.nb_sth1; j["array2"] = slotsToJson(s.array2); return j; }
static ed9::StructDef jsonToStruct(const ojson& j) { ed9::StructDef s; s.id = j.value("id", 0); s.nb_sth1 = (uint16_t)j.value("nb_sth1", 0); if (j.contains("array2")) s.array2 = jsonToSlots(j["array2"]); return s; }
static ojson funcToJson(const ed9::Func& f) {
    ojson j;
    j["name"] = f.name; j["crc"] = f.crc; j["nin"] = f.nin; j["b0"] = f.b0; j["b1"] = f.b1; j["nout"] = f.nout;
    j["varin"] = slotsToJson(f.varin); j["varout"] = slotsToJson(f.varout);
    ojson st = ojson::array(); for (auto& s : f.structs) st.push_back(structToJson(s)); j["structs"] = st;
    j["start"] = f.start;
    ojson code = ojson::array(); for (auto& in : f.code) code.push_back(instrToJson(in)); j["code"] = code;
    return j;
}
static ed9::Func jsonToFunc(const ojson& j) {
    ed9::Func f;
    f.name = j.value("name", std::string()); f.crc = j.value("crc", 0u);
    f.nin = (uint8_t)j.value("nin", 0); f.b0 = (uint8_t)j.value("b0", 0); f.b1 = (uint8_t)j.value("b1", 0); f.nout = (uint8_t)j.value("nout", 0);
    if (j.contains("varin")) f.varin = jsonToSlots(j["varin"]);
    if (j.contains("varout")) f.varout = jsonToSlots(j["varout"]);
    if (j.contains("structs")) for (auto& e : j["structs"]) f.structs.push_back(jsonToStruct(e));
    f.start = j.value("start", 0u);
    if (j.contains("code")) for (auto& e : j["code"]) f.code.push_back(jsonToInstr(e));
    return f;
}
static ojson toJson(const ed9::Script& s, const std::string& stem) {
    ojson j; j["format"] = "ed9_dat_json_v1"; j["source"] = stem; j["name"] = s.name;
    j["scriptVarIn"] = s.nScriptVarIn; j["scriptVarOut"] = s.nScriptVarOut;
    j["scriptVars"] = slotsToJson(s.scriptVars);
    ojson fs = ojson::array(); for (auto& f : s.funcs) fs.push_back(funcToJson(f)); j["funcs"] = fs;
    return j;
}
static ed9::Script fromJson(const ojson& j) {
    ed9::Script s; s.name = j.value("name", std::string());
    s.nScriptVarIn = j.value("scriptVarIn", 0u); s.nScriptVarOut = j.value("scriptVarOut", 0u);
    if (j.contains("scriptVars")) s.scriptVars = jsonToSlots(j["scriptVars"]);
    if (j.contains("funcs")) for (auto& e : j["funcs"]) s.funcs.push_back(jsonToFunc(e));
    return s;
}
}

static void doConvertDatToJson(App& a) {
    if (a.datFiles.empty()) { a.datStatus = T("请先选择要转换的 dat。"); return; }
    if (a.datOutDir[0] == 0) { a.datStatus = T("请先选择导出目录。"); return; }
    fs::path outDir = utf82ws(a.datOutDir);
    std::error_code ec; fs::create_directories(outDir, ec);
    int ok = 0, failN = 0; std::string firstErr;
    for (const auto& p : a.datFiles) {
        std::string fn = ws2utf8(fs::path(p).filename().wstring());
        std::ifstream f(p, std::ios::binary);
        if (!f) { ++failN; if (firstErr.empty()) firstErr = fn + ": 打不开"; continue; }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
        std::string stem = ws2utf8(fs::path(p).stem().wstring());
        try {
            ed9::Script s = ed9::parse(bytes);
            if (s.name.empty()) s.name = stem;
            ojson j = datjson::toJson(s, stem);
            fs::path outP = outDir / (fs::path(p).stem().wstring() + L".json");
            std::ofstream o(outP, std::ios::binary);
            if (!o) { ++failN; if (firstErr.empty()) firstErr = fn + ": 写出失败"; continue; }
            o << j.dump(1, '\t');
            ++ok;
        } catch (const std::exception& e) { ++failN; if (firstErr.empty()) firstErr = fn + ": 解析失败(" + e.what() + ")"; }
    }
    { char b[160]; snprintf(b, sizeof b, T("导出完成:成功 %d,失败 %d"), ok, failN);
      a.datStatus = std::string(b) + (firstErr.empty() ? "" : ("\n(" + firstErr + ")")); }
}

static void doConvertJsonToDat(App& a) {
    if (a.datFiles.empty()) { a.datStatus = T("请先选择要转换的 json。"); return; }
    if (a.datOutDir[0] == 0) { a.datStatus = T("请先选择导出目录。"); return; }
    fs::path outDir = utf82ws(a.datOutDir);
    std::error_code ec; fs::create_directories(outDir, ec);
    int ok = 0, failN = 0; std::string firstErr;
    for (const auto& p : a.datFiles) {
        std::string fn = ws2utf8(fs::path(p).filename().wstring());
        std::ifstream f(p, std::ios::binary);
        if (!f) { ++failN; if (firstErr.empty()) firstErr = fn + ": 打不开"; continue; }
        ojson j;
        try { j = ojson::parse(f); }
        catch (const std::exception& e) { ++failN; if (firstErr.empty()) firstErr = fn + ": JSON 解析失败(" + e.what() + ")"; continue; }
        if (!j.is_object() || !j.contains("funcs")) { ++failN; if (firstErr.empty()) firstErr = fn + ": 非 dat JSON(缺 funcs;需 DAT→JSON 导出的格式)"; continue; }
        try {
            ed9::Script s = datjson::fromJson(j);
            std::vector<uint8_t> bytes = ed9::assemble(s);
            if (bytes.empty()) { ++failN; if (firstErr.empty()) firstErr = fn + ": 组装结果为空"; continue; }
            fs::path outP = outDir / (fs::path(p).stem().wstring() + L".dat");
            std::ofstream o(outP, std::ios::binary);
            if (!o) { ++failN; if (firstErr.empty()) firstErr = fn + ": 写出失败"; continue; }
            o.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            ++ok;
        } catch (const std::exception& e) { ++failN; if (firstErr.empty()) firstErr = fn + ": 组装失败(" + e.what() + ")"; }
    }
    { char b[160]; snprintf(b, sizeof b, T("导出完成:成功 %d,失败 %d"), ok, failN);
      a.datStatus = std::string(b) + (firstErr.empty() ? "" : ("\n(" + firstErr + ")")); }
}

static bool drawSegToggle(const char* id, const char* const* labels, int count,
                          int& state, float& anim, bool& dragging, float minWidth, float scale = 1.0f);

static bool labeledSeg(const char* lead, const char* id, const char* const* labels, int count,
                       int& state, float& anim, bool& dragging, float minWidth, float scale = 1.0f) {
    float H = ImGui::GetFrameHeight() * 1.35f * scale;
    float baseY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(baseY + (H - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextUnformatted(lead);
    ImGui::SameLine();
    ImGui::SetCursorPosY(baseY);
    return drawSegToggle(id, labels, count, state, anim, dragging, minWidth, scale);
}
static bool flatButton(App& a, const char* label, const char* id, bool compact = false) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImVec4 base = st.Colors[ImGuiCol_Button];
    std::string btnId = std::string(label) + "###" + id;
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, base);
    bool clicked = compact ? ImGui::SmallButton(btnId.c_str()) : ImGui::Button(btnId.c_str());
    bool hov = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    float& anim = a.btnAnim[id];
    float sp = ImGui::GetIO().DeltaTime * 14.0f; if (sp > 1.0f) sp = 1.0f;
    anim += ((hov ? 1.0f : 0.0f) - anim) * sp;
    if (anim > 0.01f)
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            IM_COL32(255, 255, 255, (int)(230 * anim)), st.FrameRounding, 0, 2.0f);
    return clicked;
}

static void drawSelectedFiles(App& a, std::vector<std::wstring>& files, const char* idPrefix) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float rightX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float delW   = ImGui::CalcTextSize(T("删")).x + st.FramePadding.x * 2.0f;
    const float gap    = st.ItemSpacing.x * 2.0f;
    int rm = -1;
    std::vector<std::string> names(files.size());
    for (size_t i = 0; i < files.size(); ++i) names[i] = ws2utf8(fs::path(files[i]).filename().wstring());
    for (int i = 0; i < (int)files.size(); ++i) {
        char id[64]; snprintf(id, sizeof id, "%s%d", idPrefix, i);
        if (flatButton(a, T("删"), id)) rm = i;
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(names[i].c_str());
        if (i + 1 < (int)files.size()) {
            float nextW = delW + st.ItemInnerSpacing.x + ImGui::CalcTextSize(names[i + 1].c_str()).x;
            if (ImGui::GetItemRectMax().x + gap + nextW < rightX) ImGui::SameLine(0, gap);
        }
    }
    if (rm >= 0) files.erase(files.begin() + rm);
}

static void drawConvertTab(App& a) {
    int prevMode = a.convMode;
    const char* cmLabels[2] = { T("TBL → JSON(解码)"), T("JSON → TBL(编码)") };
    labeledSeg(T("方位"), "##convmode", cmLabels, 2, a.convMode, a.convSegAnim, a.convSegDrag, 340.0f, 0.85f);
    if (a.convMode != prevMode) { a.convTbls.clear(); a.convStatus.clear(); }

    const bool toJson = (a.convMode == 0);
    ImGui::Spacing();

    const wchar_t* filter = toJson ? L"tbl 文件 (*.tbl)\0*.tbl\0所有文件 (*.*)\0*.*\0"
                                   : L"json 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    const wchar_t* pickTitle = toJson ? L"选择要转换的 tbl(可多选)" : L"选择要转换的 json(可多选)";

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(toJson ? T("1) 要转换的 tbl") : T("1) 要转换的 json"));
    ImGui::SameLine();
    if (flatButton(a, T("选择…(可多选)"), "conv_sel")) { std::vector<std::wstring> sel; if (browseFilesMulti(sel, filter, pickTitle)) a.convTbls = sel; }
    ImGui::SameLine();
    if (flatButton(a, T("追加…"), "conv_add"))         { std::vector<std::wstring> sel; if (browseFilesMulti(sel, filter, pickTitle)) for (auto& s : sel) a.convTbls.push_back(s); }
    ImGui::SameLine();
    if (flatButton(a, T("清空"), "conv_clr")) a.convTbls.clear();
    ImGui::SameLine();
    ImGui::TextDisabled(T("已选 %d 个"), (int)a.convTbls.size());

    ImGui::BeginChild("convlist", ImVec2(0, 240), true);
    if (a.convTbls.empty()) ImGui::TextDisabled("%s", toJson ? T("(未选择;可从 table_sc 等文件夹一次多选 .tbl)")
                                                             : T("(未选择;选之前 TBL→JSON 导出并编辑过的 .json)"));
    else drawSelectedFiles(a, a.convTbls, "convdel");
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("2) 导出目录"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-200.0f * g_dpiScale);
    ImGui::InputText("##convout", a.convOutDir, sizeof(a.convOutDir));
    ImGui::SameLine();
    if (flatButton(a, T("选择目录…"), "conv_out")) { std::wstring sel; if (browseFolder(sel, toJson ? L"选择 JSON 导出目录" : L"选择 TBL 导出目录")) strncpy_s(a.convOutDir, ws2utf8(sel).c_str(), _TRUNCATE); }

    ImGui::Spacing();
    if (flatButton(a, toJson ? T("  解码并导出 JSON  ") : T("  编码并导出 TBL  "), "conv_run")) { if (toJson) doConvert(a); else doConvertJsonToTbl(a); }
    ImGui::SameLine();
    const char* cs = a.convStatus.empty()
        ? (toJson ? T("选择 tbl 与导出目录,然后点「解码并导出 JSON」。") : T("选择 json 与导出目录,然后点「编码并导出 TBL」。"))
        : a.convStatus.c_str();
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", cs);
}

static void drawDatConvertTab(App& a) {
    int prevMode = a.datConvMode;
    const char* dmLabels[2] = { T("DAT → JSON(反编)"), T("JSON → DAT(回编)") };
    labeledSeg(T("方位"), "##datmode", dmLabels, 2, a.datConvMode, a.datSegAnim, a.datSegDrag, 340.0f, 0.85f);
    if (a.datConvMode != prevMode) { a.datFiles.clear(); a.datStatus.clear(); }

    const bool toJson = (a.datConvMode == 0);
    ImGui::Spacing();

    const wchar_t* filter = toJson ? L"dat 脚本 (*.dat)\0*.dat\0所有文件 (*.*)\0*.*\0"
                                   : L"json 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    const wchar_t* pickTitle = toJson ? L"选择要转换的 dat(可多选)" : L"选择要转换的 json(可多选)";

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(toJson ? T("1) 要转换的 dat") : T("1) 要转换的 json"));
    ImGui::SameLine();
    if (flatButton(a, T("选择…(可多选)"), "dat_sel")) { std::vector<std::wstring> sel; if (browseFilesMulti(sel, filter, pickTitle)) a.datFiles = sel; }
    ImGui::SameLine();
    if (flatButton(a, T("追加…"), "dat_add"))         { std::vector<std::wstring> sel; if (browseFilesMulti(sel, filter, pickTitle)) for (auto& s : sel) a.datFiles.push_back(s); }
    ImGui::SameLine();
    if (flatButton(a, T("清空"), "dat_clr")) a.datFiles.clear();
    ImGui::SameLine();
    ImGui::TextDisabled(T("已选 %d 个"), (int)a.datFiles.size());

    ImGui::BeginChild("datconvlist", ImVec2(0, 240), true);
    if (a.datFiles.empty()) ImGui::TextDisabled("%s", toJson ? T("(未选择;选 script\\scena 等文件夹里的 .dat)")
                                                             : T("(未选择;选之前 DAT→JSON 导出并编辑过的 .json)"));
    else drawSelectedFiles(a, a.datFiles, "datdel");
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("2) 导出目录"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-200.0f * g_dpiScale);
    ImGui::InputText("##datconvout", a.datOutDir, sizeof(a.datOutDir));
    ImGui::SameLine();
    if (flatButton(a, T("选择目录…"), "dat_out")) { std::wstring sel; if (browseFolder(sel, toJson ? L"选择 JSON 导出目录" : L"选择 DAT 导出目录")) strncpy_s(a.datOutDir, ws2utf8(sel).c_str(), _TRUNCATE); }

    ImGui::Spacing();
    if (flatButton(a, toJson ? T("  反编并导出 JSON  ") : T("  回编并导出 DAT  "), "dat_run")) { if (toJson) doConvertDatToJson(a); else doConvertJsonToDat(a); }
    ImGui::SameLine();
    const char* cs = a.datStatus.empty()
        ? (toJson ? T("选择 dat 与导出目录,然后点「反编并导出 JSON」。") : T("选择 json 与导出目录,然后点「回编并导出 DAT」。"))
        : a.datStatus.c_str();
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", cs);
}

static void drawMergeLog(App& a) {
    const json errs = a.hasReport ? a.report.value("errors", json::array()) : json::array();
    if (errs.empty() && a.mergeLog.empty()) {
        ImGui::TextDisabled("%s", T("暂无日志/错误。点上方「保存」后,这里显示日志与错误详情。"));
        return;
    }
    ImGui::BeginChild("logscroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    if (!errs.empty()) {
        const ImVec4 red(1.00f, 0.42f, 0.40f, 1.0f);
        ImGui::TextColored(red, T("错误 %d 条:"), (int)errs.size());
        for (const auto& e : errs) {
            std::string where = trErr(e.value("where", std::string("?")));
            std::string detail = trErr(e.value("detail", std::string()));
            std::string mods;
            for (const auto& mn : e.value("mods", json::array()))
                if (mn.is_string()) { if (!mods.empty()) mods += ", "; mods += mn.get<std::string>(); }
            ImGui::TextColored(red, "  %s", where.c_str());
            if (!mods.empty()) { ImGui::SameLine(); ImGui::TextDisabled("[%s]", mods.c_str()); }
            if (!detail.empty()) ImGui::TextWrapped("      %s", detail.c_str());
        }
        ImGui::Separator();
    }

    if (!a.mergeLog.empty()) {
        ImGui::TextDisabled("%s", T("日志(最近一次「保存」):"));
        ImGui::TextUnformatted(a.mergeLog.c_str());
    }
    ImGui::EndChild();
}

static ImVec4 compKindColor(const std::string& k) {
    if (k == "改")   return ImVec4(1.00f, 0.85f, 0.45f, 1.0f);
    if (k == "加")   return ImVec4(0.50f, 0.92f, 0.50f, 1.0f);
    if (k == "克隆") return ImVec4(0.45f, 0.85f, 0.90f, 1.0f);
    if (k == "NPC")  return ImVec4(0.80f, 0.65f, 1.00f, 1.0f);
    return ImVec4(1.0f, 0.72f, 0.40f, 1.0f);
}

static void drawModAssets(App& a, const orch::ModInfo& m) {
    if (a.selAssets.empty()) return;
    int nconf = 0; for (auto& ar : a.selAssets) if (ar.conflict) ++nconf;
    ImGui::Spacing();
    char hdr[260];
    if (nconf) snprintf(hdr, sizeof hdr, T("资源覆盖 asset (%d 个, 冲突 %d)"), (int)a.selAssets.size(), nconf);
    else       snprintf(hdr, sizeof hdr, T("资源覆盖 asset (%d 个)"), (int)a.selAssets.size());
    std::string hdrId = std::string(hdr) + "###assetgrp";
    if (!ImGui::CollapsingHeader(hdrId.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    const ImVec4 red(1.0f, 0.45f, 0.40f, 1.0f);
    const ImVec4 blue(0.55f, 0.78f, 1.00f, 1.0f);
    for (const auto& ar : a.selAssets) {
        ImGui::Indent(16.0f);
        ImGui::TextColored(ar.conflict ? red : blue, "%s", ar.conflict ? T("[覆盖冲突]") : T("[资源]"));
        ImGui::SameLine();
        ImGui::TextUnformatted(ar.rel.c_str());
        if (ar.conflict) {
            std::string others;
            for (const auto& p : ar.providers) if (p != m.name) others += (others.empty() ? "" : ", ") + p;
            ImGui::Indent(16.0f);
            ImGui::TextColored(red, T("也被 [%s] 覆盖  →  加载顺序靠后的 [%s] 生效"),
                               others.empty() ? T("本 mod 内重复") : others.c_str(), ar.winner.c_str());
            ImGui::Unindent(16.0f);
        }
        ImGui::Unindent(16.0f);
    }
    if (nconf) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("同一资源被多个 MOD 覆盖 → 加载顺序靠后者生效(左栏 ▲▼ 调顺序)。不想冲突就关掉其一或调序。"));
    }
}

static ImFont* g_chipFont = nullptr;

struct ChipQ { ImVec2 c, half; float* anim; float scaleUp; bool hov; std::string lbl; };
static std::vector<ChipQ> g_chipQ;

static bool chipButton(const char* label, const char* idSuffix, float& anim, float scaleUp = 0.35f) {
    const ImVec4 clear0(0, 0, 0, 0);
    std::string btnId = std::string(label) + "###" + idSuffix;
    ImGui::PushStyleColor(ImGuiCol_Button, clear0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, clear0);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, clear0);
    ImGui::PushStyleColor(ImGuiCol_Text, clear0);
    bool clicked = ImGui::Button(btnId.c_str(), ImVec2(0.0f, 0.0f));
    bool hov = ImGui::IsItemHovered();
    ImGui::PopStyleColor(4);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    g_chipQ.push_back(ChipQ{ ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f),
                             ImVec2((mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f),
                             &anim, scaleUp, hov, label });
    return clicked;
}

static void chipFlush() {
    if (g_chipQ.empty()) return;
    bool anyHov = false;
    for (const ChipQ& q : g_chipQ) if (q.hov) { anyHov = true; break; }
    const ImGuiStyle& st = ImGui::GetStyle();
    ImFont* chipFont = g_chipFont ? g_chipFont : ImGui::GetFont();
    const float fsz = ImGui::GetFontSize();
    const ImU32 colBtn  = ImGui::GetColorU32(ImGuiCol_Button);
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    float sp = ImGui::GetIO().DeltaTime * 14.0f; if (sp > 1.0f) sp = 1.0f;
    auto rnd = [](float v) { return (float)(long)(v + 0.5f); };
    struct Item { ImVec2 c, half; float s, glow; std::string lbl; };
    std::vector<Item> items; items.reserve(g_chipQ.size());
    for (ChipQ& q : g_chipQ) {
        float target = q.hov ? 1.0f : (anyHov ? -1.0f : 0.0f);
        *q.anim += (target - *q.anim) * sp;
        float av = *q.anim;
        float s = 1.0f + (av >= 0.0f ? av * q.scaleUp : av * 0.15f);
        items.push_back(Item{ q.c, q.half, s, (av > 0.0f ? av : 0.0f), q.lbl });
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.s < y.s; });
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const Item& p : items) {
        ImVec2 mn(rnd(p.c.x - p.half.x * p.s), rnd(p.c.y - p.half.y * p.s));
        ImVec2 mx(rnd(p.c.x + p.half.x * p.s), rnd(p.c.y + p.half.y * p.s));
        float round = st.FrameRounding * p.s;
        dl->AddRectFilled(mn, mx, colBtn, round);
        ImVec2 ts = ImGui::CalcTextSize(p.lbl.c_str());
        ImVec2 tp(rnd((mn.x + mx.x - ts.x * p.s) * 0.5f), rnd((mn.y + mx.y - ts.y * p.s) * 0.5f));
        dl->AddText(chipFont, fsz * p.s, tp, colText, p.lbl.c_str());
        if (p.glow > 0.01f)
            dl->AddRect(mn, mx, IM_COL32(255, 255, 255, (int)(230 * p.glow)), round, 0, 2.0f);
    }
    g_chipQ.clear();
}

static void drawModConfig(App& a) {
    if (a.selMod < 0 || a.selMod >= (int)a.mods.size()) {
        ImGui::TextDisabled("%s", T("← 点左侧某个 MOD,在此查看并逐条开关它的 tbl 修改(tbl\\=共享 / tbl\\sc\\ tbl\\tc\\ tbl\\kr\\=分语言)。"));
        return;
    }
    orch::ModInfo& m = a.mods[a.selMod];
    if (a.compsFor != m.name) {
        a.comps = scanModComponents(a, m.name);
        computeSelAssets(a, m.name);
        a.compsFor = m.name;
        a.cfgFile.clear();
        a.cfgHoverIdx = -1; a.cfgAnim.clear();
        a.cfgBackAnim = a.cfgAllOnAnim = a.cfgAllOffAnim = a.modAllOnAnim = a.modAllOffAnim = 0.0f;
    }
    {
        int fc = ImGui::GetFrameCount();
        if (a.cfgLastFrame != fc - 1) {
            std::fill(a.cfgAnim.begin(), a.cfgAnim.end(), 0.0f);
            a.cfgBackAnim = a.cfgAllOnAnim = a.cfgAllOffAnim = a.modAllOnAnim = a.modAllOffAnim = 0.0f;
        }
        a.cfgLastFrame = fc;
        if (a.cfgFile.empty())
            a.cfgBackAnim = a.cfgAllOnAnim = a.cfgAllOffAnim = 0.0f;
        else {
            std::fill(a.cfgAnim.begin(), a.cfgAnim.end(), 0.0f);
            a.modAllOnAnim = a.modAllOffAnim = 0.0f;
        }
    }
    auto modDisabledWarn = [&]() {
        if (!m.enabled)
            ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.45f, 1.0f), "%s", T("（该 MOD 已整体关闭;下面的开关在 MOD 启用后才会生效）"));
    };

    if (a.comps.empty()) {
        ImGui::Text("MOD: %s", m.name.c_str());
        modDisabledWarn();
        ImGui::TextDisabled("%s", T("(此 MOD 无 tbl 配置:Mod\\<mod>\\tbl\\*.json,语言专属放 tbl\\sc\\ 等)"));
        drawModAssets(a, m);
        return;
    }

    if (!a.cfgFile.empty()) {
        size_t gi = 0, gj = 0; bool found = false;
        for (size_t s = 0; s < a.comps.size(); ) {
            size_t e = s; while (e < a.comps.size() && a.comps[e].file == a.comps[s].file) ++e;
            if (a.comps[s].file == a.cfgFile) { gi = s; gj = e; found = true; break; }
            s = e;
        }
        if (found) {
            ImGui::Text("MOD: %s  >  %s", m.name.c_str(), a.comps[gi].disp.c_str());
            modDisabledWarn();
            ImGui::TextDisabled("%s", T("勾选 = 该条修改生效;取消 = 保存时跳过此条(不影响其它条)。改完点上方「保存」套用。"));
            bool back = chipButton(T("← 后退"), "cfgback", a.cfgBackAnim);
            chipFlush();
            if (!a.showSettings) {
                if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace)) back = true;
                if (ImGui::IsMouseClicked(ImGuiMouseButton(3))) back = true;
            }
            {
                const ImGuiStyle& st = ImGui::GetStyle();
                const char* bOn = T("全部生效"); const char* bOff = T("全部关闭");
                float w = ImGui::CalcTextSize(bOn).x + ImGui::CalcTextSize(bOff).x
                        + st.FramePadding.x * 4 + st.ItemSpacing.x + 18.0f;
                ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
                if (chipButton(bOn, "cfgallon", a.cfgAllOnAnim))
                    for (size_t k = gi; k < gj; ++k) setCompEnabled(m, a.comps[k].rel, true);
                ImGui::SameLine();
                if (chipButton(bOff, "cfgalloff", a.cfgAllOffAnim))
                    for (size_t k = gi; k < gj; ++k) setCompEnabled(m, a.comps[k].rel, false);
                chipFlush();
            }
            ImGui::Separator();
            int grpOff = 0;
            for (size_t k = gi; k < gj; ++k) {
                ModComp& c = a.comps[k];
                if (compDisabled(m, c.rel)) ++grpOff;
                ImGui::PushID(c.rel.c_str());
                ImGui::Indent(16.0f);
                bool on = !compDisabled(m, c.rel);
                if (ImGui::Checkbox("##on", &on)) setCompEnabled(m, c.rel, on);
                ImGui::SameLine();
                ImGui::TextColored(compKindColor(c.kind), "[%s]", T(c.kind.c_str()));
                ImGui::SameLine();
                if (on) ImGui::TextUnformatted(c.label.c_str());
                else    ImGui::TextDisabled("%s", c.label.c_str());
                ImGui::Unindent(16.0f);
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::TextDisabled(T("本表 %d 条,已关闭 %d 条。"), (int)(gj - gi), grpOff);
            if (back) a.cfgFile.clear();
            return;
        }
        a.cfgFile.clear();
    }

    ImGui::Text("MOD: %s", m.name.c_str());
    {
        const ImGuiStyle& gst = ImGui::GetStyle();
        const char* gOn = T("全部生效"); const char* gOff = T("全部关闭");
        float gw = ImGui::CalcTextSize(gOn).x + ImGui::CalcTextSize(gOff).x
                 + gst.FramePadding.x * 4 + gst.ItemSpacing.x + 18.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - gw);
        if (chipButton(gOn, "modallon", a.modAllOnAnim))
            for (auto& c : a.comps) setCompEnabled(m, c.rel, true);
        ImGui::SameLine();
        if (chipButton(gOff, "modalloff", a.modAllOffAnim))
            for (auto& c : a.comps) setCompEnabled(m, c.rel, false);
        chipFlush();
    }
    modDisabledWarn();
    ImGui::Separator();
    struct Grp { std::string file, lbl, btnId; size_t i, j; };
    std::vector<Grp> grps;
    for (size_t i = 0; i < a.comps.size(); ) {
        const std::string file = a.comps[i].file;
        size_t j = i; while (j < a.comps.size() && a.comps[j].file == file) ++j;
        int grpOff = 0; for (size_t k = i; k < j; ++k) if (compDisabled(m, a.comps[k].rel)) ++grpOff;
        const char* disp = a.comps[i].disp.c_str();
        char lbl[420];
        if (grpOff > 0) snprintf(lbl, sizeof lbl, T("%s (关 %d)"), disp, grpOff);
        else            snprintf(lbl, sizeof lbl, "%s", disp);
        Grp G; G.file = file; G.lbl = lbl; G.btnId = std::string(lbl) + "###cfg_" + file; G.i = i; G.j = j;
        grps.push_back(std::move(G));
        i = j;
    }

    if (a.cfgAnim.size() < grps.size()) a.cfgAnim.resize(grps.size(), 0.0f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float rightX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (size_t g = 0; g < grps.size(); ++g) {
        Grp& G = grps[g];
        if (chipButton(G.lbl.c_str(), ("cfg_" + G.file).c_str(), a.cfgAnim[g])) a.cfgFile = G.file;
        if (g + 1 < grps.size()) {
            float nextW  = ImGui::CalcTextSize(grps[g + 1].lbl.c_str()).x + st.FramePadding.x * 2.0f;
            float nextX2 = ImGui::GetItemRectMax().x + st.ItemSpacing.x + nextW;
            if (nextX2 < rightX) ImGui::SameLine();
        }
    }
    chipFlush();
    ImGui::Separator();
    drawModAssets(a, m);
}

static void drawSettings(App& a) {
    if (!a.showSettings) return;
    if (!ImGui::IsPopupOpen("###settings")) ImGui::OpenPopup("###settings");
    ImGui::SetNextWindowSize(ImVec2(600, 380), ImGuiCond_Appearing);
    std::string title = std::string(T("设置")) + "###settings";
    if (ImGui::BeginPopupModal(title.c_str(), &a.showSettings)) {
        const float bottom = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("set_cats", ImVec2(150, -bottom), true);
        if (ImGui::Selectable(T("语言"), a.settingsCat == 0)) a.settingsCat = 0;
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("set_body", ImVec2(0, -bottom), true);
        if (a.settingsCat == 0) {
            ImGui::TextUnformatted(T("界面语言"));
            ImGui::Separator();
            auto langs = scanLanguages();
            if (langs.empty()) {
                ImGui::TextDisabled("%s", T("未找到语言文件(ED9Loader\\language\\*.json)"));
            } else {
                for (const auto& lc : langs) {
                    ImGui::PushID(lc.first.c_str());
                    bool sel = (lc.first == g_langCode);
                    if (ImGui::RadioButton(lc.second.c_str(), sel) && !sel) {
                        loadLanguage(lc.first);
                        saveIniGameDir(a);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled("%s", T("语言文件放在 ED9Loader\\language\\,可自行增删。"));
        }
        ImGui::EndChild();
        if (ImGui::Button(T("关闭"))) { a.showSettings = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static void DrawBackground();

static bool drawSegToggle(const char* id, const char* const* labels, int count,
                          int& state, float& anim, bool& dragging, float minWidth, float scale) {
    if (count < 2) count = 2;
    if (state < 0) state = 0;
    if (state >= count) state = count - 1;
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();
    float maxLbl = 0.0f;
    for (int i = 0; i < count; ++i) { float w = ImGui::CalcTextSize(labels[i]).x; if (w > maxLbl) maxLbl = w; }
    float W = (maxLbl + 32.0f * scale * g_dpiScale) * count;
    float floorW = minWidth * scale * g_dpiScale; if (W < floorW) W = floorW;
    const float H = ImGui::GetFrameHeight() * 1.35f * scale;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(W, H));
    bool hovered = ImGui::IsItemHovered();
    ImVec2 p0 = pos, p1(pos.x + W, pos.y + H);
    const float pad = 3.0f;
    const float segW = W / count;
    int oldState = state;
    auto segAt = [&](float x) { int s = (int)((x - p0.x) / segW); if (s < 0) s = 0; if (s >= count) s = count - 1; return s; };
    if (ImGui::IsItemActivated()) dragging = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        dragging = true;
        float f = (io.MousePos.x - p0.x) / segW - 0.5f;
        if (f < 0.0f) f = 0.0f;
        if (f > count - 1) f = (float)(count - 1);
        anim = f;
        state = segAt(io.MousePos.x);
    }
    if (ImGui::IsItemDeactivated() && !dragging)
        state = segAt(io.MousePos.x);
    if (!(ImGui::IsItemActive() && dragging)) {
        float sp = io.DeltaTime * 14.0f; if (sp > 1.0f) sp = 1.0f;
        anim += ((float)state - anim) * sp;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rTrack = H * 0.5f;
    dl->AddRectFilled(p0, p1, IM_COL32(20, 24, 32, 210), rTrack);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, hovered ? 65 : 32), rTrack, 0, 1.5f);
    float knobW = segW - pad * 2.0f;
    float knobCX = p0.x + segW * (anim + 0.5f);
    ImVec2 kmn(knobCX - knobW * 0.5f, p0.y + pad), kmx(knobCX + knobW * 0.5f, p1.y - pad);
    dl->AddRectFilled(kmn, kmx, ImGui::GetColorU32(st.Colors[ImGuiCol_Button]), rTrack);
    dl->AddRect(kmn, kmx, IM_COL32(255, 255, 255, 230), rTrack, 0, 2.0f);
    for (int i = 0; i < count; ++i) {
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        float cx = p0.x + segW * (i + 0.5f);
        dl->AddText(ImVec2(cx - ts.x * 0.5f, pos.y + (H - ts.y) * 0.5f),
                    (i == state) ? IM_COL32(255, 255, 255, 255) : IM_COL32(178, 184, 198, 205), labels[i]);
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return state != oldState;
}

static void drawUI(App& a) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar);

    DrawBackground();

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(T("文件"))) {
            if (ImGui::MenuItem(T("保存"), "Ctrl+S")) doMerge(a);
            if (ImGui::MenuItem(T("设置"))) a.showSettings = true;
            ImGui::Separator();
            if (ImGui::BeginMenu(T("游戏目录"))) {
                ImGui::TextDisabled("%s", T("空之轨迹 the 1st 游戏根目录(含 sora_1st.exe / ED9Loader)"));
                ImGui::SetNextItemWidth(560.0f * g_dpiScale);
                if (ImGui::InputText("##gamedir", a.gameDir, sizeof(a.gameDir), ImGuiInputTextFlags_EnterReturnsTrue)
                    || ImGui::IsItemDeactivatedAfterEdit()) { saveIniGameDir(a); refresh(a); }
                if (ImGui::MenuItem(T("浏览…"))) {
                    std::wstring sel;
                    if (browseFolder(sel, L"选择 空之轨迹 the 1st 游戏根目录(含 sora_1st.exe / ED9Loader 的那层)")) {
                        strncpy_s(a.gameDir, ws2utf8(sel).c_str(), _TRUNCATE); saveIniGameDir(a); refresh(a);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    {
        ImVec2 wp = ImGui::GetWindowPos();
        float  ww = ImGui::GetWindowSize().x;
        float  y  = wp.y + ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ww, y),
                                            IM_COL32(255, 255, 255, 255), 1.0f);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) doMerge(a);

    drawSettings(a);

    const char* kFuncs[] = { T("MOD管理"), T("TBL/Json互转"), T("DAT/Json互转") };
    labeledSeg(T("功能"), "##func", kFuncs, 3, a.funcView, a.funcSegAnim, a.funcSegDrag, 420.0f);
    ImGui::Separator();

    if (a.funcView == 1) { drawConvertTab(a); ImGui::End(); return; }
    if (a.funcView == 2) { drawDatConvertTab(a); ImGui::End(); return; }

    ImGui::TextDisabled("%s", T("靠下的 mod = 优先级更高(冲突时覆盖靠上的)"));
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", a.status.c_str());
    ImGui::Separator();

    float leftW = vp->WorkSize.x * 0.38f;

    updateLeftIndicators(a);
    ImGui::BeginChild("left", ImVec2(leftW, 0), true);
    ImGui::TextUnformatted(T("MOD 加载顺序"));
    ImGui::Separator();
    if (a.mods.empty()) ImGui::TextDisabled("%s", T("(无 mod 或未扫描)"));
    int moveFrom = -1, moveTo = -1;
    for (int i = 0; i < (int)a.mods.size(); ++i) {
        ImGui::PushID(i);
        bool en = a.mods[i].enabled;
        if (ImGui::Checkbox("##en", &en)) { a.mods[i].enabled = en; a.compsFor.clear(); }
        ImGui::SameLine();
        bool hasErr  = a.errorMods.count(a.mods[i].name) > 0;
        bool hasConf = a.conflictMods.count(a.mods[i].name) > 0;
        const char* tag = hasErr ? T("  [错误]") : (hasConf ? T("  [冲突]") : "");
        char nm[340];
        snprintf(nm, sizeof nm, "%2d. %s%s%s", i + 1, a.mods[i].name.c_str(),
                 a.mods[i].disabled.empty() ? "" : "  *", tag);
        float rowW = ImGui::GetContentRegionAvail().x;
        bool colored = true;
        if (hasErr)            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.42f, 0.40f, 1.0f));
        else if (hasConf)      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.70f, 0.30f, 1.0f));
        else if (!a.mods[i].enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        else colored = false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetNextItemAllowOverlap();
        bool clicked = ImGui::Selectable(nm, a.selMod == i, ImGuiSelectableFlags_AllowItemOverlap, ImVec2(rowW, 0));
        bool rowHov  = ImGui::IsItemHovered();
        ImVec2 rmn = ImGui::GetItemRectMin(), rmx = ImGui::GetItemRectMax();
        float rnd   = ImGui::GetStyle().FrameRounding;
        float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.5f);
        dl->ChannelsSetCurrent(0);
        if (a.selMod == i) dl->AddRectFilled(rmn, rmx, ImGui::GetColorU32(ImGuiCol_Button, 0.55f), rnd);
        dl->ChannelsMerge();
        ImGui::PopStyleColor(3);
        if (clicked) { a.selMod = i; a.rightView = 1; }
        if (colored) ImGui::PopStyleColor();
        if ((hasErr || hasConf || !a.mods[i].disabled.empty()) && ImGui::IsItemHovered()) {
            std::string tip;
            if (hasErr)  tip += T("[错误] 保存时出错(详见「日志」)\n");
            if (hasConf) tip += T("[冲突] 有资源被多个 MOD 重复覆盖(选中看「资源覆盖」)\n");
            if (!a.mods[i].disabled.empty()) { char b[96]; snprintf(b, sizeof b, T("有 %d 个修改项被单独关闭"), (int)a.mods[i].disabled.size()); tip += b; }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::SameLine(rowW - 52 * g_dpiScale);
        char upId[16], dnId[16];
        snprintf(upId, sizeof upId, "mvup%d", i);
        snprintf(dnId, sizeof dnId, "mvdn%d", i);
        if (flatButton(a, "^", upId, true) && i > 0) { moveFrom = i; moveTo = i - 1; }
        ImGui::SameLine();
        if (flatButton(a, "v", dnId, true) && i < (int)a.mods.size() - 1) { moveFrom = i; moveTo = i + 1; }
        if (rowHov)        dl->AddRectFilled(rmn, rmx, IM_COL32(255, 255, 255, (int)(16 + 34 * pulse)), rnd);
        if (a.selMod == i) dl->AddRect(rmn, rmx, IM_COL32(255, 255, 255, 220), rnd, 0, 1.5f);
        if (rowHov)        dl->AddRect(rmn, rmx, IM_COL32(255, 255, 255, (int)(80 + 140 * pulse)), rnd, 0, 1.5f);
        ImGui::PopID();
    }
    if (moveFrom >= 0 && moveTo >= 0) {
        std::swap(a.mods[moveFrom], a.mods[moveTo]);
        if (a.selMod == moveFrom) a.selMod = moveTo;
        else if (a.selMod == moveTo) a.selMod = moveFrom;
        a.compsFor.clear();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0, 0), true);
    int nErr = a.hasReport ? (int)a.report.value("errors", json::array()).size() : 0;
    char rlbl[64];
    if (nErr > 0) snprintf(rlbl, sizeof rlbl, T("日志 / 错误(%d)"), nErr);
    else          snprintf(rlbl, sizeof rlbl, "%s", T("日志 / 错误"));
    const char* vlabels[2] = { T("MOD 配置"), rlbl };
    int vi = (a.rightView == 1) ? 0 : 1;
    drawSegToggle("##viewtoggle", vlabels, 2, vi, a.viewToggleAnim, a.viewToggleDragging, 320.0f);
    a.rightView = (vi == 0) ? 1 : 0;
    ImGui::Separator();
    if (a.rightView == 1) drawModConfig(a);
    else                  drawMergeLog(a);
    ImGui::EndChild();

    ImGui::End();
}

static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) { g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView); pBackBuffer->Release(); }
}
static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, lvls, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static ID3D11ShaderResourceView* g_bgSRV = nullptr;
static int g_bgW = 0, g_bgH = 0;

static bool LoadBackgroundTexture(HINSTANCE hInst) {
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_BGIMAGE), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(hInst, hRes);
    DWORD sz = SizeofResource(hInst, hRes);
    const void* p = hData ? LockResource(hData) : nullptr;
    if (!p || !sz) return false;

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return false;

    bool ok = false;
    IWICStream*             stream = nullptr;
    IWICBitmapDecoder*      dec    = nullptr;
    IWICBitmapFrameDecode*  frame  = nullptr;
    IWICFormatConverter*    conv   = nullptr;
    do {
        if (FAILED(wic->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory((BYTE*)const_cast<void*>(p), sz))) break;
        if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        if (FAILED(wic->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                    nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (!w || !h) break;
        std::vector<BYTE> pixels((size_t)w * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data()))) break;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd = {};
        srd.pSysMem = pixels.data(); srd.SysMemPitch = w * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_pd3dDevice->CreateTexture2D(&td, &srd, &tex)) || !tex) break;
        D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
        svd.Format = td.Format; svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        HRESULT hr = g_pd3dDevice->CreateShaderResourceView(tex, &svd, &g_bgSRV);
        tex->Release();
        if (FAILED(hr)) break;
        g_bgW = (int)w; g_bgH = (int)h;
        ok = true;
    } while (false);

    if (conv)   conv->Release();
    if (frame)  frame->Release();
    if (dec)    dec->Release();
    if (stream) stream->Release();
    wic->Release();
    return ok;
}

static void DrawBackground() {
    if (!g_bgSRV || g_bgW <= 0 || g_bgH <= 0) return;
    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 sz = ImGui::GetWindowSize();
    if (sz.x <= 0 || sz.y <= 0) return;
    ImVec2 p1 = ImVec2(p0.x + sz.x, p0.y + sz.y);
    float wr = sz.x / sz.y;
    float ir = (float)g_bgW / (float)g_bgH;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (wr > ir) {
        float v = ir / wr * 0.5f; uv0.y = 0.5f - v; uv1.y = 0.5f + v;
    } else {
        float u = wr / ir * 0.5f; uv0.x = 0.5f - u; uv1.x = 0.5f + u;
    }
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)g_bgSRV, p0, p1, uv0, uv1,
                                         IM_COL32(255, 255, 255, 64));
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lParam)->ptMinTrackSize.x = (LONG)(900 * g_dpiScale);
        ((MINMAXINFO*)lParam)->ptMinTrackSize.y = (LONG)(580 * g_dpiScale);
        return 0;
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static HANDLE g_cliOut = INVALID_HANDLE_VALUE;
static void cliSetupConsole() {
    g_cliOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_cliOut == NULL || g_cliOut == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            g_cliOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
}
static void cliWrite(const std::string& u8) {
    if (g_cliOut == nullptr || g_cliOut == INVALID_HANDLE_VALUE) return;
    DWORD mode, wr;
    if (GetConsoleMode(g_cliOut, &mode)) {
        std::wstring w = utf82ws(u8);
        WriteConsoleW(g_cliOut, w.c_str(), (DWORD)w.size(), &wr, nullptr);
    } else {
        WriteFile(g_cliOut, u8.data(), (DWORD)u8.size(), &wr, nullptr);
    }
}
static void cliPrintHelp() {
    cliWrite(
        "ED9ModManager — 空轨1st ED9Loader 的 Mod 管理器(图形界面 + 命令行)\n"
        "\n"
        "用法:\n"
        "  ED9ModManager.exe                          打开图形界面(无参数时)\n"
        "  ED9ModManager.exe merge <游戏根目录> [--force]\n"
        "                                             无界面合并所有启用 mod 到 ED9Loader\\cache\\merged\n"
        "                                             --force 忽略指纹强制重合并\n"
        "  ED9ModManager.exe tbl2json <tbl文件或目录>... [--out <目录>]\n"
        "                                             tbl 解码成可读 JSON(与图形界面同一引擎)\n"
        "  ED9ModManager.exe json2tbl <json文件或目录>... [--out <目录>]\n"
        "                                             编辑过的 JSON 编回 tbl\n"
        "  ED9ModManager.exe --help | -h | help       显示本帮助\n"
        "  ED9ModManager.exe --version                显示版本\n"
        "\n"
        "tbl2json / json2tbl 说明:\n"
        "  · schema 已内置,无需游戏目录;输入可给多个文件或目录(目录取匹配后缀,非递归)。\n"
        "  · --out 缺省时输出到各输入文件的同目录;含未建模字符串池的表(如 NPCParam)拒绝回编。\n"
        "  · 每行 [成功]/[失败];末行 [result] converted=.. failed=..;退出码 0=全成功 1=有失败。\n"
        "  · 示例:ED9ModManager.exe tbl2json \"...\\table_sc\\t_item.tbl\" --out .\n"
        "\n"
        "merge 说明:\n"
        "  · 执行的合并逻辑与游戏启动时、图形界面点「保存」完全一致(orchestrator::Run)。\n"
        "  · 扫 <游戏根目录>\\Mod\\ 下各启用 mod 的 tbl/scene/script_inject/dat/asset,合并写 cache。\n"
        "  · 退出码:0=无失败,1=有失败。\n"
        "  · 末行机器可读:[result] merged=.. tbls=.. injected=.. assets=.. conflicts=.. failed=.. mods=..\n"
        "  · 示例:ED9ModManager.exe merge \"D:\\SteamLibrary\\steamapps\\common\\Sora No Kiseki the 1st\" --force\n");
}
static int cliRunTblConvert(bool toJson, int argc, LPWSTR* argv) {
    std::vector<std::wstring> inputs;
    std::wstring outDir;
    for (int i = 2; i < argc; ++i) {
        std::wstring s = argv[i];
        if (s == L"--out" || s == L"-o") { if (i + 1 < argc) outDir = argv[++i]; }
        else inputs.push_back(s);
    }
    if (inputs.empty()) {
        cliWrite(toJson ? "用法: ED9ModManager.exe tbl2json <tbl文件或目录>... [--out <目录>]\n"
                        : "用法: ED9ModManager.exe json2tbl <json文件或目录>... [--out <目录>]\n");
        return 2;
    }
    const wchar_t* inExt  = toJson ? L".tbl"  : L".json";
    const wchar_t* outExt = toJson ? L".json" : L".tbl";
    std::vector<std::wstring> files;
    for (const auto& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            for (auto& e : fs::directory_iterator(in, ec))
                if (e.is_regular_file(ec) && _wcsicmp(e.path().extension().wstring().c_str(), inExt) == 0)
                    files.push_back(e.path().wstring());
        } else {
            files.push_back(in);
        }
    }
    if (files.empty()) { cliWrite("没有匹配的输入文件(需 " + ws2utf8(inExt) + ")\n"); return 2; }
    if (!outDir.empty()) { std::error_code ec; fs::create_directories(outDir, ec); }
    const std::wstring schemas;
    int ok = 0, failN = 0;
    for (const auto& p : files) {
        fs::path src = p;
        fs::path dst = (outDir.empty() ? src.parent_path() : fs::path(outDir)) / (src.stem().wstring() + outExt);
        std::string fn = ws2utf8(src.filename().wstring());
        std::string err;
        if (toJson) {
            std::ifstream f(src, std::ios::binary | std::ios::ate);
            if (!f) { cliWrite("[失败] " + fn + ": 打不开\n"); ++failN; continue; }
            auto sz = f.tellg(); f.seekg(0);
            std::vector<uint8_t> bytes((size_t)sz);
            if (sz > 0) f.read(reinterpret_cast<char*>(bytes.data()), sz);
            mk::TblFileG g;
            if (!mk::DecodeTblG(bytes, schemas, "Sora1", g, err)) { cliWrite("[失败] " + fn + ": " + err + "\n"); ++failN; continue; }
            std::ofstream o(dst, std::ios::binary);
            if (!o) { cliWrite("[失败] " + fn + ": 写出失败\n"); ++failN; continue; }
            o << tblFileToJson(g, src.stem().string()).dump(2);
        } else {
            std::ifstream f(src, std::ios::binary);
            if (!f) { cliWrite("[失败] " + fn + ": 打不开\n"); ++failN; continue; }
            ojson j;
            try { j = ojson::parse(f); }
            catch (const std::exception& e) { cliWrite("[失败] " + fn + ": JSON解析失败(" + std::string(e.what()) + ")\n"); ++failN; continue; }
            mk::TblFileG g;
            if (!jsonToTblFileG(j, schemas, g, err)) { cliWrite("[失败] " + fn + ": " + err + "\n"); ++failN; continue; }
            std::vector<uint8_t> bytes = mk::EncodeTblG(g);
            if (bytes.empty()) { cliWrite("[失败] " + fn + ": 编码结果为空\n"); ++failN; continue; }
            std::ofstream o(dst, std::ios::binary);
            if (!o) { cliWrite("[失败] " + fn + ": 写出失败\n"); ++failN; continue; }
            o.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        }
        cliWrite("[成功] " + fn + " -> " + ws2utf8(dst.filename().wstring()) + "\n");
        ++ok;
    }
    char buf[160]; snprintf(buf, sizeof buf, "[result] converted=%d failed=%d\n", ok, failN);
    cliWrite(buf);
    return failN > 0 ? 1 : 0;
}

static int tryRunCli() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;
    if (argc < 2) { LocalFree(argv); return -1; }
    std::wstring cmd = argv[1];
    auto eq = [&](const wchar_t* s) { return cmd == s; };
    int ret = 0;
    cliSetupConsole();
    if (eq(L"--help") || eq(L"-h") || eq(L"help") || eq(L"/?")) {
        cliPrintHelp();
    } else if (eq(L"--version") || eq(L"-v")) {
        cliWrite("ED9ModManager 1.0 (modkit 内置)\n");
    } else if (eq(L"merge")) {
        if (argc < 3) { cliWrite("用法: ED9ModManager.exe merge <游戏根目录> [--force]\n"); ret = 2; }
        else {
            std::wstring game = argv[2];
            bool force = (argc >= 4 && std::wstring(argv[3]) == L"--force");
            orch::Paths paths = orch::FromGameDir(game);
            orch::RunResult r = orch::Run(paths, force);
            cliWrite(r.log + "\n");
            char buf[300];
            snprintf(buf, sizeof buf, "[result] merged=%d tbls=%d injected=%d assets=%d conflicts=%d failed=%d mods=%d\n",
                     r.merged, r.tbls, r.injected, r.assets, r.conflicts, r.failed, r.mods);
            cliWrite(buf);
            ret = r.failed > 0 ? 1 : 0;
        }
    } else if (eq(L"tbl2json")) {
        ret = cliRunTblConvert(true, argc, argv);
    } else if (eq(L"json2tbl")) {
        ret = cliRunTblConvert(false, argc, argv);
    } else {
        cliWrite("未知命令: ");
        cliWrite(ws2utf8(cmd));
        cliWrite("\n\n");
        cliPrintHelp();
        ret = 2;
    }
    LocalFree(argv);
    return ret;
}

static void applyModernStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f; s.ChildRounding = 6.0f; s.PopupRounding = 6.0f;
    s.FrameRounding = 6.0f; s.GrabRounding = 6.0f; s.ScrollbarRounding = 8.0f;
    s.WindowPadding = ImVec2(12, 12); s.FramePadding = ImVec2(11, 7);
    s.ItemSpacing = ImVec2(8, 7); s.ItemInnerSpacing = ImVec2(7, 6);
    s.ScrollbarSize = 13.0f; s.GrabMinSize = 11.0f;
    s.WindowBorderSize = 1.0f; s.FrameBorderSize = 0.0f; s.PopupBorderSize = 1.0f;
    ImVec4* c = s.Colors;
    const ImVec4 accent   = ImVec4(0.26f, 0.55f, 0.95f, 1.00f);
    const ImVec4 accentHi = ImVec4(0.34f, 0.63f, 1.00f, 1.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.46f, 0.49f, 0.55f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.13f, 0.14f, 0.17f, 0.50f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.13f, 0.14f, 0.17f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.24f, 0.26f, 0.31f, 0.55f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.17f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.24f, 0.29f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.26f, 0.29f, 0.35f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.13f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = accent;
    c[ImGuiCol_ButtonActive]         = accentHi;
    c[ImGuiCol_Header]               = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.38f, 0.62f, 1.00f);
    c[ImGuiCol_HeaderActive]         = accent;
    c[ImGuiCol_CheckMark]            = accentHi;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = accentHi;
    c[ImGuiCol_Separator]            = ImVec4(0.24f, 0.26f, 0.31f, 0.60f);
    c[ImGuiCol_SeparatorHovered]     = accent;
    c[ImGuiCol_SeparatorActive]      = accentHi;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.11f, 0.13f, 0.55f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.26f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.35f, 0.42f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.26f, 0.28f, 0.34f, 0.50f);
    c[ImGuiCol_ResizeGripHovered]    = accent;
    c[ImGuiCol_ResizeGripActive]     = accentHi;
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.26f, 0.55f, 0.95f, 0.35f);
}

static void enableDpiAwareness() {
    HMODULE u32 = LoadLibraryW(L"user32.dll");
    if (u32) {
        typedef BOOL(WINAPI * PFN_SetCtx)(HANDLE);
        auto setCtx = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            if (setCtx((HANDLE)-4)) { FreeLibrary(u32); return; }
            if (setCtx((HANDLE)-3)) { FreeLibrary(u32); return; }
        }
        FreeLibrary(u32);
    }
    SetProcessDPIAware();
}
static float queryDpiScale() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef UINT(WINAPI * PFN_GetDpiForSystem)();
        auto getDpi = (PFN_GetDpiForSystem)GetProcAddress(u32, "GetDpiForSystem");
        if (getDpi) { UINT d = getDpi(); if (d) return (float)d / 96.0f; }
    }
    HDC dc = GetDC(nullptr);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return (float)d / 96.0f;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    int cliRet = tryRunCli();
    if (cliRet >= 0) return cliRet;
    enableDpiAwareness();
    g_dpiScale = queryDpiScale();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HICON hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    HICON hIconSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, hIconBig, nullptr, nullptr, nullptr, L"ED9ModManager", hIconSm };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ED9 Mod Manager", WS_OVERLAPPEDWINDOW,
                              100, 100, (int)(1100 * g_dpiScale), (int)(720 * g_dpiScale),
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    applyModernStyle();
    ImGui::GetStyle().ScaleAllSizes(g_dpiScale);

    static const ImWchar kRanges[] = {
        0x0020, 0x00FF,
        0x2000, 0x206F,
        0x2190, 0x21FF,
        0x2460, 0x24FF,
        0x2500, 0x257F,
        0x25A0, 0x25FF,
        0x2600, 0x26FF,
        0x3000, 0x30FF,
        0x31F0, 0x31FF,
        0xFF00, 0xFFEF,
        0x4E00, 0x9FAF,
        0,
    };
    const float mainPx = 18.0f * g_dpiScale;
    const float chipPx = 26.0f * g_dpiScale;
    ImFont* f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", mainPx, nullptr, kRanges);
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", mainPx, nullptr, kRanges);
    if (!f) io.Fonts->AddFontDefault();
    static ImVector<ImWchar> chipRanges;
    if (chipRanges.empty()) {
        static const ImWchar kArrow[] = { 0x2190, 0x21FF, 0 };
        ImFontGlyphRangesBuilder gb;
        gb.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        gb.AddRanges(kArrow);
        gb.BuildRanges(&chipRanges);
    }
    g_chipFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", chipPx, nullptr, chipRanges.Data);
    if (!g_chipFont) g_chipFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", chipPx, nullptr, chipRanges.Data);
    if (!g_chipFont) g_chipFont = f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    LoadBackgroundTexture(hInst);

    App app;
    loadIniGameDir(app);
    if (app.gameDir[0]) refresh(app);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        pollWatch(app);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawUI(app);
        ImGui::Render();

        const float clear[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    closeWatch(app);
    if (g_bgSRV) { g_bgSRV->Release(); g_bgSRV = nullptr; }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
