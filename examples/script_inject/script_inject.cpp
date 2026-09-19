#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

static const Ed9Api* g_api = nullptr;
static uintptr_t g_base = 0;

typedef uint64_t(__fastcall* LoadScript_t)(void* mgr, const char* name);
static LoadScript_t o_LoadScript = nullptr;

static std::map<std::string, std::vector<std::string>> g_inject_map;
static std::map<std::string, std::vector<std::string>> g_mon_map;
static std::string g_active_mon_table;
static std::map<std::string, std::vector<std::string>> g_monevt_map;
static std::string g_active_mon_event;
static bool g_mon_event_spawned = false;
static bool g_injecting = false;

typedef void(__fastcall* MonsLoad_t)(uintptr_t holder, const char* name);
typedef uintptr_t(__fastcall* AreaProc_t)(uintptr_t areaMgr);
typedef void(__fastcall* AreaRefresh_t)(uintptr_t areaMgr, unsigned flags, int filter, const char* name);
typedef void(__fastcall* SpawnEvent_t)(void* mgr, const char* name, void* p3, unsigned p4);
typedef void(__fastcall* AreaRegister_t)(uintptr_t area, uintptr_t entry);
static AreaProc_t o_AreaProc = nullptr;
static bool g_mon_bound = false;

static int register_area_monsters(uintptr_t areaMgr, uintptr_t table) {
    if (!areaMgr || !table) return 0;
    uintptr_t areaList = *(uintptr_t*)(areaMgr + 0x1488);
    uint32_t  areaCount = *(uint32_t*)(areaMgr + 0x1490);
    if (!areaList || areaCount == 0) return 0;

    uint32_t rowCount = 0; uint64_t ptrForm = *(uint64_t*)(table + 0x50);
    uintptr_t rowPtrs = 0, dataBase = 0; uint32_t coloff = 0; int stride = 0;
    if (ptrForm != 0) {
        rowCount = (uint32_t)ptrForm;
        rowPtrs = *(uintptr_t*)(table + 0x48);
    } else {
        uintptr_t colbase = *(uintptr_t*)(table + 0x20);
        uint32_t  sel = *(uint32_t*)(table + 0x28);
        if (!colbase) return 0;
        uintptr_t col = colbase + (uintptr_t)sel * 0x50;
        rowCount = *(uint32_t*)(col + 0x4c);
        stride   = *(int*)(col + 0x48);
        coloff   = *(uint32_t*)(col + 0x44);
        dataBase = *(uintptr_t*)(table + 0x10);
        if (!dataBase) return 0;
    }
    AreaRegister_t reg = (AreaRegister_t)(g_base + 0x252680);
    int registered = 0;
    for (uint32_t r = 0; r < rowCount && r < 256; ++r) {
        uintptr_t entry = ptrForm ? *(uintptr_t*)(rowPtrs + (uintptr_t)r * 8)
                                  : (dataBase + coloff + (uintptr_t)r * (uint32_t)stride);
        if (!entry) continue;
        const char* mname = *(const char**)(entry + 0x50);
        if (!mname) continue;
        for (uint32_t a = 0; a < areaCount; ++a) {
            uintptr_t area = *(uintptr_t*)(areaList + (uintptr_t)a * 0x20);
            if (!area) continue;
            const char* aname = (const char*)(area + 0x10);
            if (strcmp(mname, aname) == 0) {
                uintptr_t mlist = *(uintptr_t*)(area + 0x210);
                uint32_t  mcount = *(uint32_t*)(area + 0x218);
                bool present = false;
                for (uint32_t k = 0; mlist && k < mcount; ++k)
                    if (*(uintptr_t*)(mlist + (uintptr_t)k * 0x18) == entry) { present = true; break; }
                if (!present) { reg(area, entry); ++registered; }
                break;
            }
        }
    }
    return registered;
}

