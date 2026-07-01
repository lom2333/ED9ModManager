cmake_minimum_required(VERSION 3.20)
project(sora_console_plugin LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# modkit: mod 资源自动合并(框架级)
set(MODKIT_SOURCES
    src/modkit/fpac_reader.cpp
    src/modkit/bjson_decoder.cpp
    src/modkit/bjson_patcher.cpp
    src/modkit/tbl_codec.cpp
    src/modkit/tbl_schema.cpp
    src/modkit/generic_tbl.cpp
    src/modkit/tbl_merge.cpp
    src/modkit/patch_config.cpp
    src/modkit/scene_merge.cpp
    src/modkit/scene_inject.cpp
    src/modkit/mod_merge_orchestrator.cpp
)

set(SORA_CONSOLE_COMMON_SOURCES
    src/command_console.cpp
    src/crash_logger.cpp
    src/runtime_locator.cpp
    src/safe_scan.cpp
    src/plugin_loader.cpp
    ${MODKIT_SOURCES}
)

# ---- MinHook 静态库(inline hook,BepInEx 的 Harmony 等价物)----
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include)

# 注入入口:仅 xinput1_4.dll 劫持(统一单一入口)。
add_library(xinput1_4 SHARED
    ${SORA_CONSOLE_COMMON_SOURCES}
    src/xinput_proxy.cpp
)

target_include_directories(xinput1_4 PRIVATE src third_party/nlohmann)

target_compile_definitions(xinput1_4 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if (MSVC)
    target_compile_options(xinput1_4 PRIVATE /utf-8 /W4 /EHsc)
else()
    target_compile_options(xinput1_4 PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(xinput1_4 PRIVATE dbghelp minhook)

set_target_properties(xinput1_4 PROPERTIES
    OUTPUT_NAME "xinput1_4"
    PREFIX ""
    SUFFIX ".dll"
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/xinput_proxy.def\""
)

# ---- 示范插件 PlayerPosGet(实时坐标,完整复现坐标链,全用框架 API)----
add_library(PlayerPosGet SHARED examples/player_pos_get/player_pos_get.cpp)
target_include_directories(PlayerPosGet PRIVATE src)
target_compile_definitions(PlayerPosGet PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(PlayerPosGet PRIVATE /utf-8 /W4 /EHsc)
endif()
set_target_properties(PlayerPosGet PROPERTIES
    OUTPUT_NAME "PlayerPosGet"
    PREFIX ""
    SUFFIX ".dll"
)

# ---- 观测插件 SceneProbe(hook FindFile/Open,确认 scene 查找键 .json/.bin)----
add_library(SceneProbe SHARED examples/scene_probe/scene_probe.cpp)
target_include_directories(SceneProbe PRIVATE src)
target_compile_definitions(SceneProbe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(SceneProbe PRIVATE /utf-8 /W4 /EHsc)
endif()
set_target_properties(SceneProbe PROPERTIES
    OUTPUT_NAME "SceneProbe"
    PREFIX ""
    SUFFIX ".dll"
)

# ---- 资源重定向插件 SceneRedirect(FindFile→0 + CreateFileW 重定向到 mod 文件)----
add_library(SceneRedirect SHARED examples/scene_redirect/scene_redirect.cpp)
target_include_directories(SceneRedirect PRIVATE src)
target_compile_definitions(SceneRedirect PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(SceneRedirect PRIVATE /utf-8 /W4 /EHsc)
endif()
set_target_properties(SceneRedirect PROPERTIES
    OUTPUT_NAME "SceneRedirect"
    PREFIX ""
    SUFFIX ".dll"
)

# ---- 脚本注入插件 ScriptInject(hook FUN_140247460,让引擎额外加载独立脚本 dat)----
add_library(ScriptInject SHARED examples/script_inject/script_inject.cpp)
target_include_directories(ScriptInject PRIVATE src)
target_compile_definitions(ScriptInject PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(ScriptInject PRIVATE /utf-8 /W4 /EHsc)
endif()
set_target_properties(ScriptInject PROPERTIES
    OUTPUT_NAME "ScriptInject"
    PREFIX ""
    SUFFIX ".dll"
)

# ---- 诊断插件 BattleProbe(实时 dump 战斗区列表,逆向 script_command_btl.cpp)----
add_library(BattleProbe SHARED examples/battle_probe/battle_probe.cpp)
target_include_directories(BattleProbe PRIVATE src)
target_compile_definitions(BattleProbe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(BattleProbe PRIVATE /utf-8 /W4 /EHsc)
endif()
set_target_properties(BattleProbe PROPERTIES
    OUTPUT_NAME "BattleProbe"
    PREFIX ""
    SUFFIX ".dll"
)

# ---- 插件 EventStarter(把脚本函数当真事件线程启动,FUN_140247550;对话触发事件战斗)----
add_library(EventStarter SHARED examples/event_starter/event_starter.cpp)
target_include_directories(EventStarter PRIVATE src)
target_compile_definitions(EventStarter PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(EventStarter PRIVATE /utf-8 /W4 /EHsc)
endif()
set_target_properties(EventStarter PROPERTIES
    OUTPUT_NAME "EventStarter"
    PREFIX ""
    SUFFIX ".dll"
)

# ---- modkit 离线测试 harness(字节对照 golden,不依赖游戏;MODKIT_SOURCES 定义在顶部)----
add_executable(modkit_test
    ${MODKIT_SOURCES}
    src/modkit/modkit_test.cpp
)
target_include_directories(modkit_test PRIVATE src third_party/nlohmann)
target_compile_definitions(modkit_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(modkit_test PRIVATE /utf-8 /W4 /EHsc)
endif()

# modkit_cli 已并入 ED9ModManager(`ED9ModManager.exe merge <游戏根> [--force]`),不再单独构建。

# ---- Dear ImGui(GUI 框架,供 ED9 Mod Manager;Win32 + DX11 后端)----
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
if (MSVC)
    target_compile_options(imgui PRIVATE /utf-8)
endif()

# ---- ED9 Mod Manager(加载顺序 + 冲突可视化;复用 modkit;无控制台)----
add_executable(mod_manager WIN32
    tools/mod_manager/mod_manager.cpp
    tools/mod_manager/app_icon.rc
    ${MODKIT_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/ed9_dat/ed9_dat.cpp   # dat #scp 解析/组装引擎(供 DAT⇄JSON 页复用;vendored)
)
target_include_directories(mod_manager PRIVATE src third_party/nlohmann third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/ed9_dat)
target_compile_definitions(mod_manager PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(mod_manager PRIVATE /utf-8 /EHsc)
endif()
target_link_libraries(mod_manager PRIVATE imgui d3d11 d3dcompiler shell32 ole32 comdlg32)
set_target_properties(mod_manager PROPERTIES OUTPUT_NAME "ED9ModManager")
