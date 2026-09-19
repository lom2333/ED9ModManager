#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static const Ed9Api* g_api = nullptr;
static std::set<std::string> g_seen;
static int g_audio_logged = 0;
static int g_sanity_logged = 0;

static bool is_audio(const char* n) {
    return n && (strstr(n, "voice") || strstr(n, "wav") || strstr(n, ".at9") ||
                 strstr(n, ".opus") || strstr(n, ".snd") || strstr(n, "bgm") ||
                 strstr(n, "/se/") || strstr(n, "sound"));
}

typedef uint64_t(__fastcall* FindFile_t)(void*, const char*, uint64_t*, uint64_t*);
typedef void* (__fastcall* Open_t)(void*, const char*, unsigned, unsigned, unsigned short);
static FindFile_t o_FindFile = nullptr;
static Open_t o_Open = nullptr;

static void note(const char* tag, const char* name) {
    if (!name || !g_api || !g_api->log) return;
    if (is_audio(name)) {
        if (g_audio_logged >= 200) return;
        std::string key = std::string("A|") + tag + "|" + name;
        if (g_seen.count(key)) return;
        g_seen.insert(key);
        ++g_audio_logged;
        char buf[320];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[audio_probe] %s: \"%s\"", tag, name);
        g_api->log(buf);
    } else if (g_sanity_logged < 8) {
        ++g_sanity_logged;
        char buf[320];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[audio_probe][sanity] %s: \"%s\"", tag, name);
        g_api->log(buf);
    }
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
    int r1 = api->install_hook((void*)(base + 0x490d60), (void*)hk_FindFile, (void**)&o_FindFile);
    int r2 = api->install_hook((void*)(base + 0x52c240), (void*)hk_Open, (void**)&o_Open);
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[audio_probe] loaded. hook FindFile(0x490d60)=%d Open(0x52c240)=%d base=0x%p", r1, r2, (void*)base);
    api->log(buf);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