static uintptr_t __fastcall hk_AreaProc(uintptr_t areaMgr) {
    uintptr_t areaCount = areaMgr ? *(uintptr_t*)(areaMgr + 0x1490) : 0;

    if (!g_active_mon_event.empty()) {
        if (areaCount != 0) {
            uintptr_t mgr = *(uintptr_t*)(g_base + 0xadd7a8);
            uintptr_t holder = mgr ? *(uintptr_t*)(mgr + 0x48) : 0;
            if (!g_mon_event_spawned) {
                uintptr_t scrmgr = *(uintptr_t*)(g_base + 0xae0d38);
                if (scrmgr) {
                    ((SpawnEvent_t)(g_base + 0x24afa0))((void*)scrmgr, g_active_mon_event.c_str(), nullptr, 0);
                    g_mon_event_spawned = true;
                }
            }
            if (holder && *(uintptr_t*)(holder + 8) == 0) return 0;
            if (holder && *(uintptr_t*)(holder + 8) != 0) {
                uintptr_t encmgr = *(uintptr_t*)(g_base + 0xae0d38);
                uintptr_t encsub = encmgr ? *(uintptr_t*)(encmgr + 0x738) : 0;
                uint32_t mode = encsub ? *(uint32_t*)(encsub + 0x1f4540) : 0xFFFFFFFFu;
                if (mode == 0) {
                    uintptr_t table = *(uintptr_t*)(holder + 8);
                    register_area_monsters(areaMgr, table);
                }
            }
        }
        return o_AreaProc(areaMgr);
    }

    if (areaCount != 0 && !g_active_mon_table.empty()) {
        uintptr_t mgr = *(uintptr_t*)(g_base + 0xadd7a8);
        uintptr_t holder = mgr ? *(uintptr_t*)(mgr + 0x48) : 0;
        if (holder && *(uintptr_t*)(holder + 8) == 0) {
            ((MonsLoad_t)(g_base + 0x1fe5b0))(holder, g_active_mon_table.c_str());
            if (*(uintptr_t*)(holder + 8) == 0) return 0;
        }
        if (holder && *(uintptr_t*)(holder + 8) != 0 && !g_mon_bound) {
            uintptr_t table = *(uintptr_t*)(holder + 8);
            register_area_monsters(areaMgr, table);
            g_mon_bound = true;
        }
    }
    return o_AreaProc(areaMgr);
}

static const uint32_t LIST_CAP = 64;
static std::set<void*> g_enlarged;

static uint8_t* find_unique(uint8_t* base, size_t len, const uint8_t* pat, size_t plen) {
    uint8_t* hit = nullptr;
    for (size_t i = 0; i + plen <= len; ++i) {
        if (memcmp(base + i, pat, plen) != 0) continue;
        if (hit != nullptr) return nullptr;
        hit = base + i;
    }
    return hit;
}

typedef void* (__fastcall* Alloc_t)(void* heap, size_t n);
static Alloc_t g_engine_alloc = nullptr;
static void*   g_memmgr = nullptr;
static void* eng_alloc(size_t n) {
    if (g_engine_alloc && g_memmgr) return g_engine_alloc((char*)g_memmgr + 8, n);
    return malloc(n);
}

static void ensure_big_list(void* mgr) {
    if (!mgr || g_enlarged.count(mgr)) return;
    void** pArr = (void**)((char*)mgr + 0x160);
    uint32_t* pCount = (uint32_t*)((char*)mgr + 0x168);
    void* oldArr = *pArr;
    if (!oldArr) return;
    void* newArr = eng_alloc((size_t)LIST_CAP * 8);
    if (!newArr) return;
    memset(newArr, 0, (size_t)LIST_CAP * 8);
    uint32_t cnt = *pCount; if (cnt > 8) cnt = 8;
    memcpy(newArr, oldArr, (size_t)cnt * 8);
    *pArr = newArr;
    g_enlarged.insert(mgr);
}

static void load_list(const wchar_t* fname, std::map<std::string, std::vector<std::string>>& m) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* sl = wcsrchr(exe, L'\\'); if (sl) *sl = 0;
    std::wstring p = std::wstring(exe) + L"\\ED9Loader\\cache\\" + fname;
    std::ifstream f(p, std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t i = 0; while (i < line.size() && line[i] != '\t' && line[i] != ' ') ++i;
        if (i == 0 || i >= line.size()) continue;
        std::string key = line.substr(0, i);
        size_t j = i; while (j < line.size() && (line[j] == '\t' || line[j] == ' ')) ++j;
        std::string val = line.substr(j);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        if (key.empty() || val.empty()) continue;
        m[key].push_back(val);
    }
}
static void load_inject_table() {
    load_list(L"script_inject.list", g_inject_map);
    load_list(L"mon_load.list", g_mon_map);
    load_list(L"mon_event.list", g_monevt_map);
}

