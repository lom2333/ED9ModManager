// ED9Loader 脚本注入插件(配置驱动):让引擎额外加载独立脚本 dat(如 Test_point.dat)。
// 机制(逆向 fieldmap_manager.cpp):FUN_140247460(mgr, name) 按名加载脚本到「已加载列表」(最多8槽)。
//   detour:调原函数加载请求脚本;当加载到某「图主脚本名」(配置里声明的 map,如 "mp4000")时,
//   追加调原函数加载配置声明的脚本名("Test_point")→ 引擎 Open "script/scena/Test_point.dat" → SceneRedirect 提供。
// LookPoint 触发时引擎在已加载列表按名搜函数,从而能找到 Test_point 里的 LP_test_00。
//
// 配置来源(零硬编码):modkit orchestrator 启动时扫 Mod\<mod>\add_dat_ini.json(旧名 script_inject.json),生成注入表
//   <游戏>\ED9Loader\cache\script_inject.list,每行 "<map>\t<script>"。本插件启动读表建 map->scripts 映射。
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

// 配置驱动:图主脚本名 -> 要追加加载的脚本名列表(来自 script_inject.list)
static std::map<std::string, std::vector<std::string>> g_inject_map;
// 图主脚本名 -> 进图时要加载的怪物表名列表(来自 mon_load.list)。
static std::map<std::string, std::vector<std::string>> g_mon_map;
static std::string g_active_mon_table;  // 当前图的怪物表名(进图时由 hk_LoadScript 记下,实际加载在 hk_AreaProc)
// 图主脚本名 -> 进图后要 spawn 的事件函数名列表(来自 mon_event.list)。
// 配了 mon_event 的图:插件不预加载怪物表,改用 FUN_140247550 把该函数当事件线程跑一次,
//   让其内部 Cmd_map_02(=map_load_mons_table)在真正脚本 VM ctx 里做完整①加载表②创建宝箱怪char③绑定挂区。
static std::map<std::string, std::vector<std::string>> g_monevt_map;
static std::string g_active_mon_event;  // 当前图要 spawn 的事件函数名
static bool g_mon_event_spawned = false; // 事件只 spawn 一次(每图)
static bool g_injecting = false;        // 防注入递归

// ---- 怪物表加载(不动主 dat 实现「进图载入 t_mon」)----
// 逆向:map_load_mons_table(name) = TableHolder<MonsterSettingTable>::Load(holder,name)
//   (FUN_1401fab50,RVA 0x1fab50)→ FUN_1404a0f60(表管理器, name, hash=0xd77fe308,内部含 Sleep 等待加载完成)
//   → 存进 holder+8。holder = *(*(base+0xad1908)+0x48)(DAT_140ad1908=field manager 指针,+0x48=MonsterSettingTable holder)。
//   holder+8 正是区处理器 FUN_1400ab4f0 读的怪物表 → 空则进图首帧空指针崩(RVA 0xab5df)。
// ⚠时机:在「主脚本加载 hook」调太早(资源系统未就绪→FUN_1404a0f60 走缓存返回 null,不真加载);
//   故改在区处理器 FUN_1400ab4f0(首帧、field 全跑起来=主 dat Init 之后的时机)里按需加载+守卫。
typedef void(__fastcall* MonsLoad_t)(uintptr_t holder, const char* name);
typedef uintptr_t(__fastcall* AreaProc_t)(uintptr_t areaMgr);
// 区怪刷新(预实例化):FUN_1400aaf20(区管理器DAT_140ad18f8, flags, filter, name)——
//   读已加载怪物表→按名匹配战斗区→建怪挂区(填 area+0x210 怪列表)→调 FUN_140242aa0 绑定。
//   逆向链:btl命令 FUN_14036e900 → 此函数。flags 含 0x10=绑定分支、filter=0xffff=全部、name=NULL=全区。
typedef void(__fastcall* AreaRefresh_t)(uintptr_t areaMgr, unsigned flags, int filter, const char* name);
// 引擎「按名把脚本函数当真事件线程启动」入口(EventStarter 逆向,RVA 0x247550):
//   FUN_140247550(脚本/事件管理器=*(base+0xad4e98), 函数名C串, 0, 0)。在已加载脚本列表(含注入脚本)按名查函数生成事件线程。
typedef void(__fastcall* SpawnEvent_t)(void* mgr, const char* name, void* p3, unsigned p4);
// 直接登记+造宝箱怪(绕过 mode 门控)。逆向:FUN_14024ebe0(区对象, 怪物表条目) —
//   把条目指针 append 进区怪列表 area+0x210(0x18B/entry,+0x218=count/+0x220=cap),条目+0x28≠0 时
//   立刻 FUN_14024f280→FUN_140244270 造 char+绑区。本身不受 encounter mode 门控(mode门控在 FUN_140242aa0
//   的 FUN_14024f5a0 刷新循环里)。这就是 init 态本该做、而 field 态被跳过的那步——插件直接补上。
typedef void(__fastcall* AreaRegister_t)(uintptr_t area, uintptr_t entry);
static AreaProc_t o_AreaProc = nullptr;
static bool g_mon_bound = false;        // 区怪绑定只做一次(每图,mon_load 路径用)

