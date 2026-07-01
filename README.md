# ED9ModManager

*A modding toolchain for the ED9/fdk engine build of «The Legend of Heroes: Trails in the Sky the 1st» (空の軌跡 the 1st).*

面向空之轨迹 the 1st（ED9/fdk 引擎，x64）的 Mod 工具链：一个原生插件加载框架 + 一个图形化 Mod 管理/合并器 + 若干示例插件。全部为 C++17/CMake，Windows 平台。

---

## 组成

| 组件 | 产物 | 说明 |
|---|---|---|
| **ED9Loader**（注入框架） | `xinput1_4.dll` | 类 BepInEx 的原生插件加载器：劫持 `xinput1_4` 入口，向插件暴露 `Ed9Api`（MinHook inline hook、配置 ini、按 RTTI 定位 vtable/实例、安全读写、控制台命令注册）。 |
| **ED9ModManager**（管理器 GUI） | `ED9ModManager.exe` | Dear ImGui + DX11 图形界面：Mod 加载顺序与冲突可视化、资源自动合并（TBL / scene bjson / dat 脚本），并内置 **TBL⇄JSON**、**DAT⇄JSON** 双向转换页。 |
| **modkit**（合并引擎） | 静态链接进上面两者 | `#TBL` 表 codec（schema 驱动读写/合并）、FPAC 读取、bjson 场景解码/patch、scene/dat 注入、mod 合并编排。附离线测试 `modkit_test`。 |
| **ed9_dat**（脚本引擎） | vendored 于 `ed9_dat/` | `#scp` dat 脚本的解析/组装引擎，供管理器 DAT⇄JSON 页复用。 |
| **示例插件** | `plugins/*.dll` | `SceneRedirect`（资源重定向不覆盖原文件）、`ScriptInject`（额外加载独立脚本 dat / 进图加载怪物表）、`PlayerPosGet`、`BattleProbe`、`EventStarter`、`SceneProbe`。 |

## 构建

需要 Visual Studio 2022（MSVC，x64）+ CMake ≥ 3.20。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

主要产物（`build\Release\`）：

- `xinput1_4.dll` —— 注入入口（放到游戏目录，与 `sora_1st.exe` 同级）。
- `ED9ModManager.exe` —— Mod 管理器。
- `plugins\*.dll` —— 各示例插件（放到 `<游戏目录>\ED9Loader\plugins\`）。
- `Mod\` —— Mod本体所在位置（放到 `<游戏目录>\Mod\`）。

## 目录结构

```
ED9ModManager/
├─ CMakeLists.txt          总构建脚本
├─ src/                    ED9Loader 框架 + modkit 合并引擎
│  ├─ ed9loader_api.h      插件 ABI(Ed9Api)
│  ├─ plugin_loader.*      加载器 / 控制台 / 崩溃日志 / 运行时定位
│  └─ modkit/              tbl codec / bjson / scene / dat 注入 / 合并编排
├─ tools/mod_manager/      ED9ModManager GUI(mod_manager.cpp)
├─ examples/               示例插件源码(每个一个 .cpp)
├─ ed9_dat/                dat #scp 解析/组装引擎(vendored)
├─ third_party/            imgui / minhook / nlohmann(vendored)
└─ assets/                 图标
```

## 第三方

- [Dear ImGui](https://github.com/ocornut/imgui)(MIT)
- [MinHook](https://github.com/TsudaKageyu/minhook)(BSD-2-Clause)
- [nlohmann/json](https://github.com/nlohmann/json)(MIT)

详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 许可

> 待补：请在开源前添加 `LICENSE` 文件并选择许可协议（第三方库各自的许可见 `THIRD_PARTY_NOTICES.md`）。