static uint64_t __fastcall hk_LoadScript(void* mgr, const char* name) {
    ensure_big_list(mgr);
    uint64_t r = o_LoadScript(mgr, name);
    if (name && !g_injecting) {
        auto it = g_inject_map.find(name);
        if (it != g_inject_map.end()) {
            g_injecting = true;
            for (const std::string& s : it->second) {
                o_LoadScript(mgr, s.c_str());
            }
            g_injecting = false;
        }
        auto mit = g_mon_map.find(name);
        if (mit != g_mon_map.end() && !mit->second.empty()) {
            g_active_mon_table = mit->second[0];
            g_mon_bound = false;
        }
        auto eit = g_monevt_map.find(name);
        if (eit != g_monevt_map.end() && !eit->second.empty()) {
            g_active_mon_event = eit->second[0];
            g_mon_event_spawned = false;
            g_active_mon_table.clear();
        }
    }
    return r;
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) return;
    g_api = api;
    if (api->abi_version < 7 || api->install_hook == nullptr ||
        api->get_module_base == nullptr || api->resolve_symbol == nullptr) {
        api->log("[ScriptInject] 需要 ED9Loader ABI v7(符号表),已禁用");
        return;
    }
    load_inject_table();
    uintptr_t base = (uintptr_t)api->get_module_base();
    g_base = base;

    void* loadScript = api->resolve_symbol("LoadScript");
    if (loadScript == nullptr) {
        api->log("[ScriptInject] LoadScript 未解析(游戏更新过?请重新锚定符号表),已禁用");
        return;
    }
    api->install_hook(loadScript, (void*)hk_LoadScript, (void**)&o_LoadScript);

    g_engine_alloc = (Alloc_t)api->resolve_symbol("engine_alloc");
    void* mm_vt = (api->find_vtable != nullptr) ? api->find_vtable("MemoryManager@fdk") : nullptr;
    g_memmgr = (mm_vt != nullptr && api->find_instance != nullptr) ? api->find_instance(mm_vt) : nullptr;

    if (api->abi_version >= 3 && api->safe_write != nullptr) {
        static const uint8_t kCap1[] = { 0x83, 0xB9, 0x68, 0x01, 0x00, 0x00, 0x08 };
        static const uint8_t kCap2[] = { 0x83, 0xF8, 0x08 };
        uint8_t* fn = (uint8_t*)loadScript;
        uint8_t* c1 = find_unique(fn, 0x100, kCap1, sizeof(kCap1));
        uint8_t* c2 = find_unique(fn, 0x100, kCap2, sizeof(kCap2));
        uint8_t cap = (uint8_t)LIST_CAP;
        if (c1 != nullptr && c2 != nullptr) {
            api->safe_write(c1 + 6, &cap, 1);
            api->safe_write(c2 + 2, &cap, 1);
        } else {
            api->log("[ScriptInject] 警告:没定位到 8 槽上限的立即数,脚本列表仍限 8 个");
        }
    }

    void* areaProc = api->resolve_symbol("AreaProc");
    const bool monOk = (areaProc != nullptr) && ((uintptr_t)areaProc == base + 0xab930) &&
                       (api->resolve_symbol("AreaRegister") != nullptr) &&
                       (api->resolve_symbol("SpawnEvent") != nullptr) &&
                       (api->resolve_symbol("MonsLoad") != nullptr);
    if (!g_mon_map.empty() || !g_monevt_map.empty()) {
        if (monOk) api->install_hook(areaProc, (void*)hk_AreaProc, (void**)&o_AreaProc);
        else api->log("[ScriptInject] 怪物/事件链在本版本不可用(缺符号或 .data 常量不适配),仅脚本注入生效");
    }
    api->log("[ScriptInject] 已按当前游戏版本自适应装载");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
