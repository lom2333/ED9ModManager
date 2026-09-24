#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "modkit/mod_merge_orchestrator.h"
#include "modkit/generic_tbl.h"
#include "ed9_dat.hpp"
#include "json.hpp"
#include "miniz.h"
#include "modkit/mod_archive.h"
#include "modkit/dds_crypt.h"
#include "modkit/fpac_reader.h"
#include "modkit/fpac_writer.h"

#include <d3d11.h>
#include <wincodec.h>
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tchar.h>
#include "resource.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace orch = ed9loader::modkit::orchestrator;
namespace mk = ed9loader::modkit;
namespace ddsc = ed9loader::modkit::ddscrypt;
using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

static float g_dpiScale = 1.0f;

static const char* kVersion = "1.0.9";
static const wchar_t* kUpdateHost = L"api.github.com";
static const wchar_t* kUpdatePath = L"/repos/lom2333/ED9ModManager/releases/latest";

struct UpdateState {
    std::atomic<int>       phase{0};
    std::atomic<long long> dlDone{0}, dlTotal{0};
    std::mutex             mtx;
    std::string            latest;
    std::string            url;
    std::string            assetName;
    std::string            err;
    std::wstring           dlPath;
    bool                   popupOpened   = false;
    bool                   dlPopupOpened = false;
    bool                   dismissed     = false;
    std::atomic<bool>      manual{false};
    bool                   manualPopupOpened = false;
    bool                   restartLaunched = false;
};
static UpdateState g_upd;

