#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static const Ed9Api* g_api = nullptr;

typedef void*   (__fastcall* LoadDDS_t)(void*, void*);

static LoadDDS_t o_LoadDDS = nullptr;
static int g_logged = 0;
static uintptr_t g_base = 0;

static void* __fastcall hk_LoadDDS(void* self, void* stream) {
    uint64_t f[8] = {0};
    bool got = (g_api && g_api->safe_read && stream &&
                g_api->safe_read(stream, f, sizeof(f)) != 0);
    uint64_t inner_size = 0;
    if (got && f[5]) {
        uint64_t g[4] = {0};
        if (g_api->safe_read((void*)f[5], g, sizeof(g)))
            inner_size = g[2];
    }

    void* r = o_LoadDDS(self, stream);

    if (g_api && g_api->log && g_logged < 20000) {
        ++g_logged;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[texprobe] %s #%-4d vt=+0x%-7llX mode=%-2u inner=%s p30=%llu p38=%llu innerSize=%llu",
            r ? "OK  " : "FAIL", g_logged,
            (unsigned long long)(f[0] ? f[0] - (uint64_t)g_base : 0),
            (unsigned)(f[1] & 0xFFFFFFFFu),
            f[5] ? "有" : "无",
            (unsigned long long)f[6], (unsigned long long)f[7],
            (unsigned long long)inner_size);
        g_api->log(buf);
    }
    return r;
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    g_api = api;
    if (!api || !api->log) return;
    if (api->abi_version < 7 || !api->find_vtable || !api->safe_read) {
        api->log("[texprobe] 框架 ABI < 7 或缺 find_vtable/safe_read,自禁用");
        return;
    }

    void* vt = api->find_vtable("RenderDeviceDX11");
    if (!vt) { api->log("[texprobe] 找不到 fdk::RenderDeviceDX11 的 vtable,自禁用"); return; }

    void* fn = nullptr;
    if (!api->safe_read((char*)vt + 9 * 8, &fn, sizeof(fn)) || !fn) {
        api->log("[texprobe] vtable[9] 读不出来,自禁用"); return;
    }

    char buf[200];
    void* base = api->get_module_base();
    g_base = (uintptr_t)base;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[texprobe] RenderDeviceDX11 vtable=+0x%llX  LoadDDS=+0x%llX",
                (unsigned long long)((char*)vt - (char*)base),
                (unsigned long long)((char*)fn - (char*)base));
    api->log(buf);

    if (api->install_hook(fn, (void*)&hk_LoadDDS, (void**)&o_LoadDDS) != 0) {
        api->log("[texprobe] hook LoadDDS 失败"); return;
    }
    api->log("[texprobe] 已挂 LoadDDS,等待贴图加载");
}
