# ED9ModManager

**空之轨迹 the 1st / the 2nd 的 MOD 管理器 + MOD 加载器**<br>
**Mod Manager & Mod Loader for Trails in the Sky 1st Chapter / 2nd Chapter**

[下载最新版 / Download](https://github.com/lom2333/ED9ModManager/releases/latest) · [English](#english)

给《英雄传说 空之轨迹 the 1st》《空之轨迹 the 2nd》(空轨 1 / 空轨 2,也就是 FC / SC 的重制版;Steam PC 版)用的 MOD 管理器和 MOD 加载器。把 MOD 放进文件夹、勾选、保存,进游戏就生效;多个 MOD 改到同一张表、同一个场景或同一个脚本时会自动合并。

## 安装和使用

1. 到 [Releases](https://github.com/lom2333/ED9ModManager/releases/latest) 下载最新的 `Ed9ModManager-V*.zip`。
2. 解压到游戏目录(`sora_1st.exe` 或 `sora_2nd.exe` 所在的文件夹)。游戏目录里会多出 `xinput1_4.dll`、`ED9Loader\`、`Mod\`。
3. 把 MOD 放进 `Mod\`:文件夹或压缩包(zip / 7z / rar)都行,压缩包不用解压。
4. 运行 `ED9Loader\ED9ModManager.exe`,确认「游戏目录」是游戏根目录,勾选要用的 MOD,点「保存」。
5. 照常启动游戏。

- 同一个包同时支持 the 1st 和 the 2nd,按游戏目录自动识别。
- 多个 MOD 冲突时,列表里靠下的优先;管理器会标出冲突,用 ▲▼ 调顺序。
- MOD 走重定向加载,不覆盖游戏原文件。不想用了,删掉游戏目录里的 `xinput1_4.dll` 即可。
- 界面语言:简体中文、繁體中文、English、日本語、한국어、Deutsch、Français、Español。
- 管理器里可以检查并安装更新。

## 常见问题

**空轨 2(空之轨迹 the 2nd)有 MOD 管理器吗?**<br>
有,就是这个。the 1st 和 the 2nd 用同一个包。

**支持哪个版本的游戏?**<br>
Steam 的 PC 版(Windows 64 位)。主机版不支持。

**游戏更新之后还能用吗?**<br>
加载器带自我修复,游戏更新后会自己重新定位,一般不用等新版;还是不行的话,在管理器里检查更新,或到 Releases 下载新版。

**想自己做 MOD,从哪看起?**<br>
发布包里的 `Mod\2ndMod示例` 是一个可以照着改的例子;管理器自带 TBL / DAT 和 JSON 互转、DDS 加解密、PAC 浏览。

## English

**ED9ModManager** is a mod manager and mod loader for the PC (Steam) versions of *The Legend of Heroes: Trails in the Sky 1st Chapter* and *Trails in the Sky 2nd Chapter* (空の軌跡 the 1st / the 2nd). Drop mods into a folder, tick them, click Save, and play. When several mods edit the same table, scene or script, their changes are merged automatically.

**Install**

1. Download the latest `Ed9ModManager-V*.zip` from [Releases](https://github.com/lom2333/ED9ModManager/releases/latest).
2. Extract it into the game folder (the one containing `sora_1st.exe` or `sora_2nd.exe`). You will get `xinput1_4.dll`, `ED9Loader\` and `Mod\` there.
3. Put mods into `Mod\`, as folders or as archives (zip / 7z / rar, no need to extract).
4. Run `ED9Loader\ED9ModManager.exe`, make sure the game folder is set, tick the mods you want, and click **Save**.
5. Start the game as usual.

- One package works for both 1st Chapter and 2nd Chapter; the game is detected from the folder.
- On conflicts, mods lower in the list win; the manager marks conflicts and lets you reorder them.
- Mods are loaded through redirection and never overwrite the original game files. To stop using it, delete `xinput1_4.dll` from the game folder.
- Interface languages: English, 简体中文, 繁體中文, 日本語, 한국어, Deutsch, Français, Español.
- The manager can check for and install updates.

**FAQ**

*Is there a mod manager for Trails in the Sky 2nd Chapter?*<br>
Yes, this one. The same package also supports 1st Chapter.

*Which versions of the game are supported?*<br>
The Steam PC version (Windows x64). Console versions are not supported.

*Does it keep working after a game update?*<br>
The loader re-locates what it needs after an update, so it usually keeps working. If it does not, update from inside the manager or download the latest release.

---

## 开发者 / For developers

*A modding toolchain for the ED9/fdk engine builds of «The Legend of Heroes: Trails in the Sky the 1st» and «the 2nd» (空の軌跡 the 1st / the 2nd).*

面向空之轨迹 the 1st / the 2nd（ED9/fdk 引擎，x64）的 Mod 工具链：一个原生插件加载框架 + 一个图形化 Mod 管理/合并器 + 若干示例插件。全部为 C++20/CMake，Windows 平台。

---

## 组成

| 组件 | 产物 | 说明 |
|---|---|---|
| **ED9Loader**（注入框架） | `xinput1_4.dll` | 类 BepInEx 的原生插件加载器：劫持 `xinput1_4` 入口，向插件暴露 `Ed9Api`（MinHook inline hook、配置 ini、按 RTTI 定位 vtable/实例、安全读写、控制台命令注册、按游戏版本查函数地址）。 |
| **ED9ModManager**（管理器 GUI） | `ED9ModManager.exe` | Dear ImGui + DX11 图形界面：Mod 加载顺序与冲突可视化、资源自动合并（TBL / scene bjson / dat 脚本），Mod 可以是文件夹也可以是压缩包；按游戏目录自动识别 1st / 2nd；内置 **TBL⇄JSON**、**DAT⇄JSON**、**DDS 加解密** 页与 **PAC** 包浏览/编辑窗口；自动更新。 |
| **ED9ModManagerCLI**（命令行） | `ED9ModManagerCLI.exe` | 控制台转发壳：把命令交给 `ED9ModManager.exe`，等它跑完并传回输出与退出码（见下方「命令行」）。 |
| **modkit**（合并引擎） | 静态链接进上面两者 | `#TBL` 表 codec（schema 驱动读写/合并）、FPAC 读写、bjson 场景解码/patch、scene/dat 注入、压缩包 Mod 解包、DDS 加解密、mod 合并编排。 |
| **ed9_dat**（脚本引擎） | vendored 于 `ed9_dat/` | `#scp` dat 脚本的解析/组装引擎，供管理器 DAT⇄JSON 页与合并时的 dat 补丁复用。 |
| **示例插件** | `plugins/*.dll` | `SceneRedirect`（资源重定向不覆盖原文件）、`ScriptInject`（额外加载独立脚本 dat / 进图加载怪物表）、`PlayerPosGet`、`BattleProbe`、`EventStarter`、`SceneProbe`、`TexProbe`。 |

## 构建

需要 Visual Studio 2022（MSVC，x64）+ CMake ≥ 3.20。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

主要产物（`build\Release\`）：

- `xinput1_4.dll` —— 注入入口（放到游戏目录，与 `sora_1st.exe` / `sora_2nd.exe` 同级）。
- `ED9ModManager.exe`、`ED9ModManagerCLI.exe` —— Mod 管理器与命令行壳。
- `plugins\*.dll` —— 各示例插件（放到 `<游戏目录>\ED9Loader\plugins\`）。
- `Mod\` —— Mod本体所在位置（放到 `<游戏目录>\Mod\`）。

各游戏版本的函数地址表在 `src/rva/*.json`，编译时内嵌进 `xinput1_4.dll`；游戏目录下的 `ED9Loader\rva\<exe名>.json` 优先于内嵌表，游戏更新后换这张表即可，不必重新编译。改了 `src/rva/*.json` 或 `src/modkit/sora1_tbl_schemas.json` 后，先跑 `python tools/embed_rva.py` / `python tools/embed_schema.py` 重新生成内嵌头文件。

## 命令行

```
ED9ModManagerCLI.exe merge <游戏根目录> [--force]
ED9ModManagerCLI.exe tbl2json <tbl文件或目录>... [--out <目录>]
ED9ModManagerCLI.exe json2tbl <json文件或目录>... [--out <目录>]
ED9ModManagerCLI.exe dat2json <dat文件或目录>... [--out <目录>]
ED9ModManagerCLI.exe json2dat <json文件或目录>... [--out <目录>]
ED9ModManagerCLI.exe ddsdec <dds文件或目录>... [--out <目录>]
ED9ModManagerCLI.exe ddsenc <dds文件或目录>... [--out <目录>]
ED9ModManagerCLI.exe --version
```

## 目录结构

```
ED9ModManager/
├─ CMakeLists.txt          总构建脚本
├─ src/                    ED9Loader 框架 + modkit 合并引擎
│  ├─ ed9loader_api.h      插件 ABI(Ed9Api)
│  ├─ plugin_loader.*      加载器 / 控制台 / 崩溃日志 / 运行时定位
│  ├─ symbol_table.*       按游戏版本查函数地址(外部表优先,内嵌表兜底)
│  ├─ rva/                 各游戏版本的函数地址表(json)
│  └─ modkit/              tbl codec / bjson / scene / dat 注入 / 压缩包 / DDS / 合并编排
├─ tools/mod_manager/      ED9ModManager GUI(mod_manager.cpp)+ 命令行壳(cli_launcher.cpp)
├─ tools/embed_*.py        生成内嵌头文件(地址表 / tbl schema)
├─ examples/               示例插件源码(每个一个 .cpp)
├─ ed9_dat/                dat #scp 解析/组装引擎(vendored)
├─ third_party/            imgui / minhook / nlohmann / miniz(vendored)
└─ assets/                 图标
```

## 第三方

- [Dear ImGui](https://github.com/ocornut/imgui)(MIT)
- [MinHook](https://github.com/TsudaKageyu/minhook)(BSD-2-Clause)
- [nlohmann/json](https://github.com/nlohmann/json)(MIT)
- [KuroTools](https://github.com/nnguyen259/KuroTools)(MIT)
- [miniz](https://github.com/richgel999/miniz)(MIT)

详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