static int cmpVersion(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s) {
        std::vector<int> v; int cur = 0; bool inNum = false;
        size_t i = 0; while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;
        for (; i < s.size(); ++i) {
            char c = s[i];
            if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); inNum = true; }
            else if (c == '.') { v.push_back(cur); cur = 0; inNum = false; }
            else break;
        }
        v.push_back(cur);
        return v;
    };
    std::vector<int> va = parse(a), vb = parse(b);
    size_t n = va.size() > vb.size() ? va.size() : vb.size();
    for (size_t i = 0; i < n; ++i) {
        int x = i < va.size() ? va[i] : 0, y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static bool httpsGet(const wchar_t* host, const wchar_t* path, std::string& body, std::string& err) {
    HINTERNET hs = WinHttpOpen(L"ED9ModManager", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { err = "WinHttpOpen 失败"; return false; }
    WinHttpSetTimeouts(hs, 8000, 8000, 8000, 8000);
    HINTERNET hc = WinHttpConnect(hs, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hr = nullptr;
    bool ok = false;
    do {
        if (!hc) { err = "连接失败"; break; }
        hr = WinHttpOpenRequest(hc, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hr) { err = "创建请求失败"; break; }
        const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
        if (!WinHttpSendRequest(hr, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { err = "发送失败"; break; }
        if (!WinHttpReceiveResponse(hr, nullptr)) { err = "无响应"; break; }
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (code == 404) { err = "no-release"; break; }
        if (code != 200) { char b[48]; snprintf(b, sizeof b, "HTTP %lu", code); err = b; break; }
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(hr, &avail) || avail == 0) break;
            std::vector<char> buf(avail);
            DWORD rd = 0;
            if (!WinHttpReadData(hr, buf.data(), avail, &rd) || rd == 0) break;
            body.append(buf.data(), rd);
        } while (avail > 0);
        ok = true;
    } while (false);
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return ok;
}

static void doUpdateCheck() {
    g_upd.phase = 1;
    std::string body, err;
    if (!httpsGet(kUpdateHost, kUpdatePath, body, err)) {
        std::lock_guard<std::mutex> lk(g_upd.mtx);
        if (err == "no-release") { g_upd.phase = 2; return; }
        g_upd.err = err; g_upd.phase = 4; return;
    }
    try {
        auto j = nlohmann::json::parse(body);
        std::string tag = j.value("tag_name", std::string());
        std::string url, name;
        if (j.contains("assets") && j["assets"].is_array()) {
            for (auto& a : j["assets"]) {
                std::string n = a.value("name", std::string());
                if (n.size() >= 4 && n.compare(n.size() - 4, 4, ".zip") == 0) {
                    url = a.value("browser_download_url", std::string()); name = n; break;
                }
            }
        }
        std::lock_guard<std::mutex> lk(g_upd.mtx);
        g_upd.latest = tag; g_upd.url = url; g_upd.assetName = name;
        g_upd.phase = (!tag.empty() && cmpVersion(kVersion, tag) < 0) ? 3 : 2;
    } catch (...) {
        std::lock_guard<std::mutex> lk(g_upd.mtx);
        g_upd.err = "解析响应失败"; g_upd.phase = 4;
    }
}
static void startUpdateCheck() { std::thread(doUpdateCheck).detach(); }

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

struct AudioGroup {
    std::string ns;
    std::string name;
    std::string rel;
    int files = 0;
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
    std::string gameId;
    std::map<std::string, std::string> modGame;
    bool showBadGamePopup = false;
    std::vector<std::string> badGameMods;
    std::string enableWarnMod;
    bool showEnableWarn = false;
    json report;
    bool hasReport = false;
    std::string status = "就绪。设置游戏目录后会自动扫描,并持续监听 Mod 目录变动。";
    std::string mergeLog;
    bool busy = false;
    int funcView = 0;
    bool showSettings = false;
    bool showChangelog = false;
    int settingsCat = 0;
    int selMod = -1;
    int rightView = 1;
    float viewToggleAnim = 0.0f;
    bool  viewToggleDragging = false;
    float funcSegAnim = 0.0f;  bool funcSegDrag = false;
    float convSegAnim = 0.0f;  bool convSegDrag = false;
    float datSegAnim  = 0.0f;  bool datSegDrag  = false;
    float ddsSegAnim  = 0.0f;  bool ddsSegDrag  = false;
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
    std::vector<AssetRow> selAi;
    std::vector<AssetRow> selAni;
    std::vector<AudioGroup> audioGroups;
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
    int ddsMode = 0;
    std::vector<std::wstring> ddsFiles;
    char ddsOutDir[1024] = {};
    std::string ddsStatus;
    std::vector<std::string> ddsLog;
    bool ddsOverwrite = false;
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
static fs::path modRootOf(const App& a, const std::string& modName) {
    return fs::path(orch::ModRoot(modsDirOf(a).wstring(), modName));
}
static fs::path reportPathOf(const App& a) { return fs::path(utf82ws(a.gameDir)) / L"ED9Loader" / L"cache" / L"merge_report.json"; }

struct GameDef { const char* id; const wchar_t* exe; const char* disp; };
static const GameDef kGames[] = {
    { "sora_1st", L"sora_1st.exe", "空之轨迹 the 1st" },
    { "sora_2nd", L"sora_2nd.exe", "空之轨迹 the 2nd" },
};

static std::string detectGameId(const char* gameDirUtf8) {
    if (gameDirUtf8 == nullptr || gameDirUtf8[0] == 0) return {};
    fs::path root = utf82ws(gameDirUtf8);
    std::error_code ec;
    for (const auto& g : kGames)
        if (fs::exists(root / g.exe, ec)) return g.id;
    return {};
}
static const char* gameDisplay(const std::string& id) {
    for (const auto& g : kGames) if (id == g.id) return g.disp;
    return "";
}
static fs::path modMetaPath(const App& a, const std::string& modName) {
    return modRootOf(a, modName) / L"mod.json";
}
static std::string readModGameDeclared(const App& a, const std::string& modName) {
    json j;
    if (!readJson(modMetaPath(a, modName), j) || !j.is_object()) return {};
    std::string g = j.value("game", std::string());
    if (g == "any") return {};
    for (const auto& gd : kGames) if (g == gd.id) return g;
    return {};
}

static bool tblHeadInfo(const fs::path& p, std::string& table, uint32_t& rowLen) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    char hdr[8 + 80] = {};
    f.read(hdr, sizeof hdr);
    if (f.gcount() < (std::streamsize)sizeof hdr) return false;
    if (memcmp(hdr, "#TBL", 4) != 0) return false;
    table.assign(hdr + 8, strnlen(hdr + 8, 64));
    memcpy(&rowLen, hdr + 8 + 64 + 8, 4);
    return !table.empty() && rowLen > 0;
}

struct ModGameGuess { std::string game; int votes1 = 0, votes2 = 0; bool conflict = false; };
static ModGameGuess inferModGame(const App& a, const std::string& modName) {
    ModGameGuess g;
    const fs::path root = modRootOf(a, modName);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return g;

    auto vote = [&](const std::string& who) {
        if (who == "sora_1st") ++g.votes1;
        else if (who == "sora_2nd") ++g.votes2;
    };
    std::vector<fs::path> roots;
    for (const wchar_t* sub : { L"tbl", L"table" }) {
        fs::path d = root / sub;
        if (fs::is_directory(d, ec)) roots.push_back(d);
    }
    if (roots.empty()) return g;
    std::vector<fs::path> files;
    for (const auto& r : roots)
        for (fs::recursive_directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(it->path());

    for (const fs::path& p : files) {
        const std::wstring ext = p.extension().wstring();
        if (ext == L".tbl") {
            std::string t; uint32_t len = 0;
            if (tblHeadInfo(p, t, len)) vote(mk::TblVariantGame(t, len));
        } else if (ext == L".json") {
            json j;
            if (!readJson(p, j) || !j.is_object() || !j.contains("table")) continue;
            if (!j["table"].is_string()) continue;
            const std::string t = j["table"].get<std::string>();
            if (j.contains("row_length") && j["row_length"].is_number_unsigned()) {
                vote(mk::TblVariantGame(t, j["row_length"].get<uint32_t>()));
                continue;
            }
            std::vector<std::string> names;
            auto collect = [&names](const json& o) {
                if (!o.is_object()) return;
                for (auto f = o.begin(); f != o.end(); ++f)
                    if (!f.key().empty() && f.key()[0] != '_') names.push_back(f.key());
            };
            for (const char* sec : { "add_rows", "edit_rows", "clone_rows" }) {
                if (!j.contains(sec) || !j[sec].is_array()) continue;
                for (const auto& r : j[sec]) {
                    if (!r.is_object()) continue;
                    if (r.contains("set") || r.contains("match")) {
                        collect(r.value("match", json::object()));
                        collect(r.value("set", json::object()));
                    } else {
                        collect(r);
                    }
                }
            }
            vote(mk::TblGameByFieldNames(t, names));
        }
    }
    if (g.votes1 > 0 && g.votes2 > 0) { g.conflict = true; g.game = (g.votes1 >= g.votes2) ? "sora_1st" : "sora_2nd"; }
    else if (g.votes1 > 0) g.game = "sora_1st";
    else if (g.votes2 > 0) g.game = "sora_2nd";
    return g;
}

static std::string resolveModGame(const App& a, const std::string& modName, ModGameGuess* out = nullptr) {
    const std::string decl = readModGameDeclared(a, modName);
    ModGameGuess g = inferModGame(a, modName);
    if (out) *out = g;
    return decl.empty() ? g.game : decl;
}

static void loadReport(App& a) {
    a.hasReport = readJson(reportPathOf(a), a.report);
}
static void refresh(App& a) {
    if (a.gameDir[0] == 0) { a.status = "请先填游戏目录。"; return; }
    a.gameId = detectGameId(a.gameDir);
    {
      std::string alog;
      ed9loader::modkit::archive::EnsureAllStaged(modsDirOf(a), alog);
    }
    a.mods = orch::ScanMods(modsDirOf(a).wstring());
    a.modGame.clear();
    for (const auto& m : a.mods) a.modGame[m.name] = resolveModGame(a, m.name);
    a.selMod = -1; a.compsFor.clear(); a.comps.clear();
    a.conflictSig.clear();
    loadReport(a);
    int mismatch = 0;
    for (const auto& m : a.mods) {
        const std::string& g = a.modGame[m.name];
        if (!g.empty() && !a.gameId.empty() && g != a.gameId) ++mismatch;
    }
    a.status = "已扫描:" + std::to_string(a.mods.size()) + " 个 mod" +
               (mismatch ? ("(其中 " + std::to_string(mismatch) + " 个不适用于当前游戏)") : "");
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
    a.modGame.clear();
    for (const auto& m : a.mods) a.modGame[m.name] = resolveModGame(a, m.name);
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
        fs::path d = modRootOf(a, modName) / f.sub;
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
    fs::path assetDir = modRootOf(a, modName) / L"asset";
    if (!fs::is_directory(assetDir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(assetDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        out.push_back("asset/" + fs::relative(it->path(), assetDir, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::vector<std::string> scanModScriptRels(const App& a, const std::string& modName, const char* sub) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path top = modRootOf(a, modName) / sub;
    for (const char* lang : { "", "sc", "tc", "kr" }) {
        const fs::path d = lang[0] ? top / lang : top;
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& e : fs::directory_iterator(d, ec))
            if (e.is_regular_file(ec) && e.path().extension() == L".dat")
                out.push_back(std::string(sub) + "/" + (lang[0] ? std::string(lang) + "/" : std::string()) + ws2utf8(e.path().filename().wstring()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

static void computeSelScripts(App& a, const std::string& modName, const char* sub, std::vector<AssetRow>& rows) {
    rows.clear();
    std::map<std::string, std::vector<std::string>> prov;
    for (const auto& mm : a.mods) {
        if (!mm.enabled) continue;
        for (const auto& rel : scanModScriptRels(a, mm.name, sub)) prov[rel].push_back(mm.name);
    }
    for (const auto& rel : scanModScriptRels(a, modName, sub)) {
        AssetRow ar; ar.rel = rel;
        auto it = prov.find(rel);
        if (it != prov.end()) ar.providers = it->second;
        ar.conflict = ar.providers.size() > 1;
        ar.winner = ar.providers.empty() ? std::string() : ar.providers.back();
        rows.push_back(std::move(ar));
    }
}

static void scanAudioGroups(App& a, const std::string& modName) {
    a.audioGroups.clear();
    const fs::path root = modRootOf(a, modName);
    std::error_code ec;
    for (const char* ns : { "voice", "se", "bgm1", "bgm2", "bgm3" }) {
        fs::path nsDir = root / ns;
        if (!fs::is_directory(nsDir, ec)) continue;
        for (fs::directory_iterator de(nsDir, ec), end; !ec && de != end; de.increment(ec)) {
            std::error_code e2;
            if (!de->is_directory(e2)) continue;
            const std::string gname = ws2utf8(de->path().filename().wstring());
            if (gname == "wav") continue;
            int n = 0;
            for (fs::recursive_directory_iterator it(de->path(), ec), rend; !ec && it != rend; it.increment(ec))
                if (it->is_regular_file(e2)) ++n;
            AudioGroup g;
            g.ns = ns; g.name = gname; g.rel = std::string(ns) + "/" + gname; g.files = n;
            a.audioGroups.push_back(std::move(g));
        }
    }
    std::sort(a.audioGroups.begin(), a.audioGroups.end(),
              [](const AudioGroup& x, const AudioGroup& y) {
                  return x.ns != y.ns ? x.ns < y.ns : x.name < y.name;
              });
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
            for (const auto& rel : scanModScriptRels(a, m.name, "ai")) prov[rel].push_back(m.name);
            for (const auto& rel : scanModScriptRels(a, m.name, "ani")) prov[rel].push_back(m.name);
        }
        for (const auto& kv : prov)
            if (kv.second.size() > 1) for (const auto& mn : kv.second) a.conflictMods.insert(mn);
    }
}

static void doMerge(App& a) {
    if (a.gameDir[0] == 0) { a.status = "请先填游戏目录。"; return; }
    {
        std::vector<std::string> bad;
        for (const auto& m : a.mods) {
            if (!m.enabled) continue;
            auto it = a.modGame.find(m.name);
            const std::string g = (it != a.modGame.end()) ? it->second : std::string();
            if (!g.empty() && !a.gameId.empty() && g != a.gameId) bad.push_back(m.name);
        }
        if (!bad.empty()) {
            a.badGameMods = bad;
            a.showBadGamePopup = true;
            a.status = T("有 MOD 的适用作品与当前游戏不符,已中止合并");
            return;
        }
    }
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
        tj["row_length"] = mk::TblSchemaSize(t.schema);
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
        std::string text;
        try { text = tblFileToJson(g, fs::path(p).stem().string()).dump(2); }
        catch (const std::exception& e) {
            ++failN; if (firstErr.empty()) firstErr = ws2utf8(fs::path(p).filename().wstring()) + ": 导出 JSON 失败(" + e.what() + ")";
            continue;
        }
        fs::path outP = outDir / (fs::path(p).stem().wstring() + L".json");
        std::ofstream o(outP, std::ios::binary);
        if (!o) { ++failN; continue; }
        o << text;
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
        uint32_t rowLen = 0;
        if (tj.contains("row_length") && tj["row_length"].is_number_unsigned())
            rowLen = tj["row_length"].get<uint32_t>();
        if (rowLen != 0) {
            if (!mk::ResolveTblSchema(schemasDir, name, rowLen, "Sora1", schema, err)) return false;
        } else {
            std::vector<std::string> fieldNames;
            for (const auto& rj : rows) {
                if (!rj.is_object()) continue;
                for (auto it = rj.begin(); it != rj.end(); ++it) fieldNames.push_back(it.key());
                break;
            }
            if (!mk::ResolveTblSchemaByFieldNames(schemasDir, name, fieldNames, schema, err)) return false;
        }
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

static void doConvertDds(App& a) {
    const bool dec = (a.ddsMode == 0);
    a.ddsLog.clear();
    if (a.ddsFiles.empty()) { a.ddsStatus = T("请先选择要转换的 dds。"); return; }
    if (!a.ddsOverwrite && a.ddsOutDir[0] == 0) { a.ddsStatus = T("请先选择导出目录,或勾选「就地覆盖原文件」。"); return; }

    fs::path outDir;
    if (!a.ddsOverwrite) {
        outDir = utf82ws(a.ddsOutDir);
        std::error_code ec; fs::create_directories(outDir, ec);
    }
    int ok = 0, skip = 0, failN = 0, warnN = 0;
    for (const auto& p : a.ddsFiles) {
        const fs::path src = p;
        const std::string fn = ws2utf8(src.filename().wstring());
        std::ifstream f(src, std::ios::binary | std::ios::ate);
        if (!f) { a.ddsLog.push_back(std::string("[失败] ") + fn + ":打不开"); ++failN; continue; }
        const auto sz = f.tellg(); f.seekg(0);
        std::vector<uint8_t> in((size_t)sz);
        if (sz > 0) f.read(reinterpret_cast<char*>(in.data()), sz);
        f.close();

        const ddsc::Form form = ddsc::Detect(in);
        if ((dec && form == ddsc::Form::Plain) || (!dec && form == ddsc::Form::Game)) {
            a.ddsLog.push_back(std::string("[跳过] ") + fn + (dec ? T(":已经是普通 DDS,无需解密") : T(":已经是游戏格式,无需加密")));
            ++skip; continue;
        }

        std::vector<uint8_t> out; std::string err;
        const bool done = dec ? ddsc::Decrypt(in, out, err) : ddsc::Encrypt(in, out, err);
        if (!done) { a.ddsLog.push_back(std::string("[失败] ") + fn + ":" + err); ++failN; continue; }

        const fs::path dst = a.ddsOverwrite ? src : (outDir / src.filename());
        std::error_code ec2;
        if (!a.ddsOverwrite && fs::exists(dst, ec2) && fs::equivalent(src, dst, ec2)) {
            a.ddsLog.push_back(std::string("[失败] ") + fn + T(":导出目录就是源目录,会盖掉原件。换个目录,或勾「就地覆盖原文件」"));
            ++failN; continue;
        }
        std::ofstream o(dst, std::ios::binary);
        if (!o) { a.ddsLog.push_back(std::string("[失败] ") + fn + T(":写出失败")); ++failN; continue; }
        o.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
        o.close();

        const std::vector<uint8_t>& plain = dec ? out : in;
        const ddsc::DdsInfo info = ddsc::Inspect(plain.data(), plain.size());
        char line[512];
        if (info.ok)
            snprintf(line, sizeof line, "[成功] %s  %ux%u  mip%u  %s  %zu → %zu 字节",
                     fn.c_str(), info.width, info.height, info.mips, info.format.c_str(), in.size(), out.size());
        else
            snprintf(line, sizeof line, "[成功] %s  %zu → %zu 字节", fn.c_str(), in.size(), out.size());
        a.ddsLog.push_back(line);
        for (const auto& w : info.warns) { a.ddsLog.push_back(std::string("  [注意] ") + w); ++warnN; }
        ++ok;
    }
    char b[220];
    snprintf(b, sizeof b, T("完成:成功 %d,跳过 %d,失败 %d%s"), ok, skip, failN,
             warnN > 0 ? T("(有体检警告,见下)") : "");
    a.ddsStatus = b;
}

static void drawDdsTab(App& a) {
    int prevMode = a.ddsMode;
    const char* dmLabels[2] = { T("解密(游戏 → 普通 DDS)"), T("加密(普通 DDS → 游戏)") };
    labeledSeg(T("方位"), "##ddsmode", dmLabels, 2, a.ddsMode, a.ddsSegAnim, a.ddsSegDrag, 420.0f, 0.85f);
    if (a.ddsMode != prevMode) { a.ddsFiles.clear(); a.ddsStatus.clear(); a.ddsLog.clear(); }

    const bool dec = (a.ddsMode == 0);
    ImGui::Spacing();

    const wchar_t* filter = L"dds 贴图 (*.dds)\0*.dds\0所有文件 (*.*)\0*.*\0";
    const wchar_t* pickTitle = L"选择要转换的 dds(可多选)";

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("1) 要转换的 dds"));
    ImGui::SameLine();
    if (flatButton(a, T("选择…(可多选)"), "dds_sel")) { std::vector<std::wstring> sel; if (browseFilesMulti(sel, filter, pickTitle)) a.ddsFiles = sel; }
    ImGui::SameLine();
    if (flatButton(a, T("追加…"), "dds_add"))         { std::vector<std::wstring> sel; if (browseFilesMulti(sel, filter, pickTitle)) for (auto& x : sel) a.ddsFiles.push_back(x); }
    ImGui::SameLine();
    if (flatButton(a, T("整个目录…"), "dds_dir")) {
        std::wstring d;
        if (browseFolder(d, L"选择一个目录,里面的 .dds 会全部加入")) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(d, ec))
                if (e.is_regular_file(ec) && _wcsicmp(e.path().extension().wstring().c_str(), L".dds") == 0)
                    a.ddsFiles.push_back(e.path().wstring());
        }
    }
    ImGui::SameLine();
    if (flatButton(a, T("清空"), "dds_clr")) { a.ddsFiles.clear(); a.ddsLog.clear(); a.ddsStatus.clear(); }
    ImGui::SameLine();
    ImGui::TextDisabled(T("已选 %d 个"), (int)a.ddsFiles.size());

    ImGui::BeginChild("ddslist", ImVec2(0, 150), true);
    if (a.ddsFiles.empty()) ImGui::TextDisabled("%s", T("(未选择)"));
    else drawSelectedFiles(a, a.ddsFiles, "ddsdel");
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("2) 导出目录"));
    ImGui::SameLine();
    ImGui::BeginDisabled(a.ddsOverwrite);
    ImGui::SetNextItemWidth(-340.0f * g_dpiScale);
    ImGui::InputText("##ddsout", a.ddsOutDir, sizeof(a.ddsOutDir));
    ImGui::SameLine();
    if (flatButton(a, T("选择目录…"), "dds_out")) { std::wstring sel; if (browseFolder(sel, L"选择 dds 导出目录")) strncpy_s(a.ddsOutDir, ws2utf8(sel).c_str(), _TRUNCATE); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox(T("就地覆盖原文件"), &a.ddsOverwrite);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("直接改写选中的文件本身,不另存。"));

    ImGui::Spacing();
    if (flatButton(a, dec ? T("  解密  ") : T("  加密  "), "dds_run")) doConvertDds(a);
    ImGui::SameLine();
    const char* cs = a.ddsStatus.empty()
        ? (dec ? T("选好 dds 与导出目录,然后点「解密」。") : T("选好 dds 与导出目录,然后点「加密」。"))
        : a.ddsStatus.c_str();
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", cs);

    ImGui::Spacing();
    ImGui::TextUnformatted(T("结果"));
    ImGui::BeginChild("ddslog", ImVec2(0, 0), true);
    if (a.ddsLog.empty()) ImGui::TextDisabled("%s", T("(没结果)"));
    for (const auto& l : a.ddsLog) {
        ImVec4 col(0.85f, 0.88f, 0.92f, 1.0f);
        if (l.rfind("[失败]", 0) == 0)        col = ImVec4(1.00f, 0.42f, 0.40f, 1.0f);
        else if (l.rfind("[跳过]", 0) == 0)   col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        else if (l.rfind("  [注意]", 0) == 0) col = ImVec4(1.00f, 0.70f, 0.30f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s", l.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
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

static const char* const kAudioCfgKey = "##audio";

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

static void drawModScripts(const orch::ModInfo& m, const std::vector<AssetRow>& rows, const char* hdrConf, const char* hdrPlain,
                           const char* grpId, const char* chip) {
    if (rows.empty()) return;
    int nconf = 0; for (auto& ar : rows) if (ar.conflict) ++nconf;
    ImGui::Spacing();
    char hdr[260];
    if (nconf) snprintf(hdr, sizeof hdr, T(hdrConf), (int)rows.size(), nconf);
    else       snprintf(hdr, sizeof hdr, T(hdrPlain), (int)rows.size());
    std::string hdrId = std::string(hdr) + "###" + grpId;
    if (!ImGui::CollapsingHeader(hdrId.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    const ImVec4 red(1.0f, 0.45f, 0.40f, 1.0f);
    const ImVec4 blue(0.55f, 0.78f, 1.00f, 1.0f);
    for (const auto& ar : rows) {
        ImGui::Indent(16.0f);
        ImGui::TextColored(ar.conflict ? red : blue, "%s", ar.conflict ? T("[覆盖冲突]") : chip);
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
}

static void drawModAi(App& a, const orch::ModInfo& m) {
    drawModScripts(m, a.selAi, "AI文件 (%d 个, 冲突 %d)", "AI文件 (%d 个)", "aigrp", "[AI]");
}

static void drawModAni(App& a, const orch::ModInfo& m) {
    drawModScripts(m, a.selAni, "动作文件 (%d 个, 冲突 %d)", "动作文件 (%d 个)", "anigrp", "[ANI]");
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
        computeSelScripts(a, m.name, "ai", a.selAi);
        computeSelScripts(a, m.name, "ani", a.selAni);
        scanAudioGroups(a, m.name);
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
    auto modGameSetting = [&]() {
        const std::string cur = a.modGame.count(m.name) ? a.modGame[m.name] : std::string();
        const bool bad = !cur.empty() && !a.gameId.empty() && cur != a.gameId;
        if (bad) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.36f, 0.34f, 1.0f));
        ImGui::TextUnformatted(T("适用作品"));
        ImGui::SameLine();
        if (cur.empty())      ImGui::TextDisabled("%s", T("通用 / 无法从内容判定"));
        else if (bad)         ImGui::TextUnformatted(gameDisplay(cur));
        else                  ImGui::TextColored(ImVec4(0.62f, 0.90f, 0.82f, 1.0f), "%s", gameDisplay(cur));
        if (bad) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("由管理器自动识别。"));
    };

    if (a.comps.empty() && a.audioGroups.empty()) {
        ImGui::Text("MOD: %s", m.name.c_str());
        modDisabledWarn();
        modGameSetting();
        ImGui::TextDisabled("%s", T("(此 MOD 无 tbl 配置:Mod\\<mod>\\tbl\\*.json,语言专属放 tbl\\sc\\ 等)"));
        drawModAssets(a, m);
        drawModAi(a, m);
        drawModAni(a, m);
        return;
    }

    if (a.cfgFile == kAudioCfgKey) {
        if (a.audioGroups.empty()) { a.cfgFile.clear(); }
        else {
            ImGui::Text("MOD: %s  >  %s", m.name.c_str(), T("语音配置"));
            modDisabledWarn();
            ImGui::TextDisabled("%s", T("勾选 = 该文件夹内的音频生效;取消 = 保存时整组跳过。改完点上方「保存」套用。"));
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
                    for (const auto& g : a.audioGroups) setCompEnabled(m, g.rel, true);
                ImGui::SameLine();
                if (chipButton(bOff, "cfgalloff", a.cfgAllOffAnim))
                    for (const auto& g : a.audioGroups) setCompEnabled(m, g.rel, false);
                chipFlush();
            }
            ImGui::Separator();
            int offN = 0;
            for (const auto& g : a.audioGroups) {
                bool on = !compDisabled(m, g.rel);
                if (!on) ++offN;
                ImGui::PushID(g.rel.c_str());
                ImGui::Indent(16.0f);
                if (ImGui::Checkbox("##ag", &on)) setCompEnabled(m, g.rel, on);
                ImGui::SameLine();
                if (on) ImGui::TextUnformatted(g.name.c_str());
                else    ImGui::TextDisabled("%s", g.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled(T("[%s]  %d 个音频"), g.ns.c_str(), g.files);
                ImGui::Unindent(16.0f);
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::TextDisabled(T("本页 %d 组,已关闭 %d 组。"), (int)a.audioGroups.size(), offN);
            if (back) a.cfgFile.clear();
            return;
        }
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
    modGameSetting();
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
    if (!a.audioGroups.empty()) {
        int aOff = 0;
        for (const auto& g : a.audioGroups) if (compDisabled(m, g.rel)) ++aOff;
        char lbl[160];
        if (aOff > 0) snprintf(lbl, sizeof lbl, T("%s (关 %d)"), T("语音配置"), aOff);
        else          snprintf(lbl, sizeof lbl, "%s", T("语音配置"));
        Grp G; G.file = kAudioCfgKey; G.lbl = lbl;
        G.btnId = std::string(lbl) + "###cfg_audio"; G.i = 0; G.j = 0;
        grps.push_back(std::move(G));
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
    drawModAi(a, m);
    drawModAni(a, m);
}

static void centerModal(float fracW = 0, float fracH = 0,
                        float minW = 0, float minH = 0, float maxW = 0, float maxH = 0) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (fracW > 0.0f) {
        auto cl = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
        ImVec2 sz(cl(vp->WorkSize.x * fracW, minW * g_dpiScale, maxW * g_dpiScale),
                  cl(vp->WorkSize.y * fracH, minH * g_dpiScale, maxH * g_dpiScale));
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    }
}

static void drawSettings(App& a) {
    if (!a.showSettings) return;
    if (!ImGui::IsPopupOpen("###settings")) ImGui::OpenPopup("###settings");
    centerModal(0.42f, 0.52f, 420, 320, 760, 680);
    std::string title = std::string(T("设置")) + "###settings";
    if (ImGui::BeginPopupModal(title.c_str(), &a.showSettings,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
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

struct ChangeEntry { const char* ver; const char* date; std::vector<const char*> items; };
static const std::vector<ChangeEntry>& changelog() {
    static const std::vector<ChangeEntry> log = {
        { "1.0.9", "2026-09-24", {
            "新增Ani和Ai文件通道",
        } },
        { "1.0.8", "2026-09-17", {
            "已适配正式版本",
        } },
        { "1.0.7", "2026-09-03", {
            "已支持PAC包体编辑",
            "适配了2026/9/3的更新",
        } },
        { "1.0.6", "2026-08-29", {
            "现已支持DDS加解密功能",
        } },
        { "1.0.5", "2026-08-27", {
            "适配了Demo更新过后的兼容",
            "MOD 支持压缩包形式:zip / 7z / rar / tar.gz 等,与放文件夹等价,两种可混用",
        } },
        { "1.0.4", "2026-08-20", {
            "支持《空之轨迹 the 2nd》:按游戏目录自动识别当前作品",
            "MOD 适用作品自动识别(比对它改动的表结构),不符则拦截合并并提示",
            "补齐 2nd 的 tbl 结构定义,两作的表均可解析与回编",
            "插件改为按当前游戏版本自适应,游戏更新后不必重新编译",
        } },
        { "1.0.3", "2026-07-12", {
            "新增自动更新",
        } },
        { "1.0.2", "2026-07-10", {
            "适配了7月10日的更新。",
            "优化了UI表现,新增了语音相关的配置",
        } },
        { "1.0.0", "2026-07-01", {
            "首个发布版本:MOD",
        } },
    };
    return log;
}

static void drawChangelog(App& a) {
    if (!a.showChangelog) return;
    if (!ImGui::IsPopupOpen("###changelog")) ImGui::OpenPopup("###changelog");
    centerModal(0.44f, 0.64f, 460, 380, 820, 860);
    std::string title = std::string(T("更新日志")) + "###changelog";
    if (ImGui::BeginPopupModal(title.c_str(), &a.showChangelog,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        const float bottom = ImGui::GetFrameHeightWithSpacing() + 6 * g_dpiScale;
        ImGui::BeginChild("changelog_body", ImVec2(0, -bottom), true);
        bool first = true;
        for (const auto& e : changelog()) {
            if (!first) ImGui::Spacing();
            first = false;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.86f, 1.0f, 1.0f));
            ImGui::Text("v%s", e.ver);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", e.date);
            for (const char* it : e.items) {
                ImGui::Bullet();
                ImGui::TextWrapped("%s", it);
            }
            ImGui::Spacing();
            ImGui::Separator();
        }
        ImGui::EndChild();
        ImGui::TextDisabled(T("当前版本 v%s"), kVersion);
        const ImGuiStyle& st = ImGui::GetStyle();
        float w1 = ImGui::CalcTextSize(T("检查更新")).x + st.FramePadding.x * 2.0f;
        float w2 = ImGui::CalcTextSize(T("关闭")).x     + st.FramePadding.x * 2.0f;
        float totalW = w1 + w2 + st.ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - totalW);
        if (flatButton(a, T("检查更新"), "cl_check")) {
            g_upd.manual = true;
            g_upd.manualPopupOpened = false;
            startUpdateCheck();
            a.showChangelog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (flatButton(a, T("关闭"), "cl_close")) { a.showChangelog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static void DrawBackground();
static void PacWindowShow();

static bool downloadFile(const std::string& urlUtf8, const std::wstring& dest, std::string& err) {
    std::wstring url = utf82ws(urlUtf8);
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "URL 解析失败"; return false; }
    HINTERNET hs = WinHttpOpen(L"ED9ModManager", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { err = "WinHttpOpen 失败"; return false; }
    WinHttpSetTimeouts(hs, 10000, 10000, 30000, 30000);
    HINTERNET hc = WinHttpConnect(hs, host, uc.nPort, 0);
    HINTERNET hr = nullptr;
    bool ok = false;
    do {
        if (!hc) { err = "连接失败"; break; }
        DWORD reqFlags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        hr = WinHttpOpenRequest(hc, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
        if (!hr) { err = "创建请求失败"; break; }
        if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { err = "发送失败"; break; }
        if (!WinHttpReceiveResponse(hr, nullptr)) { err = "无响应"; break; }
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) { char b[48]; snprintf(b, sizeof b, "HTTP %lu", code); err = b; break; }
        DWORD cl = 0, cll = sizeof(cl);
        if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &cl, &cll, WINHTTP_NO_HEADER_INDEX))
            g_upd.dlTotal = (long long)cl;
        std::ofstream out(dest, std::ios::binary);
        if (!out) { err = "无法写入临时文件"; break; }
        long long got = 0; DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(hr, &avail) || avail == 0) break;
            std::vector<char> buf(avail); DWORD rd = 0;
            if (!WinHttpReadData(hr, buf.data(), avail, &rd) || rd == 0) break;
            out.write(buf.data(), rd);
            got += rd; g_upd.dlDone = got;
        } while (avail > 0);
        out.close();
        ok = (got > 0);
        if (!ok) err = "下载内容为空";
    } while (false);
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return ok;
}

static void startDownload(const std::wstring& destDir) {
    { std::lock_guard<std::mutex> lk(g_upd.mtx); g_upd.err.clear(); }
    g_upd.dlDone = 0; g_upd.dlTotal = 0; g_upd.phase = 5;
    std::string url; { std::lock_guard<std::mutex> lk(g_upd.mtx); url = g_upd.url; }
    std::wstring dir = destDir;
    std::thread([url, dir]{
        std::wstring base = dir;
        if (base.empty()) { wchar_t tmp[MAX_PATH] = {0}; GetTempPathW(MAX_PATH, tmp); base = tmp; }
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
        std::wstring dest = base + L"ED9ModManager-update.zip";
        std::string err;
        bool ok = downloadFile(url, dest, err);
        std::lock_guard<std::mutex> lk(g_upd.mtx);
        if (ok) { g_upd.dlPath = dest; g_upd.phase = 6; }
        else    { g_upd.err = err;    g_upd.phase = 7; }
    }).detach();
}

static bool readWholeFileW(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    size_t off = 0; bool ok = true;
    while (off < out.size()) {
        DWORD want = (DWORD)((out.size() - off) > (1u << 20) ? (1u << 20) : (out.size() - off));
        DWORD rd = 0;
        if (!ReadFile(h, out.data() + off, want, &rd, nullptr) || rd == 0) { ok = false; break; }
        off += rd;
    }
    CloseHandle(h);
    return ok && off == out.size();
}

static void ensureDirW(const std::wstring& dir) {
    for (size_t i = 0; i < dir.size(); ++i) {
        if (dir[i] == L'\\' || dir[i] == L'/') {
            std::wstring sub = dir.substr(0, i);
            if (sub.size() >= 2 && sub[1] == L':') CreateDirectoryW(sub.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static bool writeFileReplace(const std::wstring& target, const void* data, size_t size, std::string& err) {
    size_t slash = target.find_last_of(L"\\/");
    if (slash != std::wstring::npos) ensureDirW(target.substr(0, slash));
    HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) {
            std::wstring old = target + L".old";
            DeleteFileW(old.c_str());
            if (MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
                h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (h == INVALID_HANDLE_VALUE) { err = "无法写入 " + ws2utf8(target); return false; }
    bool ok = true; size_t off = 0; const unsigned char* p = (const unsigned char*)data;
    while (off < size) {
        DWORD want = (DWORD)((size - off) > (1u << 20) ? (1u << 20) : (size - off));
        DWORD wr = 0;
        if (!WriteFile(h, p + off, want, &wr, nullptr) || wr == 0) { ok = false; break; }
        off += wr;
    }
    CloseHandle(h);
    if (!ok) err = "写入失败 " + ws2utf8(target);
    return ok;
}

static bool extractUpdateZip(const std::wstring& zipPath, const std::wstring& root, const std::wstring& selfPath,
                             std::string& err, bool& gotSelf, int& written, int& skipped) {
    gotSelf = false; written = 0; skipped = 0;
    std::vector<unsigned char> zipData;
    if (!readWholeFileW(zipPath, zipData) || zipData.empty()) { err = "读取更新包失败"; return false; }
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) { err = "更新包不是有效 zip"; return false; }
    std::wstring base = root;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    std::wstring self = selfPath; for (auto& c : self) if (c == L'/') c = L'\\';
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        std::string name = st.m_filename;
        for (auto& c : name) if (c == '\\') c = '/';
        const std::string pfx = "ED9ModManager/";
        if (name.rfind(pfx, 0) == 0) name = name.substr(pfx.size());
        if (name.empty()) continue;
        if (name.rfind("Mod/", 0) == 0) { ++skipped; continue; }
        size_t usize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &usize, 0);
        if (!p) { ++skipped; continue; }
        std::wstring rel = utf82ws(name); for (auto& c : rel) if (c == L'/') c = L'\\';
        std::wstring target = base + L"\\" + rel;
        std::string werr;
        if (writeFileReplace(target, p, usize, werr)) {
            ++written;
            if (_wcsicmp(target.c_str(), self.c_str()) == 0) gotSelf = true;
        } else {
            ++skipped; if (err.empty()) err = werr;
        }
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return gotSelf;
}

static void startInstall() {
    std::wstring zip; { std::lock_guard<std::mutex> lk(g_upd.mtx); zip = g_upd.dlPath; }
    g_upd.phase = 8;
    std::thread([zip]{
        std::wstring root = fs::path(zip).parent_path().wstring();
        wchar_t self[MAX_PATH] = {0}; GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::string err; bool gotSelf = false; int written = 0, skipped = 0;
        extractUpdateZip(zip, root, self, err, gotSelf, written, skipped);
        if (gotSelf) DeleteFileW(zip.c_str());
        std::lock_guard<std::mutex> lk(g_upd.mtx);
        if (gotSelf) { g_upd.phase = 9; }
        else { g_upd.err = err.empty() ? "解压失败(未能替换管理器)" : err; g_upd.phase = 10; }
    }).detach();
}

static void restartManager() {
    wchar_t self[MAX_PATH] = {0}; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = fs::path(self).parent_path().wstring();
    ShellExecuteW(nullptr, L"open", self, nullptr, dir.c_str(), SW_SHOWNORMAL);
    PostQuitMessage(0);
}

static void cleanupOldFiles() {
    wchar_t self[MAX_PATH] = {0}; GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path dir = fs::path(self).parent_path();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == L".old") { std::error_code e2; fs::remove(it->path(), e2); }
    }
}

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

    if (g_upd.phase == 3 && !g_upd.dismissed && !g_upd.popupOpened) {
        ImGui::OpenPopup(T("更新检测###update_avail"));
        g_upd.popupOpened = true;
    }
    centerModal();
    if (ImGui::BeginPopupModal(T("更新检测###update_avail"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string latest;
        { std::lock_guard<std::mutex> lk(g_upd.mtx); latest = g_upd.latest; }
        ImGui::Text(T("发现新版本  %s"), latest.c_str());
        ImGui::TextDisabled(T("当前版本 %s"), kVersion);
        ImGui::Spacing();
        ImGui::Separator();
        if (flatButton(a, T("更新"), "upd_go")) { startDownload(utf82ws(a.gameDir)); g_upd.dlPopupOpened = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (flatButton(a, T("以后再说"), "upd_later")) { g_upd.dismissed = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (g_upd.phase >= 5 && !g_upd.dlPopupOpened) {
        ImGui::OpenPopup(T("下载更新###update_dl"));
        g_upd.dlPopupOpened = true;
    }
    centerModal();
    if (ImGui::BeginPopupModal(T("下载更新###update_dl"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int ph = g_upd.phase.load();
        if (ph == 5) {
            long long done = g_upd.dlDone.load(), total = g_upd.dlTotal.load();
            ImGui::TextUnformatted(T("正在下载新版本…"));
            float frac = total > 0 ? (float)((double)done / (double)total) : 0.0f;
            char ov[64];
            if (total > 0) snprintf(ov, sizeof ov, "%.1f / %.1f MB", done / 1048576.0, total / 1048576.0);
            else           snprintf(ov, sizeof ov, "%.1f MB", done / 1048576.0);
            ImGui::ProgressBar(frac, ImVec2(300 * g_dpiScale, 0), ov);
        } else if (ph == 6) {
            std::wstring p; { std::lock_guard<std::mutex> lk(g_upd.mtx); p = g_upd.dlPath; }
            ImGui::TextUnformatted(T("下载完成!点「安装并重启」应用新版本。"));
            ImGui::TextDisabled(T("已保存到:%s"), ws2utf8(p).c_str());
            ImGui::Spacing(); ImGui::Separator();
            if (flatButton(a, T("安装并重启"), "upd_install")) { startInstall(); }
            ImGui::SameLine();
            if (flatButton(a, T("取消"), "upd_cancel")) { g_upd.phase = 2; g_upd.dismissed = true; ImGui::CloseCurrentPopup(); }
        } else if (ph == 8) {
            ImGui::TextUnformatted(T("正在安装更新…"));
            ImGui::TextDisabled(T("解压覆盖中,请勿关闭程序。"));
        } else if (ph == 9) {
            ImGui::TextUnformatted(T("安装完成,正在重启…"));
            if (!g_upd.restartLaunched) { g_upd.restartLaunched = true; restartManager(); }
        } else if (ph == 10) {
            std::string e; { std::lock_guard<std::mutex> lk(g_upd.mtx); e = g_upd.err; }
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), T("安装失败:%s"), e.c_str());
            ImGui::TextDisabled(T("可手动解压更新包覆盖游戏目录。"));
            ImGui::Separator();
            if (flatButton(a, T("关闭"), "upd_instclose")) { g_upd.phase = 2; ImGui::CloseCurrentPopup(); }
        } else {
            std::string e; { std::lock_guard<std::mutex> lk(g_upd.mtx); e = g_upd.err; }
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), T("下载失败:%s"), e.c_str());
            ImGui::Separator();
            if (flatButton(a, T("关闭"), "upd_dlclose")) { g_upd.phase = 2; ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }

    if (g_upd.manual && !g_upd.manualPopupOpened) {
        ImGui::OpenPopup(T("检查更新###upd_manual"));
        g_upd.manualPopupOpened = true;
    }
    centerModal();
    if (ImGui::BeginPopupModal(T("检查更新###upd_manual"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int ph = g_upd.phase.load();
        if (ph <= 1) {
            ImGui::TextUnformatted(T("正在检查更新…"));
        } else if (ph == 3) {
            std::string latest;
            { std::lock_guard<std::mutex> lk(g_upd.mtx); latest = g_upd.latest; }
            ImGui::Text(T("发现新版本  %s"), latest.c_str());
            ImGui::TextDisabled(T("当前版本 %s"), kVersion);
            ImGui::Spacing(); ImGui::Separator();
            if (flatButton(a, T("更新"), "um_go")) {
                startDownload(utf82ws(a.gameDir)); g_upd.dlPopupOpened = false;
                g_upd.manual = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (flatButton(a, T("以后再说"), "um_later")) { g_upd.manual = false; ImGui::CloseCurrentPopup(); }
        } else if (ph == 2) {
            ImGui::Text(T("已是最新版本  v%s"), kVersion);
            ImGui::Spacing(); ImGui::Separator();
            if (flatButton(a, T("确定"), "um_ok")) { g_upd.manual = false; ImGui::CloseCurrentPopup(); }
        } else {
            std::string e; { std::lock_guard<std::mutex> lk(g_upd.mtx); e = g_upd.err; }
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), T("检查失败:%s"), e.c_str());
            ImGui::Spacing(); ImGui::Separator();
            if (flatButton(a, T("确定"), "um_ok2")) { g_upd.manual = false; ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }

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
        if (ImGui::MenuItem(T("更新"))) a.showChangelog = true;
        if (ImGui::MenuItem("PAC")) PacWindowShow();
        {
            char verbuf[40];
            snprintf(verbuf, sizeof verbuf, "v%s", kVersion);
            const char* gd = gameDisplay(a.gameId);
            char gbuf[96];
            if (a.gameDir[0] == 0)      snprintf(gbuf, sizeof gbuf, "%s", T("未设置游戏目录"));
            else if (a.gameId.empty())  snprintf(gbuf, sizeof gbuf, "%s", T("未识别的游戏目录"));
            else                        snprintf(gbuf, sizeof gbuf, "%s", gd);
            float vw = ImGui::CalcTextSize(verbuf).x;
            float gw = ImGui::CalcTextSize(gbuf).x;
            float gap = ImGui::GetStyle().ItemSpacing.x * 2.0f;
            float pad = ImGui::GetStyle().WindowPadding.x + 12.0f * g_dpiScale;
            ImGui::SameLine(ImGui::GetWindowWidth() - vw - gw - gap - pad);
            ImVec4 gc = a.gameId.empty() ? ImVec4(1.00f, 0.70f, 0.30f, 1.0f)
                                         : ImVec4(0.62f, 0.90f, 0.82f, 1.0f);
            ImGui::TextColored(gc, "%s", gbuf);
            ImGui::SameLine(ImGui::GetWindowWidth() - vw - pad);
            ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.95f, 0.90f), "%s", verbuf);
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
    drawChangelog(a);

    if (a.showEnableWarn) {
        if (!ImGui::IsPopupOpen("###enable_warn")) ImGui::OpenPopup(T("适用作品不符###enable_warn"));
        a.showEnableWarn = false;
    }
    centerModal();
    if (ImGui::BeginPopupModal(T("适用作品不符###enable_warn"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        auto it = a.modGame.find(a.enableWarnMod);
        const std::string g = (it != a.modGame.end()) ? it->second : std::string();
        ImGui::Text(T("MOD「%s」"), a.enableWarnMod.c_str());
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.00f, 0.70f, 0.35f, 1.0f), T("适用作品:%s"), gameDisplay(g));
        ImGui::Text(T("当前游戏:%s"), gameDisplay(a.gameId));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("保存时会被拒绝合并。要用它请切换到对应作品的游戏目录,\n或在右侧「MOD 配置」页改它的适用作品。"));
        ImGui::Separator();
        if (flatButton(a, T("知道了"), "bg_ok")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (a.showBadGamePopup) {
        if (!ImGui::IsPopupOpen("###badgame")) ImGui::OpenPopup(T("无法保存###badgame"));
        a.showBadGamePopup = false;
    }
    centerModal();
    if (ImGui::BeginPopupModal(T("无法保存###badgame"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
                           T("以下已启用的 MOD 不适用于当前游戏,为避免改坏数据,本次未合并:"));
        ImGui::Spacing();
        for (const auto& n : a.badGameMods) {
            auto it = a.modGame.find(n);
            const std::string g = (it != a.modGame.end()) ? it->second : std::string();
            ImGui::BulletText("%s  [%s]", n.c_str(), gameDisplay(g));
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("请关掉它们,或在「MOD 配置」页改其适用作品后重试。"));
        ImGui::Separator();
        if (flatButton(a, T("关掉这些 MOD"), "bg_disable")) {
            for (const auto& n : a.badGameMods)
                for (auto& m : a.mods) if (m.name == n) m.enabled = false;
            a.compsFor.clear();
            a.status = T("已关掉不适用于当前游戏的 MOD,可重新保存");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (flatButton(a, T("关闭"), "bg_close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    const char* kFuncs[] = { T("MOD管理"), T("TBL/Json互转"), T("DAT/Json互转"), T("DDS加解密") };
    labeledSeg(T("功能"), "##func", kFuncs, 4, a.funcView, a.funcSegAnim, a.funcSegDrag, 560.0f);
    ImGui::Separator();

    if (a.funcView == 1) { drawConvertTab(a); ImGui::End(); return; }
    if (a.funcView == 2) { drawDatConvertTab(a); ImGui::End(); return; }
    if (a.funcView == 3) { drawDdsTab(a); ImGui::End(); return; }

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
        if (ImGui::Checkbox("##en", &en)) {
            a.mods[i].enabled = en; a.compsFor.clear();
            if (en) {
                auto it = a.modGame.find(a.mods[i].name);
                const std::string g = (it != a.modGame.end()) ? it->second : std::string();
                if (!g.empty() && !a.gameId.empty() && g != a.gameId) {
                    a.enableWarnMod = a.mods[i].name;
                    a.showEnableWarn = true;
                }
            }
        }
        ImGui::SameLine();
        bool hasErr  = a.errorMods.count(a.mods[i].name) > 0;
        bool hasConf = a.conflictMods.count(a.mods[i].name) > 0;
        const std::string& mg = a.modGame[a.mods[i].name];
        bool wrongGame = !mg.empty() && !a.gameId.empty() && mg != a.gameId;
        char gtag[48] = {};
        if (wrongGame) snprintf(gtag, sizeof gtag, "  [%s]", gameDisplay(mg));
        const char* tag = hasErr ? T("  [错误]") : (hasConf ? T("  [冲突]") : "");
        char nm[400];
        snprintf(nm, sizeof nm, "%2d. %s%s%s%s", i + 1, a.mods[i].name.c_str(),
                 a.mods[i].disabled.empty() ? "" : "  *", gtag, tag);
        float rowW = ImGui::GetContentRegionAvail().x;
        bool colored = true;
        if (wrongGame)         ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.60f, 0.25f, 1.0f));
        else if (hasErr)       ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.42f, 0.40f, 1.0f));
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
        if ((wrongGame || hasErr || hasConf || !a.mods[i].disabled.empty()) && ImGui::IsItemHovered()) {
            std::string tip;
            if (wrongGame) tip += T("与当前游戏作品不符。\n");
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
        "  ED9ModManager.exe dat2json <dat文件或目录>... [--out <目录>]\n"
        "                                             #scp 脚本 dat 解成 JSON(往返无损)\n"
        "  ED9ModManager.exe json2dat <json文件或目录>... [--out <目录>]\n"
        "                                             编辑过的 JSON 组装回 dat\n"
        "  ED9ModManager.exe ddsdec <dds文件或目录>... [--out <目录>]\n"
        "                                             解密:游戏格式的 dds -> 普通 dds(图像软件能打开)\n"
        "  ED9ModManager.exe ddsenc <dds文件或目录>... [--out <目录>]\n"
        "                                             加密:普通 dds -> 游戏格式(放进 MOD 前必做)\n"
        "  ED9ModManager.exe --help | -h | help       显示本帮助\n"
        "  ED9ModManager.exe --version                显示版本\n"
        "\n"
        "tbl2json / json2tbl 说明:\n"
        "  · schema 已内置,无需游戏目录;输入可给多个文件或目录(目录取匹配后缀,非递归)。\n"
        "  · --out 缺省时输出到各输入文件的同目录;含未建模字符串池的表(如 NPCParam)拒绝回编。\n"
        "  · 每行 [成功]/[失败];末行 [result] converted=.. failed=..;退出码 0=全成功 1=有失败。\n"
        "  · 示例:ED9ModManager.exe tbl2json \"...\\table_sc\\t_item.tbl\" --out .\n"
        "\n"
        "ddsdec / ddsenc 说明:\n"
        "  · 游戏里的 dds 外面套了一层 LZ4 帧,图像软件打不开;解密就是把那层拆掉。\n"
        "  · ★ 改完必须 ddsenc 回去 —— 裸 dds 放进 MOD 的话引擎会**静默地不画这个网格**(不报错、不崩)。\n"
        "  · --out 缺省时**就地覆盖**输入文件(两边后缀都是 .dds,不给 --out 就没法另存)。\n"
        "  · 已经是目标形态的算 [跳过] 不算失败;成功行会带尺寸/格式,引擎层面的隐患另起 [注意] 行。\n"
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
            std::string text;
            try { text = tblFileToJson(g, src.stem().string()).dump(2); }
            catch (const std::exception& e) { cliWrite("[失败] " + fn + ": 导出 JSON 失败(" + std::string(e.what()) + ")\n"); ++failN; continue; }
            std::ofstream o(dst, std::ios::binary);
            if (!o) { cliWrite("[失败] " + fn + ": 写出失败\n"); ++failN; continue; }
            o << text;
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

static int cliRunDatConvert(bool toJson, int argc, LPWSTR* argv) {
    std::vector<std::wstring> inputs;
    std::wstring outDir;
    for (int i = 2; i < argc; ++i) {
        std::wstring s = argv[i];
        if (s == L"--out" || s == L"-o") { if (i + 1 < argc) outDir = argv[++i]; }
        else inputs.push_back(s);
    }
    if (inputs.empty()) {
        cliWrite(toJson ? "用法: ED9ModManager.exe dat2json <dat文件或目录>... [--out <目录>]\n"
                        : "用法: ED9ModManager.exe json2dat <json文件或目录>... [--out <目录>]\n");
        return 2;
    }
    const wchar_t* inExt  = toJson ? L".dat"  : L".json";
    const wchar_t* outExt = toJson ? L".json" : L".dat";
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
    int ok = 0, failN = 0;
    for (const auto& p : files) {
        fs::path src = p;
        fs::path dst = (outDir.empty() ? src.parent_path() : fs::path(outDir)) / (src.stem().wstring() + outExt);
        std::string fn = ws2utf8(src.filename().wstring());
        std::string stem = ws2utf8(src.stem().wstring());
        std::ifstream f(src, std::ios::binary);
        if (!f) { cliWrite("[失败] " + fn + ": 打不开\n"); ++failN; continue; }
        try {
            if (toJson) {
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
                ed9::Script s = ed9::parse(bytes);
                if (s.name.empty()) s.name = stem;
                std::ofstream o(dst, std::ios::binary);
                if (!o) { cliWrite("[失败] " + fn + ": 写出失败\n"); ++failN; continue; }
                o << datjson::toJson(s, stem).dump(1, '\t');
            } else {
                ojson j = ojson::parse(f);
                if (!j.is_object() || !j.contains("funcs")) {
                    cliWrite("[失败] " + fn + ": 非 dat JSON(缺 funcs)\n"); ++failN; continue;
                }
                std::vector<uint8_t> bytes = ed9::assemble(datjson::fromJson(j));
                if (bytes.empty()) { cliWrite("[失败] " + fn + ": 组装结果为空\n"); ++failN; continue; }
                std::ofstream o(dst, std::ios::binary);
                if (!o) { cliWrite("[失败] " + fn + ": 写出失败\n"); ++failN; continue; }
                o.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            }
        } catch (const std::exception& e) {
            cliWrite("[失败] " + fn + ": " + e.what() + "\n"); ++failN; continue;
        }
        cliWrite("[成功] " + fn + " -> " + ws2utf8(dst.filename().wstring()) + "\n");
        ++ok;
    }
    char buf[160]; snprintf(buf, sizeof buf, "[result] converted=%d failed=%d\n", ok, failN);
    cliWrite(buf);
    return failN > 0 ? 1 : 0;
}

static int cliRunDdsConvert(bool dec, int argc, LPWSTR* argv) {
    std::vector<std::wstring> inputs;
    std::wstring outDir;
    for (int i = 2; i < argc; ++i) {
        std::wstring x = argv[i];
        if (x == L"--out" || x == L"-o") { if (i + 1 < argc) outDir = argv[++i]; }
        else inputs.push_back(x);
    }
    if (inputs.empty()) {
        cliWrite(dec ? "用法: ED9ModManager.exe ddsdec <dds文件或目录>... [--out <目录>]\n"
                     : "用法: ED9ModManager.exe ddsenc <dds文件或目录>... [--out <目录>]\n");
        return 2;
    }
    std::vector<std::wstring> files;
    for (const auto& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            for (auto& e : fs::directory_iterator(in, ec))
                if (e.is_regular_file(ec) && _wcsicmp(e.path().extension().wstring().c_str(), L".dds") == 0)
                    files.push_back(e.path().wstring());
        } else {
            files.push_back(in);
        }
    }
    if (files.empty()) { cliWrite("没有匹配的输入文件(需 .dds)\n"); return 2; }
    if (!outDir.empty()) { std::error_code ec; fs::create_directories(outDir, ec); }

    int ok = 0, skip = 0, failN = 0;
    for (const auto& p : files) {
        const fs::path src = p;
        const std::string fn = ws2utf8(src.filename().wstring());
        std::ifstream f(src, std::ios::binary | std::ios::ate);
        if (!f) { cliWrite("[失败] " + fn + ": 打不开\n"); ++failN; continue; }
        const auto sz = f.tellg(); f.seekg(0);
        std::vector<uint8_t> in((size_t)sz);
        if (sz > 0) f.read(reinterpret_cast<char*>(in.data()), sz);
        f.close();

        const ddsc::Form form = ddsc::Detect(in);
        if ((dec && form == ddsc::Form::Plain) || (!dec && form == ddsc::Form::Game)) {
            cliWrite("[跳过] " + fn + (dec ? ": 已经是普通 DDS\n" : ": 已经是游戏格式\n"));
            ++skip; continue;
        }
        std::vector<uint8_t> out; std::string err;
        if (!(dec ? ddsc::Decrypt(in, out, err) : ddsc::Encrypt(in, out, err))) {
            cliWrite("[失败] " + fn + ": " + err + "\n"); ++failN; continue;
        }
        const fs::path dst = outDir.empty() ? src : (fs::path(outDir) / src.filename());
        std::ofstream o(dst, std::ios::binary);
        if (!o) { cliWrite("[失败] " + fn + ": 写出失败\n"); ++failN; continue; }
        o.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
        o.close();

        const std::vector<uint8_t>& plain = dec ? out : in;
        const ddsc::DdsInfo info = ddsc::Inspect(plain.data(), plain.size());
        char line[512];
        if (info.ok)
            snprintf(line, sizeof line, "[成功] %s  %ux%u mip%u %s  %zu -> %zu\n",
                     fn.c_str(), info.width, info.height, info.mips, info.format.c_str(), in.size(), out.size());
        else
            snprintf(line, sizeof line, "[成功] %s  %zu -> %zu\n", fn.c_str(), in.size(), out.size());
        cliWrite(line);
        for (const auto& w : info.warns) cliWrite("  [注意] " + w + "\n");
        ++ok;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "[result] converted=%d skipped=%d failed=%d\n", ok, skip, failN);
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
        cliWrite(std::string("ED9ModManager ") + kVersion + " (modkit 内置)\n");
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
    } else if (eq(L"dat2json")) {
        return cliRunDatConvert(true, argc, argv);
    } else if (eq(L"json2dat")) {
        return cliRunDatConvert(false, argc, argv);
    } else if (eq(L"json2tbl")) {
        ret = cliRunTblConvert(false, argc, argv);
    } else if (eq(L"ddsdec")) {
        return cliRunDdsConvert(true, argc, argv);
    } else if (eq(L"ddsenc")) {
        return cliRunDdsConvert(false, argc, argv);
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
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
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

struct SubWindow {
    HWND                    hwnd = nullptr;
    IDXGISwapChain*         swap = nullptr;
    ID3D11RenderTargetView* rtv  = nullptr;
    ImGuiContext*           ctx  = nullptr;
    bool                    visible = false;
};
static SubWindow g_pacWin;

static const char* kSubExtraGlyphs =
    "一上下不且个中临为了些从件任份会传位何保候偏先入"
    "全共内写几出到制前加动包占原去取另可右同名在填增"
    "备复多够大失头夹好存它完定容对导小就左已帧底建开"
    "引归当录径得必戏成或所打择挂按换据提搜撤改数整文"
    "新时是替有未条来标根档正毫没消渲游源满滤点版用留"
    "的盖盘目相看磁示秒称移稍空索给置能自被要覆角记设"
    "请读败资路过还这进选透部里销键间项";

static HINSTANCE g_hInst    = nullptr;
static HICON     g_hIconBig = nullptr;
static HICON     g_hIconSm  = nullptr;

static void subMakeRTV(SubWindow& w) {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(w.swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
        g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &w.rtv);
        bb->Release();
    }
}
static void subFreeRTV(SubWindow& w) { if (w.rtv) { w.rtv->Release(); w.rtv = nullptr; } }

static LRESULT WINAPI SubWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lParam)->lpCreateParams);
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    SubWindow* w = (SubWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (w && w->ctx) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(w->ctx);
        const LRESULT eaten = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        ImGui::SetCurrentContext(prev);
        if (eaten) return true;
    }
    switch (msg) {
    case WM_SIZE:
        if (w && w->swap && wParam != SIZE_MINIMIZED) {
            subFreeRTV(*w);
            w->swap->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            subMakeRTV(*w);
        }
        return 0;
    case WM_CLOSE:
        if (w) { ShowWindow(hWnd, SW_HIDE); w->visible = false; }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool subCreateSwapChain(SubWindow& w) {
    IDXGIDevice*  dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev))) &&
        SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = w.hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (SUCCEEDED(factory->CreateSwapChain(g_pd3dDevice, &sd, &w.swap))) { subMakeRTV(w); ok = true; }
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDev) dxgiDev->Release();
    return ok;
}

struct PacTree {
    struct Dir {
        std::string      name;
        int              parent = -1;
        std::vector<int> subs;
        std::vector<int> files;
        uint64_t         bytes = 0;
        int              total = 0;
    };
    struct File {
        std::string  path;
        std::string  name;
        uint64_t     size = 0;
        uint64_t     offset = 0;
        std::wstring replaceFrom;
        bool         isNew = false;
        int          dir = 0;
    };
    std::vector<Dir>  dirs;
    std::vector<File> files;
};

struct PacJob {
    std::atomic<bool>     running{ false };
    std::atomic<bool>     done{ false };
    std::atomic<uint64_t> cur{ 0 }, total{ 0 };
    std::mutex            mtx;
    std::string           title;
    std::string           result;
    std::string           err;
    std::wstring          reopen;
    std::thread           th;
};
static PacJob g_pacJob;

struct PacState {
    bool         loaded = false;
    std::wstring path;
    std::string  title;
    std::string  err;
    PacTree      tree;
    int          cur = 0;
    std::set<int> sel;
    int          lastClick = -1;
    char         filter[128] = {};
    double       openMs = 0;
    uint64_t     fileBytes = 0;
    int          pendingExpand = -1;
    std::vector<std::wstring> gamePacs;
    std::string  gameDirScanned;
    std::string  toast;
    double       toastAt = 0;
    bool         askOverwrite = false;
};
static PacState    g_pacState;
static std::string g_pacGameDir;

static void pacToast(const std::string& msg) {
    g_pacState.toast = msg;
    g_pacState.toastAt = ImGui::GetTime();
}

static std::string pacHumanSize(uint64_t b) {
    char t[48];
    if      (b >= (1ull << 30)) snprintf(t, sizeof t, "%.2f GB", (double)b / (double)(1ull << 30));
    else if (b >= (1ull << 20)) snprintf(t, sizeof t, "%.2f MB", (double)b / (double)(1ull << 20));
    else if (b >= (1ull << 10)) snprintf(t, sizeof t, "%.1f KB", (double)b / (double)(1ull << 10));
    else                        snprintf(t, sizeof t, "%llu B", (unsigned long long)b);
    return t;
}

static int pacEnsureDir(PacTree& t, int parent, const std::string& name) {
    for (int i : t.dirs[parent].subs) if (t.dirs[i].name == name) return i;
    PacTree::Dir d; d.name = name; d.parent = parent;
    t.dirs.push_back(std::move(d));
    const int idx = (int)t.dirs.size() - 1;
    t.dirs[parent].subs.push_back(idx);
    return idx;
}

static void pacBuildTree() {
    PacTree& t = g_pacState.tree;
    t.dirs.clear();
    t.dirs.push_back(PacTree::Dir{});
    for (int fi = 0; fi < (int)t.files.size(); ++fi) {
        PacTree::File& f = t.files[fi];
        int dir = 0;
        size_t start = 0;
        for (;;) {
            const size_t sl = f.path.find('/', start);
            if (sl == std::string::npos) { f.name = f.path.substr(start); break; }
            if (sl > start) dir = pacEnsureDir(t, dir, f.path.substr(start, sl - start));
            start = sl + 1;
        }
        f.dir = dir;
        t.dirs[dir].files.push_back(fi);
        for (int d = dir; d >= 0; d = t.dirs[d].parent) { t.dirs[d].bytes += f.size; t.dirs[d].total += 1; }
    }
    for (auto& d : t.dirs) {
        std::sort(d.subs.begin(), d.subs.end(),
                  [&t](int a, int b) { return _stricmp(t.dirs[a].name.c_str(), t.dirs[b].name.c_str()) < 0; });
        std::sort(d.files.begin(), d.files.end(),
                  [&t](int a, int b) { return _stricmp(t.files[a].name.c_str(), t.files[b].name.c_str()) < 0; });
    }
}

static std::string pacDirPath(const PacTree& t, int di) {
    std::string s;
    for (int d = di; d > 0; d = t.dirs[d].parent) s = "/" + t.dirs[d].name + s;
    return s.empty() ? "/" : s;
}
static std::string pacDirPrefix(const PacTree& t, int di) {
    const std::string p = pacDirPath(t, di);
    return p == "/" ? std::string() : p.substr(1);
}

static int pacChangedCount(bool wantNew) {
    int n = 0;
    for (const auto& f : g_pacState.tree.files)
        if (wantNew ? f.isNew : (!f.isNew && !f.replaceFrom.empty())) ++n;
    return n;
}
static bool pacDirty() { return pacChangedCount(true) > 0 || pacChangedCount(false) > 0; }

static bool pacOpen(const std::wstring& p) {
    g_pacState.loaded = false;
    g_pacState.err.clear();
    g_pacState.tree = PacTree{};
    g_pacState.cur = 0;
    g_pacState.sel.clear();
    g_pacState.lastClick = -1;
    g_pacState.path = p;
    g_pacState.title = ws2utf8(fs::path(p).filename().wstring());

    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);

    mk::FpacReader r;
    if (!r.Open(p)) { g_pacState.err = g_pacState.title + T(":打不开,或不是 FPAC 归档"); return false; }

    PacTree& t = g_pacState.tree;
    t.files.reserve(r.Count());
    for (const auto& e : r.Entries()) {
        PacTree::File f;
        f.path = e.name;
        f.size = e.size;
        f.offset = e.location;
        t.files.push_back(std::move(f));
    }
    pacBuildTree();

    {
        int d = 0;
        while (t.dirs[d].files.empty() && t.dirs[d].subs.size() == 1) d = t.dirs[d].subs[0];
        g_pacState.cur = d;
        g_pacState.pendingExpand = d;
    }

    QueryPerformanceCounter(&t1);
    g_pacState.openMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f0.QuadPart;
    std::error_code ec;
    g_pacState.fileBytes = (uint64_t)fs::file_size(p, ec);
    g_pacState.loaded = true;
    if (g_pacWin.hwnd) {
        const std::wstring cap = L"PAC — " + fs::path(p).filename().wstring();
        SetWindowTextW(g_pacWin.hwnd, cap.c_str());
    }
    return true;
}

static void pacScanGamePacs() {
    if (g_pacState.gameDirScanned == g_pacGameDir) return;
    g_pacState.gameDirScanned = g_pacGameDir;
    g_pacState.gamePacs.clear();
    if (g_pacGameDir.empty()) return;
    std::error_code ec;
    const fs::path dir = fs::path(utf82ws(g_pacGameDir)) / L"pac" / L"steam";
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && _wcsicmp(e.path().extension().wstring().c_str(), L".pac") == 0)
            g_pacState.gamePacs.push_back(e.path().wstring());
    std::sort(g_pacState.gamePacs.begin(), g_pacState.gamePacs.end());
}

static void pacJobFinish() {
    if (!g_pacJob.done.load()) return;
    if (g_pacJob.th.joinable()) g_pacJob.th.join();
    std::wstring reopen;
    std::string  msg;
    { std::lock_guard<std::mutex> lk(g_pacJob.mtx);
      msg = g_pacJob.err.empty() ? g_pacJob.result : (std::string(T("失败:")) + g_pacJob.err);
      reopen = g_pacJob.reopen; }
    g_pacJob.done = false;
    g_pacJob.running = false;
    if (!reopen.empty()) pacOpen(reopen);
    pacToast(msg);
}
static bool pacBusy() { return g_pacJob.running.load(); }

static void pacStartExport(std::vector<int> idx, const fs::path& outDir) {
    if (pacBusy() || idx.empty()) return;
    if (g_pacJob.th.joinable()) g_pacJob.th.join();
    uint64_t total = 0;
    for (int i : idx) total += g_pacState.tree.files[i].size;
    g_pacJob.cur = 0; g_pacJob.total = total; g_pacJob.done = false; g_pacJob.running = true;
    { std::lock_guard<std::mutex> lk(g_pacJob.mtx);
      g_pacJob.title = T("正在导出…"); g_pacJob.result.clear(); g_pacJob.err.clear(); g_pacJob.reopen.clear(); }
    const std::wstring src = g_pacState.path;
    struct Item { std::string path; uint64_t off, size; std::wstring from; };
    auto items = std::make_shared<std::vector<Item>>();
    for (int i : idx) {
        const auto& f = g_pacState.tree.files[i];
        items->push_back({ f.path, f.offset, f.size, f.replaceFrom });
    }
    g_pacJob.th = std::thread([src, outDir, items]() {
        std::string err;
        int ok = 0;
        mk::FpacExtractor ex;
        ex.Open(src, err);
        auto prog = [](uint64_t n) { g_pacJob.cur += n; };
        for (const auto& it : *items) {
            const mk::FpacItem fi{ it.path, it.size, it.off, it.from };
            if (!ex.ExtractTo(fi, (outDir / utf82ws(it.path)).wstring(), prog, err)) break;
            ++ok;
        }
        char b[160];
        snprintf(b, sizeof b, T("已导出 %d 个文件"), ok);
        { std::lock_guard<std::mutex> lk(g_pacJob.mtx); g_pacJob.result = b; g_pacJob.err = err; }
        g_pacJob.done = true;
    });
}

static void pacCollectUnder(int di, std::vector<int>& out) {
    const PacTree& t = g_pacState.tree;
    for (int f : t.dirs[di].files) out.push_back(f);
    for (int d : t.dirs[di].subs) pacCollectUnder(d, out);
}

static void pacImportOne(const std::string& intoPath, const std::wstring& diskFile,
                         int& replaced, int& added) {
    std::error_code ec;
    const uint64_t sz = (uint64_t)fs::file_size(diskFile, ec);
    for (auto& f : g_pacState.tree.files) {
        if (_stricmp(f.path.c_str(), intoPath.c_str()) == 0) {
            f.replaceFrom = diskFile;
            f.size = sz;
            ++replaced;
            return;
        }
    }
    PacTree::File nf;
    nf.path = intoPath;
    nf.size = sz;
    nf.replaceFrom = diskFile;
    nf.isNew = true;
    g_pacState.tree.files.push_back(std::move(nf));
    ++added;
}

static void pacImportFiles(const std::vector<std::wstring>& disk) {
    const std::string prefix = pacDirPrefix(g_pacState.tree, g_pacState.cur);
    int rep = 0, add = 0;
    for (const auto& d : disk) {
        const std::string leaf = ws2utf8(fs::path(d).filename().wstring());
        pacImportOne(prefix.empty() ? leaf : (prefix + "/" + leaf), d, rep, add);
    }
    pacBuildTree();
    g_pacState.sel.clear();
    char b[160]; snprintf(b, sizeof b, T("导入完成:替换 %d,新增 %d(记得保存)"), rep, add);
    pacToast(b);
}

static void pacImportFolder(const std::wstring& folder) {
    const std::string prefix = pacDirPrefix(g_pacState.tree, g_pacState.cur);
    int rep = 0, add = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = ws2utf8(fs::relative(it->path(), folder, ec).wstring());
        for (char& c : rel) if (c == '\\') c = '/';
        pacImportOne(prefix.empty() ? rel : (prefix + "/" + rel), it->path().wstring(), rep, add);
    }
    pacBuildTree();
    g_pacState.sel.clear();
    char b[160]; snprintf(b, sizeof b, T("导入完成:替换 %d,新增 %d(记得保存)"), rep, add);
    pacToast(b);
}

static void pacStartSave(const fs::path& target) {
    if (pacBusy() || !g_pacState.loaded) return;
    if (g_pacJob.th.joinable()) g_pacJob.th.join();

    auto items = std::make_shared<std::vector<mk::FpacItem>>();
    uint64_t total = 0;
    for (const auto& f : g_pacState.tree.files) {
        items->push_back(mk::FpacItem{ f.path, f.size, f.offset, f.replaceFrom });
        total += f.size;
    }
    g_pacJob.cur = 0; g_pacJob.total = total; g_pacJob.done = false; g_pacJob.running = true;
    { std::lock_guard<std::mutex> lk(g_pacJob.mtx);
      g_pacJob.title = T("正在保存…"); g_pacJob.result.clear(); g_pacJob.err.clear(); g_pacJob.reopen.clear(); }
    const std::wstring src = g_pacState.path;

    g_pacJob.th = std::thread([src, target, items]() {
        std::string err;
        auto prog = [](uint64_t n) { g_pacJob.cur += n; };
        const bool ok = mk::FpacWrite(src, *items, target.wstring(), prog, err);
        char b[300];
        snprintf(b, sizeof b, T("已保存:%s(原件备份为同名 .bak)"),
                 ws2utf8(fs::path(target).filename().wstring()).c_str());
        { std::lock_guard<std::mutex> lk(g_pacJob.mtx);
          g_pacJob.result = b; g_pacJob.err = err;
          if (ok) g_pacJob.reopen = target.wstring(); }
        g_pacJob.done = true;
    });
}

static bool pacBrowseSave(std::wstring& out, const std::wstring& initName) {
    static wchar_t buf[4096];
    wcscpy_s(buf, initName.c_str());
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"pac 归档 (*.pac)\0*.pac\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(wchar_t));
    ofn.lpstrTitle = L"另存为";
    ofn.lpstrDefExt = L"pac";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

static bool subFlatButton(const char* label, const char* id, bool enabled = true) {
    const ImGuiStyle& st = ImGui::GetStyle();
    static std::map<std::string, float> anim;
    if (!enabled) ImGui::BeginDisabled();
    const ImVec4 base = st.Colors[ImGuiCol_Button];
    const std::string btnId = std::string(label) + "###" + id;
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, base);
    const bool clicked = ImGui::Button(btnId.c_str());
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    float& a = anim[id];
    float sp = ImGui::GetIO().DeltaTime * 14.0f; if (sp > 1.0f) sp = 1.0f;
    a += (((hov && enabled) ? 1.0f : 0.0f) - a) * sp;
    if (a > 0.01f)
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                            IM_COL32(255, 255, 255, (int)(230 * a)), st.FrameRounding, 0, 2.0f);
    if (!enabled) ImGui::EndDisabled();
    return clicked && enabled;
}

static bool subStyledSelectable(const char* label, bool selected) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const bool clicked = ImGui::Selectable(label, selected);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const float rnd = ImGui::GetStyle().FrameRounding;
    const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.5f);
    dl->ChannelsSetCurrent(0);
    if (selected) dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_Button, 0.55f), rnd);
    dl->ChannelsMerge();
    ImGui::PopStyleColor(3);
    if (hov)      dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, (int)(16 + 34 * pulse)), rnd);
    if (selected) dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 220), rnd, 0, 1.5f);
    if (hov)      dl->AddRect(mn, mx, IM_COL32(255, 255, 255, (int)(80 + 140 * pulse)), rnd, 0, 1.5f);
    return clicked;
}

static void pacDrawDirNode(int di) {
    const PacTree& t = g_pacState.tree;
    const PacTree::Dir& d = t.dirs[di];
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (d.subs.empty()) f |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (di == g_pacState.cur) f |= ImGuiTreeNodeFlags_Selected;
    if (di == 0) f |= ImGuiTreeNodeFlags_DefaultOpen;
    if (g_pacState.pendingExpand >= 0)
        for (int a = g_pacState.pendingExpand; a >= 0; a = t.dirs[a].parent)
            if (a == di) { ImGui::SetNextItemOpen(true); break; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const bool open = ImGui::TreeNodeEx((void*)(intptr_t)di, f, "%s  (%d)",
                                        di == 0 ? T("(根)") : d.name.c_str(), d.total);
    const bool nodeHov = ImGui::IsItemHovered();
    const ImVec2 nmn = ImGui::GetItemRectMin(), nmx = ImGui::GetItemRectMax();
    const float rnd = ImGui::GetStyle().FrameRounding;
    const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.5f);
    dl->ChannelsSetCurrent(0);
    if (di == g_pacState.cur) dl->AddRectFilled(nmn, nmx, ImGui::GetColorU32(ImGuiCol_Button, 0.55f), rnd);
    dl->ChannelsMerge();
    ImGui::PopStyleColor(3);
    if (nodeHov)              dl->AddRectFilled(nmn, nmx, IM_COL32(255, 255, 255, (int)(16 + 34 * pulse)), rnd);
    if (di == g_pacState.cur) dl->AddRect(nmn, nmx, IM_COL32(255, 255, 255, 220), rnd, 0, 1.5f);
    if (nodeHov)              dl->AddRect(nmn, nmx, IM_COL32(255, 255, 255, (int)(80 + 140 * pulse)), rnd, 0, 1.5f);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        g_pacState.cur = di; g_pacState.sel.clear(); g_pacState.lastClick = -1;
    }
    if (open && !d.subs.empty()) {
        for (int sd : d.subs) pacDrawDirNode(sd);
        ImGui::TreePop();
    }
}

static bool pacContainsCI(const std::string& hay, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    if (hay.size() < needleLower.size()) return false;
    for (size_t i = 0; i + needleLower.size() <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < needleLower.size(); ++j)
            if ((char)tolower((unsigned char)hay[i + j]) != needleLower[j]) break;
        if (j == needleLower.size()) return true;
    }
    return false;
}

static void drawPacContent() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##pacroot", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    pacJobFinish();
    const bool busy = pacBusy();

    if (subFlatButton(T("打开 .pac…"), "pac_open", !busy)) {
        std::vector<std::wstring> sel;
        if (browseFilesMulti(sel, L"pac 归档 (*.pac)\0*.pac\0所有文件 (*.*)\0*.*\0", L"选择一个 .pac")
            && !sel.empty())
            pacOpen(sel[0]);
    }
    ImGui::SameLine();
    pacScanGamePacs();
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(300.0f * g_dpiScale);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_Button));
    const bool comboOpen = ImGui::BeginCombo("##gamepac", T("游戏里的 pac…"));
    ImGui::PopStyleColor(2);
    if (comboOpen) {
        if (g_pacState.gamePacs.empty())
            ImGui::TextDisabled("%s", T("(未设置游戏目录,或 pac\\steam 下没有 .pac)"));
        for (const auto& p : g_pacState.gamePacs) {
            const std::string nm = ws2utf8(fs::path(p).filename().wstring());
            const bool cur = _wcsicmp(p.c_str(), g_pacState.path.c_str()) == 0;
            if (subStyledSelectable(nm.c_str(), cur)) pacOpen(p);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (g_pacState.loaded) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.62f, 0.90f, 0.82f, 1.0f), "%s", g_pacState.title.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(T("条目 %d · 内容 %s · 文件 %s · 索引 %.1f 毫秒"),
                            (int)g_pacState.tree.files.size(),
                            pacHumanSize(g_pacState.tree.dirs[0].bytes).c_str(),
                            pacHumanSize(g_pacState.fileBytes).c_str(), g_pacState.openMs);
    }

    if (!g_pacState.err.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.40f, 1.0f), "%s", g_pacState.err.c_str());
    if (!g_pacState.loaded) {
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("还没打开任何 pac。点左上角「打开 .pac…」,或从「游戏里的 pac」里选一个。"));
        ImGui::End();
        return;
    }

    const PacTree& t = g_pacState.tree;

    {
        char lb[64];
        snprintf(lb, sizeof lb, T("导出选中(%d)"), (int)g_pacState.sel.size());
        if (subFlatButton(lb, "pac_exp_sel", !busy && !g_pacState.sel.empty())) {
            std::wstring d;
            if (browseFolder(d, L"选择导出目录"))
                pacStartExport(std::vector<int>(g_pacState.sel.begin(), g_pacState.sel.end()), d);
        }
        ImGui::SameLine();
        if (subFlatButton(T("导出当前目录"), "pac_exp_dir", !busy)) {
            std::wstring d;
            if (browseFolder(d, L"选择导出目录")) {
                std::vector<int> idx; pacCollectUnder(g_pacState.cur, idx);
                pacStartExport(std::move(idx), d);
            }
        }
        ImGui::SameLine();
        if (subFlatButton(T("导出全部"), "pac_exp_all", !busy)) {
            std::wstring d;
            if (browseFolder(d, L"选择导出目录")) {
                std::vector<int> idx; pacCollectUnder(0, idx);
                pacStartExport(std::move(idx), d);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (subFlatButton(T("导入文件…"), "pac_imp_f", !busy)) {
            std::vector<std::wstring> sel;
            if (browseFilesMulti(sel, L"所有文件 (*.*)\0*.*\0", L"选择要导入的文件(可多选)") && !sel.empty())
                pacImportFiles(sel);
        }
        ImGui::SameLine();
        if (subFlatButton(T("导入文件夹…"), "pac_imp_d", !busy)) {
            std::wstring d;
            if (browseFolder(d, L"选择要导入的文件夹(按相对路径挂进当前目录)")) pacImportFolder(d);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (subFlatButton(T("保存"), "pac_save", !busy)) g_pacState.askOverwrite = true;
        ImGui::SameLine();
        if (subFlatButton(T("另存为…"), "pac_saveas", !busy)) {
            std::wstring d;
            if (pacBrowseSave(d, fs::path(g_pacState.path).filename().wstring())) pacStartSave(d);
        }
        ImGui::SameLine();
        if (subFlatButton(T("撤销改动"), "pac_revert", !busy && pacDirty())) pacOpen(g_pacState.path);
        const int rep = pacChangedCount(false), add = pacChangedCount(true);
        if (rep || add) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.00f, 0.70f, 0.30f, 1.0f), T("未保存:替换 %d,新增 %d"), rep, add);
        }
    }
    ImGui::Separator();

    const float leftW = ImGui::GetContentRegionAvail().x * 0.32f;
    ImGui::BeginChild("pactree", ImVec2(leftW, 0), true);
    pacDrawDirNode(0);
    g_pacState.pendingExpand = -1;
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("pacfiles", ImVec2(0, 0), true);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("过滤"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pacfilter", g_pacState.filter, sizeof(g_pacState.filter));

    static std::vector<int> shown;
    shown.clear();
    const bool searching = g_pacState.filter[0] != 0;
    if (searching) {
        std::string needle = g_pacState.filter;
        for (char& c : needle) c = (char)tolower((unsigned char)c);
        for (int i = 0; i < (int)t.files.size(); ++i)
            if (pacContainsCI(t.files[i].path, needle)) shown.push_back(i);
    } else {
        shown = t.dirs[g_pacState.cur].files;
    }

    if (searching) ImGui::TextDisabled(T("在整个包里搜到 %d 项"), (int)shown.size());
    else           ImGui::TextDisabled(T("当前目录 %s — 共 %d 项"),
                                       pacDirPath(t, g_pacState.cur).c_str(), (int)shown.size());

    const ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (ImGui::BeginTable("pactbl", 3, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(T("名称"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn(T("大小"), ImGuiTableColumnFlags_WidthFixed, 110.0f * g_dpiScale);
        ImGui::TableSetupColumn(T("包内偏移"), ImGuiTableColumnFlags_WidthFixed, 140.0f * g_dpiScale);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
            if (sp->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
                const bool asc = (c.SortDirection == ImGuiSortDirection_Ascending);
                std::sort(shown.begin(), shown.end(), [&](int a, int b) {
                    int r = 0;
                    if      (c.ColumnIndex == 1) r = (t.files[a].size   < t.files[b].size)   ? -1 : (t.files[a].size   > t.files[b].size)   ? 1 : 0;
                    else if (c.ColumnIndex == 2) r = (t.files[a].offset < t.files[b].offset) ? -1 : (t.files[a].offset > t.files[b].offset) ? 1 : 0;
                    if (r == 0) r = _stricmp(t.files[a].name.c_str(), t.files[b].name.c_str());
                    return asc ? (r < 0) : (r > 0);
                });
            }
        }

        ImGuiListClipper clip;
        clip.Begin((int)shown.size());
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                const int fi = shown[row];
                const PacTree::File& f = t.files[fi];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(fi);
                std::string label = searching ? f.path : f.name;
                if (f.isNew)                   label = "＋ " + label;
                else if (!f.replaceFrom.empty()) label = "● " + label;
                if (f.isNew || !f.replaceFrom.empty())
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.35f, 1.0f));
                const bool selected = g_pacState.sel.count(fi) > 0;
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
                const bool clickedRow = ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns);
                const bool rowHov = ImGui::IsItemHovered();
                const ImVec2 rmn = ImGui::GetItemRectMin(), rmx = ImGui::GetItemRectMax();
                ImGui::PopStyleColor(3);
                {
                    ImGui::TablePushBackgroundChannel();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float rnd = ImGui::GetStyle().FrameRounding;
                    const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.5f);
                    if (selected) {
                        const bool prevSel = row > 0 && g_pacState.sel.count(shown[row - 1]) > 0;
                        const bool nextSel = (row + 1 < (int)shown.size())
                                             && g_pacState.sel.count(shown[row + 1]) > 0;
                        ImVec2 a = rmn, b = rmx;
                        if (prevSel) a.y -= rnd + 4.0f;
                        if (nextSel) b.y += rnd + 4.0f;
                        dl->PushClipRect(rmn, rmx, true);
                        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_Button, 0.55f), rnd);
                        dl->AddRect(a, b, IM_COL32(255, 255, 255, 220), rnd, 0, 1.5f);
                        dl->PopClipRect();
                    }
                    if (rowHov) {
                        dl->AddRectFilled(rmn, rmx, IM_COL32(255, 255, 255, (int)(16 + 34 * pulse)), rnd);
                        dl->AddRect(rmn, rmx, IM_COL32(255, 255, 255, (int)(80 + 140 * pulse)), rnd, 0, 1.5f);
                    }
                    ImGui::TablePopBackgroundChannel();
                }
                if (clickedRow) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl) {
                        if (selected) g_pacState.sel.erase(fi); else g_pacState.sel.insert(fi);
                    } else if (io.KeyShift && g_pacState.lastClick >= 0) {
                        int a = -1, b = -1;
                        for (int k = 0; k < (int)shown.size(); ++k) {
                            if (shown[k] == g_pacState.lastClick) a = k;
                            if (shown[k] == fi) b = k;
                        }
                        if (a >= 0 && b >= 0) {
                            if (a > b) std::swap(a, b);
                            for (int k = a; k <= b; ++k) g_pacState.sel.insert(shown[k]);
                        }
                    } else {
                        g_pacState.sel.clear();
                        g_pacState.sel.insert(fi);
                    }
                    g_pacState.lastClick = fi;
                }
                if (ImGui::BeginPopupContextItem("pacrow")) {
                    if (g_pacState.sel.count(fi) == 0) { g_pacState.sel.clear(); g_pacState.sel.insert(fi); }
                    if (ImGui::MenuItem(T("复制内部路径"))) {
                        ImGui::SetClipboardText(f.path.c_str());
                        pacToast(T("已复制内部路径"));
                    }
                    if (ImGui::MenuItem(T("导出这些…"), nullptr, false, !busy)) {
                        std::wstring d;
                        if (browseFolder(d, L"选择导出目录"))
                            pacStartExport(std::vector<int>(g_pacState.sel.begin(), g_pacState.sel.end()), d);
                    }
                    if (!f.replaceFrom.empty() && ImGui::MenuItem(T("取消这条的替换"))) {
                        g_pacState.tree.files[fi].replaceFrom.clear();
                        if (g_pacState.tree.files[fi].isNew) { pacOpen(g_pacState.path); }
                    }
                    ImGui::EndPopup();
                }
                if (f.isNew || !f.replaceFrom.empty()) ImGui::PopStyleColor();
                if (!f.replaceFrom.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip(T("数据来自:%s"), ws2utf8(f.replaceFrom).c_str());
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(pacHumanSize(f.size).c_str());
                ImGui::TableSetColumnIndex(2);
                if (f.isNew) ImGui::TextDisabled("%s", T("(新增)"));
                else         ImGui::Text("0x%llX", (unsigned long long)f.offset);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (g_pacState.sel.size() == 1) {
        const int fi = *g_pacState.sel.begin();
        if (fi >= 0 && fi < (int)t.files.size()) {
            ImGui::TextDisabled("%s", T("内部路径(资源覆盖时用它当键,右键可复制):"));
            ImGui::SameLine();
            ImGui::TextUnformatted(t.files[fi].path.c_str());
        }
    }
    if (!g_pacState.toast.empty() && ImGui::GetTime() - g_pacState.toastAt < 4.0)
        ImGui::TextColored(ImVec4(0.62f, 0.90f, 0.82f, 1.0f), "%s", g_pacState.toast.c_str());

    if (g_pacState.askOverwrite) {
        ImGui::OpenPopup(T("覆盖原包?###pacovr"));
        g_pacState.askOverwrite = false;
    }
    if (ImGui::BeginPopupModal(T("覆盖原包?###pacovr"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(T("要覆盖:%s"), ws2utf8(g_pacState.path).c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("原文件会先改名成同名的 .pac.bak 留底(改名不复制,几秒就好)。"));
        ImGui::TextDisabled("%s", T("提示:给游戏加资源不必改原包 —— MOD 的 asset 透传就能覆盖,且不动原版文件。"));
        ImGui::Separator();
        if (subFlatButton(T("覆盖"), "ovr_yes")) { pacStartSave(g_pacState.path); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (subFlatButton(T("取消"), "ovr_no")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (busy) {
        ImGui::OpenPopup(T("请稍候###pacjob"));
    }
    if (ImGui::BeginPopupModal(T("请稍候###pacjob"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string ttl;
        { std::lock_guard<std::mutex> lk(g_pacJob.mtx); ttl = g_pacJob.title; }
        ImGui::TextUnformatted(ttl.c_str());
        const uint64_t c = g_pacJob.cur.load(), tt = g_pacJob.total.load();
        char ov[80];
        snprintf(ov, sizeof ov, "%s / %s", pacHumanSize(c).c_str(), pacHumanSize(tt).c_str());
        ImGui::ProgressBar(tt ? (float)((double)c / (double)tt) : 0.0f, ImVec2(360 * g_dpiScale, 0), ov);
        if (!busy) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

static bool pacWindowEnsure() {
    if (g_pacWin.hwnd) return true;
    if (!g_pd3dDevice) return false;

    static const wchar_t* kSubClass = L"ED9ModManagerSub";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, SubWndProc, 0, 0, g_hInst,
                           g_hIconBig, nullptr, nullptr, nullptr, kSubClass, g_hIconSm };
        RegisterClassExW(&wc);
        registered = true;
    }
    g_pacWin.hwnd = CreateWindowW(kSubClass, L"PAC", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  (int)(900 * g_dpiScale), (int)(600 * g_dpiScale),
                                  nullptr, nullptr, g_hInst, &g_pacWin);
    if (!g_pacWin.hwnd) return false;
    if (!subCreateSwapChain(g_pacWin)) { DestroyWindow(g_pacWin.hwnd); g_pacWin.hwnd = nullptr; return false; }

    ImGuiContext* prev = ImGui::GetCurrentContext();
    g_pacWin.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_pacWin.ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    applyModernStyle();
    ImGui::GetStyle().ScaleAllSizes(g_dpiScale);
    static ImVector<ImWchar> subRanges;
    if (subRanges.empty()) {
        ImFontGlyphRangesBuilder gb;
        gb.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        gb.AddText(kSubExtraGlyphs);
        gb.BuildRanges(&subRanges);
    }
    const float px = 18.0f * g_dpiScale;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", px, nullptr, subRanges.Data))
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", px, nullptr, subRanges.Data))
            io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(g_pacWin.hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    ImGui::SetCurrentContext(prev);
    return true;
}

static void PacWindowShow() {
    if (!pacWindowEnsure()) return;
    ShowWindow(g_pacWin.hwnd, SW_SHOW);
    SetForegroundWindow(g_pacWin.hwnd);
    g_pacWin.visible = true;
}

static void PacWindowFrame() {
    if (!g_pacWin.visible || !g_pacWin.ctx || !g_pacWin.rtv) return;
    if (IsIconic(g_pacWin.hwnd)) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_pacWin.ctx);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    drawPacContent();
    ImGui::Render();
    const float clear[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pacWin.rtv, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_pacWin.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pacWin.swap->Present(0, 0);
    ImGui::SetCurrentContext(prev);
}

static void PacWindowShutdown() {
    if (g_pacWin.ctx) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_pacWin.ctx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::SetCurrentContext(prev == g_pacWin.ctx ? nullptr : prev);
        ImGui::DestroyContext(g_pacWin.ctx);
        g_pacWin.ctx = nullptr;
    }
    subFreeRTV(g_pacWin);
    if (g_pacWin.swap) { g_pacWin.swap->Release(); g_pacWin.swap = nullptr; }
    if (g_pacWin.hwnd) { DestroyWindow(g_pacWin.hwnd); g_pacWin.hwnd = nullptr; }
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
    g_hInst = hInst; g_hIconBig = hIconBig; g_hIconSm = hIconSm;
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

    cleanupOldFiles();
    startUpdateCheck();

    App app;
    loadIniGameDir(app);
    if (app.gameDir[0]) {
        std::wstring g = utf82ws(app.gameDir);
        if (!g.empty() && g.back() != L'\\' && g.back() != L'/') g += L'\\';
        DeleteFileW((g + L"ED9ModManager-update.zip").c_str());
    }
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

        g_pacGameDir = app.gameDir;
        PacWindowFrame();
    }

    closeWatch(app);
    PacWindowShutdown();
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
