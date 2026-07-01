// ED9Loader 插件:EventStarter —— 把脚本函数当【真正的事件线程】启动。
//
// 逆向结论:
//   引擎的"交互→运行事件"分发器 FUN_1402247a0 是这样启动一个事件的:
//       FUN_140247550(DAT_140ad4e98, <事件函数名C串>, 0, 0)
//   FUN_140247550 = 在【已加载脚本列表】(mgr+0x160,含 ScriptInject 注入的脚本)里按名查函数
//       (FUN_1404b20e0,首字符做哈希种子+strcmp),找到后经 *(DAT_140ad4ee8+0x11688) 的
//       vtable+8 方法生成一条脚本线程 —— 这条线程就是被引擎当成【真正的事件】驱动的。
//   一个自包含的事件函数(EVENT_BEGIN→event_entry_chr 队友+怪→EVENT_FINALIZE_SEAMLESS→
//   EVENT_END_BTL)被这样启动后,引擎会按真事件跑完整流程(含战斗过渡),这正是
//   "对话触发隐藏事件怪战斗"所缺的关键一步——把逻辑【当真 event 跑】。
//
// 线程安全:FUN_140247550 会改游戏状态,必须在【主线程】调用。控制台命令跑在控制台线程,
//   所以命令只设 pending;由 hook 的每帧主线程函数 FUN_1401191b0(字段更新,调遭遇处理器)
//   在下一帧执行,执行完即清 pending。
//
// 用法(在 mp4000 等图里走动后,控制台输入):
//   -event EV_04_22_00       启动原版城镇事件战斗(验证启动器机制)
//   -event <自定义事件函数名> 启动我们注入的自定义事件
#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static const Ed9Api* g_api = nullptr;

// 可在 ED9Loader/config/EventStarter.ini 调(十进制)。默认来自逆向(ImageBase 0x140000000)。
static unsigned g_rva_spawn   = 0x247550;  // FUN_140247550(mgr, name, p3, p4) 按名启动事件线程
static unsigned g_rva_scrmgr  = 0xad4e98;  // DAT_140ad4e98  脚本/事件管理器(=spawn 的 param_1)
static unsigned g_rva_frame   = 0x23a2c0;  // FUN_14023a2c0  字段管理器每帧主更新(主线程 hook 点)
                                           //   (param_1=DAT_140ad4e98;mode0 正常走动时也跑,遍历交互/角色/移动)

typedef void (*fn_spawn_t)(void* mgr, const char* name, void* p3, unsigned p4);
typedef void (*fn_frame_t)(void* param_1, void* param_2);
// EventQue::SetEventQue(%d) —— 按【事件id】入队=正经事件触发(队列处理器按 t_evtable 加载 scene 资源再跑事件)
typedef void (*fn_setque_t)(void* que, int event_id, char flag);

static unsigned g_rva_setque = 0x2113e0;  // FUN_1402113e0  EventQue::SetEventQue(id)
static unsigned g_rva_encsub = 0x738;     // DAT_140ad4e98 -> 遭遇子对象
static unsigned g_rva_evtque = 0x1f4538;  // 遭遇子对象 -> 事件队列对象指针

static fn_spawn_t  g_spawn      = nullptr;
static fn_frame_t  g_frame_orig = nullptr;
static fn_setque_t g_setque     = nullptr;

// pending 请求(控制台线程写、主线程读)
static volatile LONG g_pending = 0;
static char          g_pending_name[64] = {};
static volatile LONG g_pending_evtq = 0;  // 按 id 入队事件
static volatile LONG g_pending_evtq_id = 0;

static uintptr_t Base() { return reinterpret_cast<uintptr_t>(g_api->get_module_base()); }

template <typename T>
static bool RD(uintptr_t addr, T* out) {
    if (g_api == nullptr || g_api->safe_read == nullptr) return false;
    return g_api->safe_read(reinterpret_cast<const void*>(addr), out, sizeof(T)) != 0;
}

// dump 已加载脚本列表(mgr+0x160 数组 / +0x168 计数;脚本对象+0xc=名)。只读,可在控制台线程跑。
// 用来确认目标事件函数所在脚本(如 mp4000_ev / 注入的 NpcTalk)确实在可搜索列表里。
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

// 在主线程执行一次待处理的事件启动。
static void RunPendingOnMainThread() {
    if (InterlockedCompareExchange(&g_pending, 0, 1) != 1) return; // 没有 pending
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

    // 复刻 FUN_1402247a0 的调用:FUN_140247550(DAT_140ad4e98, name, 0, 0)
    g_spawn(reinterpret_cast<void*>(mgr), name, nullptr, 0);
}

// 主线程:按事件 id 入队(走正经事件队列=会按 t_evtable 加载 scene 资源)。
// 队列对象 = *(*(DAT_140ad4e98+0x738)+0x1f4538)。FUN_1402113e0(que, id, 1)。
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

// 每帧主线程 hook:先让引擎跑完本帧字段更新,再执行待处理事件启动。
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

    // 安装每帧主线程 hook(延迟执行用)
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
