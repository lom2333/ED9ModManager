// ED9Loader 观测插件:抓引擎对 scene 文件的查找,确认实机读 .json 还是 .bin、确切查找键。
// hook 两个点(RVA, ImageBase 0x140000000):
//   FileStream::Open by name      = 0x5264e0  (原始请求名)
//   PackFileManager::FindFile     = 0x48b7b0  (pac 查找键)
// 只 log 含 "_sys"/"_navi"/"mp4000" 的名字并去重,避免刷屏。结果进 ED9Loader/loader.log。
#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static const Ed9Api* g_api = nullptr;
static std::set<std::string> g_seen;

typedef uint64_t(__fastcall* FindFile_t)(void*, const char*, uint64_t*, uint64_t*);
typedef void* (__fastcall* Open_t)(void*, const char*, unsigned, unsigned, unsigned short);
static FindFile_t o_FindFile = nullptr;
static Open_t o_Open = nullptr;

static bool interesting(const char* n) {
    return n && (strstr(n, "_sys") || strstr(n, "_navi") || strstr(n, "mp4000"));
}

static void note(const char* tag, const char* name) {
    if (!interesting(name)) return;
    std::string key = std::string(tag) + "|" + name;
    if (g_seen.count(key)) return;
    g_seen.insert(key);
    char buf[320];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[scene_probe] %s: \"%s\"", tag, name);
    if (g_api && g_api->log) g_api->log(buf);
}

static uint64_t __fastcall hk_FindFile(void* mgr, const char* name, uint64_t* off, uint64_t* sz) {
    note("FindFile", name);
    return o_FindFile(mgr, name, off, sz);
}
static void* __fastcall hk_Open(void* self, const char* name, unsigned a, unsigned b, unsigned short c) {
    note("Open", name);
    return o_Open(self, name, a, b, c);
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) return;
    g_api = api;
    if (api->abi_version < 2 || api->install_hook == nullptr || api->get_module_base == nullptr) {
        api->log("scene_probe: needs ABI>=2 + install_hook, skipped");
        return;
    }
    uintptr_t base = (uintptr_t)api->get_module_base();
    int r1 = api->install_hook((void*)(base + 0x48b7b0), (void*)hk_FindFile, (void**)&o_FindFile);
    int r2 = api->install_hook((void*)(base + 0x5264e0), (void*)hk_Open, (void**)&o_Open);
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "scene_probe: loaded. hook FindFile(0x48b7b0)=%d Open(0x5264e0)=%d base=0x%p", r1, r2, (void*)base);
    api->log(buf);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
