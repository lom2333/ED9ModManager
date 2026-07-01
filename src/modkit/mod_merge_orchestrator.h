// modkit: 启动编排——扫 <游戏>\Mod\<mod>\{scene_patch,tbl_patch,script_inject}.json → 合并 → 写 cache\merged。
// scene: 读 pac "scene/<target>.json" → 写 cache "scene/<target>.json"。
// tbl:  读 pac "table_sc/t_lookpoint.tbl" → 写 cache "table/t_lookpoint.tbl"(Open 层名,供 SceneRedirect)。
// script_inject: 复制 Mod\<mod>\<script>.dat → cache "script/scena/<script>.dat";生成 cache\..\script_inject.list
//   (每行 <map>\t<script>),供 ScriptInject 插件运行时读表通用注入。
#pragma once
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
namespace orchestrator {

struct Paths {
    std::wstring modsDir;      // 扫 <dir>\<mod>\{scene_patch,tbl_patch,script_inject}.json + tbl\*.json(= 游戏根\Mod)
    std::wstring pacSteamDir;  // scene.pac / table_sc.pac 所在
    std::wstring cacheDir;     // 输出根(= ED9Loader\cache\merged)
    std::wstring schemasDir;   // tbl schema 目录(= ED9Loader\schemas;通用 tbl 用)
};

struct RunResult {
    int merged = 0, skipped = 0, failed = 0, mods = 0, injected = 0, tbls = 0, conflicts = 0, assets = 0;
    std::string log;          // 多行简报(写 loader.log)
    // 详细机器可读报告写 cache\merge_report.json(GUI / mod 管理器读取);log 同步打 CONFLICT 行。
};

// 一个 mod 在加载清单里的状态(顺序 = 套用顺序,靠后者覆盖靠前者)。
// disabled:被单独关闭的组件相对路径(正斜杠,相对 mod 目录),如 "tbl/t_skill.json"、"tbl_patch.json"。
//   仅当 mod 本身 enabled 时才有意义;合并时这些组件被跳过(不参与套用,不影响其它组件)。
struct ModInfo { std::string name; bool enabled = true; std::vector<std::string> disabled; };

// 由游戏目录推导默认路径(Mod / pac\steam / ED9Loader\cache\merged / ED9Loader\schemas)
Paths FromGameDir(const std::wstring& gameDir);

// 读 Mod\mods.json 并对照现存文件夹补全/清理,返回「按加载顺序」列表(不写盘)。供 GUI 读取。
std::vector<ModInfo> ScanMods(const std::wstring& modsDir);
// 写 Mod\mods.json。供 GUI 保存启停/排序。
bool SaveMods(const std::wstring& modsDir, const std::vector<ModInfo>& mods);

// 执行编排。force=true 忽略指纹强制重合并。
RunResult Run(const Paths& paths, bool force = false);

} // namespace orchestrator
} // namespace modkit
} // namespace ed9loader
