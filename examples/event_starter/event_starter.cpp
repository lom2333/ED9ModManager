#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static const Ed9Api* g_api = nullptr;

static unsigned g_rva_spawn   = 0x24afa0;
static unsigned g_rva_scrmgr  = 0xae0d38;
static unsigned g_rva_frame   = 0x23dd10;

typedef void (*fn_spawn_t)(void* mgr, const char* name, void* p3, unsigned p4);
typedef void (*fn_frame_t)(void* param_1, void* param_2);
typedef void (*fn_setque_t)(void* que, int event_id, char flag);

static unsigned g_rva_setque = 0x214e40;
static unsigned g_rva_encsub = 0x738;
static unsigned g_rva_evtque = 0x1f4538;

static fn_spawn_t  g_spawn      = nullptr;
static fn_frame_t  g_frame_orig = nullptr;
static fn_setque_t g_setque     = nullptr;

static volatile LONG g_pending = 0;
static char          g_pending_name[64] = {};
static volatile LONG g_pending_evtq = 0;
static volatile LONG g_pending_evtq_id = 0;

static uintptr_t Base() { return reinterpret_cast<uintptr_t>(g_api->get_module_base()); }

template <typename T>
static bool RD(uintptr_t addr, T* out) {
    if (g_api == nullptr || g_api->safe_read == nullptr) return false;
    return g_api->safe_read(reinterpret_cast<const void*>(addr), out, sizeof(T)) != 0;
}

static void DumpLoadedScripts() {
    if (g_api->console_print == nullptr) return;
    char buf[256] = {};
    uintptr_t mgr = 0;
    if (!RD<uintptr_t>(Base() + g_rva_scrmgr, &mgr) || mgr == 0) {
        g_api->console_print("[event] script manager null (not in a scene yet?)\n");
        return;
    }
    uintptr_t list = 0; uint32_t count = 0;
    RD<uintptr_t>(mgr + 0x160, &list);
    RD<uint32_t>(mgr + 0x168, &count);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[event] loaded-scripts: mgr=0x%llx list=0x%llx count=%u\n",
                (unsigned long long)mgr, (unsigned long long)list, count);
    g_api->console_print(buf);
    if (list == 0) return;
    if (count > 128) count = 128;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t scr = 0;
        if (!RD<uintptr_t>(list + (uintptr_t)i * 8, &scr) || scr == 0) continue;
        char name[64] = {};
        g_api->safe_read(reinterpret_cast<const void*>(scr + 0xc), name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        for (int k = 0; k < (int)sizeof(name); ++k) {
            unsigned char c = (unsigned char)name[k];
            if (c == 0) break;
            if (c < 0x20 || c > 0x7e) { name[k] = '\0'; break; }
        }
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  [%u] scr=0x%llx  name=\"%s\"\n",
                    i, (unsigned long long)scr, name);
        g_api->console_print(buf);
    }
}

static void RunPendingOnMainThread() {
    if (InterlockedCompareExchange(&g_pending, 0, 1) != 1) return;
    char name[64];
    strncpy_s(name, sizeof(name), g_pending_name, _TRUNCATE);

    uintptr_t mgr = 0;
    if (g_api->safe_read == nullptr ||
        g_api->safe_read(reinterpret_cast<const void*>(Base() + g_rva_scrmgr), &mgr, sizeof(mgr)) == 0 ||
        mgr == 0) {
        return;
    }
    if (g_spawn == nullptr) return;

    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "EventStarter: spawning event '%s' via FUN_140247550(mgr=0x%llx)",
                name, (unsigned long long)mgr);
    if (g_api->console_print != nullptr) { g_api->console_print(msg); g_api->console_print("\n"); }

    g_spawn(reinterpret_cast<void*>(mgr), name, nullptr, 0);
}

static void RunPendingEvtqOnMainThread() {
    if (InterlockedCompareExchange(&g_pending_evtq, 0, 1) != 1) return;
    int id = (int)g_pending_evtq_id;
    uintptr_t encmgr = 0, subobj = 0, que = 0;
    if (!RD<uintptr_t>(Base() + g_rva_scrmgr, &encmgr) || encmgr == 0 ||
        !RD<uintptr_t>(encmgr + g_rva_encsub, &subobj) || subobj == 0 ||
        !RD<uintptr_t>(subobj + g_rva_evtque, &que) || que == 0) {
        return;
    }
    if (g_setque == nullptr) return;
    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "EventStarter: SetEventQue(id=%d) que=0x%llx (proper trigger, loads scene resources)",
                id, (unsigned long long)que);
    if (g_api->console_print != nullptr) { g_api->console_print(msg); g_api->console_print("\n"); }
    g_setque(reinterpret_cast<void*>(que), id, 1);
}

static void __cdecl Frame_Detour(void* param_1, void* param_2) {
    if (g_frame_orig != nullptr) g_frame_orig(param_1, param_2);
    if (g_pending != 0) RunPendingOnMainThread();
    if (g_pending_evtq != 0) RunPendingEvtqOnMainThread();
}

static void Cmd_Event(int argc, const char** argv) {
    if (g_api == nullptr || g_api->console_print == nullptr) return;
    if (argc < 2 || argv[1] == nullptr || argv[1][0] == '\0') {
        g_api->console_print("usage: -event <function_name>   (spawn by name, FUN_140247550)\n"
                             "       -event list              (dump loaded scripts)\n"
                             "       -event q <id>            (SetEventQue by id, proper trigger)\n");
        return;
    }
    if (strcmp(argv[1], "list") == 0) { DumpLoadedScripts(); return; }
    if (strcmp(argv[1], "q") == 0) {
        if (argc < 3 || argv[2] == nullptr) { g_api->console_print("usage: -event q <event_id>\n"); return; }
        g_pending_evtq_id = (LONG)atoi(argv[2]);
        InterlockedExchange(&g_pending_evtq, 1);
        char m[128];
        _snprintf_s(m, sizeof(m), _TRUNCATE, "EventStarter: queued SetEventQue(id=%ld) for next frame\n",
                    (long)g_pending_evtq_id);
        g_api->console_print(m);
        return;
    }
    strncpy_s(g_pending_name, sizeof(g_pending_name), argv[1], _TRUNCATE);
    InterlockedExchange(&g_pending, 1);
    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "EventStarter: queued event '%s' (will start on next main-thread frame)\n", g_pending_name);
    g_api->console_print(msg);
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) return;
    if (api->abi_version < 6) { return; }
    g_api = api;

    if (api->cfg_get_int != nullptr) {
        g_rva_spawn  = (unsigned)api->cfg_get_int("EventStarter", "rva_spawn",  (int)g_rva_spawn);
        g_rva_scrmgr = (unsigned)api->cfg_get_int("EventStarter", "rva_scrmgr", (int)g_rva_scrmgr);
        g_rva_frame  = (unsigned)api->cfg_get_int("EventStarter", "rva_frame",  (int)g_rva_frame);
    }

    g_spawn  = reinterpret_cast<fn_spawn_t>(Base() + g_rva_spawn);
    g_setque = reinterpret_cast<fn_setque_t>(Base() + g_rva_setque);

    if (api->install_hook != nullptr) {
        void* target = reinterpret_cast<void*>(Base() + g_rva_frame);
        api->install_hook(target, reinterpret_cast<void*>(&Frame_Detour),
                          reinterpret_cast<void**>(&g_frame_orig));
    }

    if (api->register_command != nullptr) {
        api->register_command("-event", "start a script function as a real event (usage: -event <name>)", Cmd_Event);
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