// 遍历怪物表(holder+8)每行 × 战斗区,按名匹配(条目+0x50 战斗区名 == 区对象+0x10 名),
// 仅对「当前无怪的区」调 FUN_14024ebe0 登记+造怪(幂等)。#TBL 行寻址逆向自 FUN_14024f150。返回本次登记数。
static int register_area_monsters(uintptr_t areaMgr, uintptr_t table) {
    if (!areaMgr || !table) return 0;
    uintptr_t areaList = *(uintptr_t*)(areaMgr + 0x1488);
    uint32_t  areaCount = *(uint32_t*)(areaMgr + 0x1490);
    if (!areaList || areaCount == 0) return 0;

    // 枚举怪物表行:*(table+0x50)!=0 → 指针数组形(该值=行数,行=*(*(table+0x48)+i*8));
    //             ==0 → 打包形(列描述@ *(table+0x20)+sel*0x50,行=databuf+coloff+i*stride)。
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
    // 遍历表行 × 战斗区,按名匹配。仅当该区当前无怪(area+0x218==0)才登记 → 幂等,不会重复叠加;
    //   战斗消耗掉怪(区清空)后下帧会自动补登记 = 同次进图可反复战;重进图区是新的(空)也会补。
    AreaRegister_t reg = (AreaRegister_t)(g_base + 0x24ebe0);
    int registered = 0;
    for (uint32_t r = 0; r < rowCount && r < 256; ++r) {
        uintptr_t entry = ptrForm ? *(uintptr_t*)(rowPtrs + (uintptr_t)r * 8)
                                  : (dataBase + coloff + (uintptr_t)r * (uint32_t)stride);
        if (!entry) continue;
        const char* mname = *(const char**)(entry + 0x50);   // 该怪条目的战斗区名
        if (!mname) continue;
        for (uint32_t a = 0; a < areaCount; ++a) {
            uintptr_t area = *(uintptr_t*)(areaList + (uintptr_t)a * 0x20);
            if (!area) continue;
            const char* aname = (const char*)(area + 0x10);
            if (strcmp(mname, aname) == 0) {
                // 该条目是否已在区怪列表(area+0x210,每项0x18B,首8B=条目指针)? 不在才登记。
                //   → 支持同一 battle 名多行=同区多敌人;每条目只登记一次=幂等;战斗清空区后所有条目再补回。
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

// hook 区处理器:有战斗区且怪物表(holder+8)为空时——先 FUN_1401fab50 加载(时机就绪,真 open 表);
//   仍空则跳过本帧返回 0(不崩),下帧重试。表加载好后,调 FUN_1400aaf20 把怪预实例化挂进区(填 monsters)。
static uintptr_t __fastcall hk_AreaProc(uintptr_t areaMgr) {
    uintptr_t areaCount = areaMgr ? *(uintptr_t*)(areaMgr + 0x1490) : 0;   // 战斗区数量(FUN_1400ab4f0: param+0x1490)

    // ---- mon_event 路径:守卫 + 自动 spawn 事件,让事件的 Cmd_map_02 做完整①②③ ----
    // 配了 mon_event 的图不预加载表,交给事件线程(有正确脚本 VM ctx,宝箱怪 char 才造得出)。
    if (!g_active_mon_event.empty()) {
        if (areaCount != 0) {
            uintptr_t mgr = *(uintptr_t*)(g_base + 0xad1908);
            uintptr_t holder = mgr ? *(uintptr_t*)(mgr + 0x48) : 0;
            // 首帧区处理时脚本列表(含注入的 Live.dat)已就绪、field 可交互 → spawn 事件一次。
            if (!g_mon_event_spawned) {
                uintptr_t scrmgr = *(uintptr_t*)(g_base + 0xad4e98);
                if (scrmgr) {
                    ((SpawnEvent_t)(g_base + 0x247550))((void*)scrmgr, g_active_mon_event.c_str(), nullptr, 0);
                    g_mon_event_spawned = true;
                }
            }
            // 守卫:事件的 Cmd_map_02 还没把表加载好(holder+8==0)→跳过本帧,避免 0xab5df 空指针崩;下帧重试。
            if (holder && *(uintptr_t*)(holder + 8) == 0) return 0;
            // 表已由事件可靠加载 → 插件在 field 稳定态(mode==0)持续补登记空区的怪。
            //   幂等(只登记无怪的区):首次进图登记一次;战斗消耗掉怪(区清空)后自动补回=同次进图反复战;
            //   重进图区是新的(空)也自动补=跨进图重复。用 mode==0 门控避免战斗中/过渡期误加。
            if (holder && *(uintptr_t*)(holder + 8) != 0) {
                uintptr_t encmgr = *(uintptr_t*)(g_base + 0xad4e98);
                uintptr_t encsub = encmgr ? *(uintptr_t*)(encmgr + 0x738) : 0;
                uint32_t mode = encsub ? *(uint32_t*)(encsub + 0x1f4540) : 0xFFFFFFFFu;  // 遭遇模式:0=field 稳定态
                if (mode == 0) {
                    uintptr_t table = *(uintptr_t*)(holder + 8);
                    register_area_monsters(areaMgr, table);   // 直调 FUN_14024ebe0 登记+造怪
                }
            }
        }
        return o_AreaProc(areaMgr);
    }

    // ---- mon_load 路径(原有:预加载①+守卫+bind尝试)----
    if (areaCount != 0 && !g_active_mon_table.empty()) {
        uintptr_t mgr = *(uintptr_t*)(g_base + 0xad1908);
        uintptr_t holder = mgr ? *(uintptr_t*)(mgr + 0x48) : 0;
        if (holder && *(uintptr_t*)(holder + 8) == 0) {
            ((MonsLoad_t)(g_base + 0x1fab50))(holder, g_active_mon_table.c_str());   // 时机正确,真加载
            if (*(uintptr_t*)(holder + 8) == 0) return 0;                            // 仍未就绪→跳过本帧,避免 0xab5df
        }
        // 表已加载 → 直接把怪登记进战斗区(绕过 mode 门控)。只做一次。
        if (holder && *(uintptr_t*)(holder + 8) != 0 && !g_mon_bound) {
            uintptr_t table = *(uintptr_t*)(holder + 8);
            register_area_monsters(areaMgr, table);   // 直调 FUN_14024ebe0 登记+造怪
            g_mon_bound = true;
        }
    }
    return o_AreaProc(areaMgr);
}

// ---- 解除「已加载脚本列表 8 槽上限」----
// 逆向:FUN_140247460 里两处 cmp count,8(RVA 0x24747E / 0x247522 的立即数)卡 8;
// 列表数组是堆指针 manager+0x160(非内联),清理只重置 count(+0x168)、从不 realloc。
// 故:① 补丁两个 8→LIST_CAP;② 首次进 hook 把 +0x160 换成 LIST_CAP 槽大数组(换一次永久)。
static const uint32_t LIST_CAP = 64;          // 抬高后的上限(对脚本列表=实际无限)
static const uint32_t RVA_CAP1 = 0x24747E;    // cmp [rcx+0x168],8 的立即数
static const uint32_t RVA_CAP2 = 0x247522;    // cmp eax,8 的立即数
static std::set<void*> g_enlarged;            // 已扩容的 manager

typedef void* (__fastcall* Alloc_t)(void* heap, size_t n);
static void* eng_alloc(size_t n) {            // 引擎分配器(与 SceneRedirect 同;free 兼容)
    void** mmp = (void**)(g_base + 0xad4f10);
    void* mm = mmp ? *mmp : nullptr;
    if (mm) { Alloc_t a = (Alloc_t)(g_base + 0x4a5050); return a((char*)mm + 8, n); }
    return malloc(n);
}

// 把 manager 脚本列表数组(+0x160)换成 LIST_CAP 槽大数组(配合 cap 补丁解除 8 槽上限)。
static void ensure_big_list(void* mgr) {
    if (!mgr || g_enlarged.count(mgr)) return;
    void** pArr = (void**)((char*)mgr + 0x160);
    uint32_t* pCount = (uint32_t*)((char*)mgr + 0x168);
    void* oldArr = *pArr;
    if (!oldArr) return;                       // 引擎还没建数组,等下次进 hook
    void* newArr = eng_alloc((size_t)LIST_CAP * 8);
    if (!newArr) return;
    memset(newArr, 0, (size_t)LIST_CAP * 8);
    uint32_t cnt = *pCount; if (cnt > 8) cnt = 8;   // 原数组最多 8 项
    memcpy(newArr, oldArr, (size_t)cnt * 8);
    *pArr = newArr;
    g_enlarged.insert(mgr);
}

// 读 <exe目录>\ED9Loader\cache\<fname>(orchestrator 生成,每行 "<key>\t<val>"),填进 m。缺失则空。
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
        // 拆 "<key>\t<val>"(也容忍空格分隔)
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
    load_list(L"script_inject.list", g_inject_map);   // map -> 追加脚本名
    load_list(L"mon_load.list", g_mon_map);           // map -> 怪物表名
    load_list(L"mon_event.list", g_monevt_map);       // map -> 进图后 spawn 的事件函数名
}

static uint64_t __fastcall hk_LoadScript(void* mgr, const char* name) {
    ensure_big_list(mgr);  // 先确保列表数组够大(在引擎本次 add 之前)
    uint64_t r = o_LoadScript(mgr, name);
    // 追加注入:加载到配置声明的图主脚本时,额外加载该图的所有 mod 脚本。
    // 每次该图加载都注入(引擎重复加载已在列表的脚本是幂等的),修复离图再回的丢失。
    if (name && !g_injecting) {
        auto it = g_inject_map.find(name);
        if (it != g_inject_map.end()) {
            g_injecting = true;
            for (const std::string& s : it->second) {
                o_LoadScript(mgr, s.c_str());
            }
            g_injecting = false;
        }
        // 进图加载怪物表(不动主 dat):记下该图的 t_mon 表名;实际加载在 hk_AreaProc(时机正确)。
        auto mit = g_mon_map.find(name);
        if (mit != g_mon_map.end() && !mit->second.empty()) {
            g_active_mon_table = mit->second[0];   // 当前图怪物表(一图一表)
            g_mon_bound = false;
        }
        // 进图后 spawn 事件线程:记下事件函数名;spawn 在 hk_AreaProc(首帧、脚本列表就绪)。
        auto eit = g_monevt_map.find(name);
        if (eit != g_monevt_map.end() && !eit->second.empty()) {
            g_active_mon_event = eit->second[0];
            g_mon_event_spawned = false;
            g_active_mon_table.clear();            // mon_event 优先:不走预加载路径,避免双路径干扰
        }
    }
    return r;
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) return;
    g_api = api;
    if (api->abi_version < 2 || api->install_hook == nullptr || api->get_module_base == nullptr) {
        return;
    }
    load_inject_table();
    uintptr_t base = (uintptr_t)api->get_module_base();
    g_base = base;
    api->install_hook((void*)(base + 0x247460), (void*)hk_LoadScript, (void**)&o_LoadScript);
    // 区处理器 hook(FUN_1400ab4f0):mon_load(按需加载表+守卫) 或 mon_event(守卫+spawn事件) 任一有配置就装。
    if (!g_mon_map.empty() || !g_monevt_map.empty())
        api->install_hook((void*)(base + 0xab4f0), (void*)hk_AreaProc, (void**)&o_AreaProc);

    // 解除 8 槽上限:补丁两处 cmp ...,8 的立即数为 LIST_CAP(数组扩容在 hook 里完成)
    if (api->abi_version >= 3 && api->safe_write) {
        uint8_t cap = (uint8_t)LIST_CAP;
        api->safe_write((void*)(base + RVA_CAP1), &cap, 1);
        api->safe_write((void*)(base + RVA_CAP2), &cap, 1);
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
