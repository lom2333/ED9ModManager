cmake_minimum_required(VERSION 3.20)
project(sora_console_plugin LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MODKIT_SOURCES
    src/modkit/fpac_reader.cpp
    src/modkit/fpac_writer.cpp
    src/modkit/bjson_decoder.cpp
    src/modkit/bjson_patcher.cpp
    src/modkit/tbl_codec.cpp
    src/modkit/tbl_schema.cpp
    src/modkit/generic_tbl.cpp
    src/modkit/tbl_merge.cpp
    src/modkit/patch_config.cpp
    src/modkit/dat_patch.cpp
    src/modkit/scene_merge.cpp
    src/modkit/scene_inject.cpp
    src/modkit/mod_merge_orchestrator.cpp
    src/modkit/mod_archive.cpp
    src/modkit/dds_crypt.cpp
    third_party/miniz/miniz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ed9_dat/ed9_dat.cpp
)
set(MODKIT_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/ed9_dat
                        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)

set(SORA_CONSOLE_COMMON_SOURCES
    src/command_console.cpp
    src/crash_logger.cpp
    src/runtime_locator.cpp
    src/safe_scan.cpp
    src/plugin_loader.cpp
    src/symbol_table.cpp
    ${MODKIT_SOURCES}
)

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include)

add_library(xinput1_4 SHARED
    ${SORA_CONSOLE_COMMON_SOURCES}
    src/xinput_proxy.cpp
)

target_include_directories(xinput1_4 PRIVATE src third_party/nlohmann ${MODKIT_INCLUDE_DIRS})

target_compile_definitions(xinput1_4 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if (MSVC)
    target_compile_options(xinput1_4 PRIVATE /utf-8 /W4 /EHsc)
else()
    target_compile_options(xinput1_4 PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(xinput1_4 PRIVATE dbghelp minhook imm32)

set_target_properties(xinput1_4 PROPERTIES
    OUTPUT_NAME "xinput1_4"
    PREFIX ""
    SUFFIX ".dll"
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/xinput_proxy.def\""
)

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

add_library(TexProbe SHARED examples/tex_probe/tex_probe.cpp)
target_include_directories(TexProbe PRIVATE src)
target_compile_definitions(TexProbe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(TexProbe PRIVATE /utf-8 /W4 /EHsc)
endif()
set_target_properties(TexProbe PROPERTIES
    OUTPUT_NAME "TexProbe"
    PREFIX ""
    SUFFIX ".dll"
)

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

add_executable(mod_manager WIN32
    tools/mod_manager/mod_manager.cpp
    tools/mod_manager/app_icon.rc
    ${MODKIT_SOURCES}
)
target_include_directories(mod_manager PRIVATE src third_party/nlohmann third_party/imgui
    third_party/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/ed9_dat)
target_compile_definitions(mod_manager PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(mod_manager PRIVATE /utf-8 /EHsc)
endif()
target_link_libraries(mod_manager PRIVATE imgui d3d11 dxgi dxguid d3dcompiler shell32 ole32 comdlg32 windowscodecs winhttp)
set_target_properties(mod_manager PROPERTIES OUTPUT_NAME "ED9ModManager")

add_executable(mod_manager_cli tools/mod_manager/cli_launcher.cpp)
target_compile_definitions(mod_manager_cli PRIVATE UNICODE _UNICODE)
if (MSVC)
    target_compile_options(mod_manager_cli PRIVATE /utf-8)
endif()
set_target_properties(mod_manager_cli PROPERTIES OUTPUT_NAME "ED9ModManagerCLI")
