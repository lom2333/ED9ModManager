#include "crash_logger.h"
#include "command_console.h"
#include "plugin_loader.h"

#include <Windows.h>
#include <Xinput.h>

#include <mutex>
#include <stdexcept>
#include <string>

namespace {

std::once_flag g_runtime_once;
std::once_flag g_xinput_load_once;
HMODULE g_real_xinput = nullptr;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

XInputGetStateFn g_real_xinput_get_state = nullptr;
XInputSetStateFn g_real_xinput_set_state = nullptr;

std::wstring BuildSystemXinputPath() {
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        throw std::runtime_error("GetSystemDirectoryW failed");
    }
    std::wstring path(system_dir);
    path += L"\\xinput1_4.dll";
    return path;
}

void LoadRealXinput() {
    std::call_once(g_xinput_load_once, []() {
        const std::wstring path = BuildSystemXinputPath();
        g_real_xinput = LoadLibraryW(path.c_str());
        if (g_real_xinput == nullptr) {
            throw std::runtime_error("LoadLibraryW(xinput1_4.dll) failed");
        }
        g_real_xinput_get_state = reinterpret_cast<XInputGetStateFn>(
            GetProcAddress(g_real_xinput, "XInputGetState")
        );
        g_real_xinput_set_state = reinterpret_cast<XInputSetStateFn>(
            GetProcAddress(g_real_xinput, "XInputSetState")
        );
        if (g_real_xinput_get_state == nullptr || g_real_xinput_set_state == nullptr) {
            throw std::runtime_error("GetProcAddress(XInput*) failed");
        }
    });
}

void InitializePluginRuntime() {
    std::call_once(g_runtime_once, []() {
        sora_console::crash_logger::Install();
        ed9loader::plugin_loader::LoadAll();      // 先:EnsureConsole 建控制台 + 加载插件 + 注册命令
        sora_console::command_console::Start();    // 后:输入线程接管已建好的控制台(避免两处抢 AllocConsole)
    });
}

}  // namespace

extern "C" DWORD WINAPI SoraXInputGetState(DWORD user_index, XINPUT_STATE* state) {
    try {
        InitializePluginRuntime();
        LoadRealXinput();
        return g_real_xinput_get_state(user_index, state);
    } catch (...) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
}

extern "C" DWORD WINAPI SoraXInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) {
    try {
        InitializePluginRuntime();
        LoadRealXinput();
        return g_real_xinput_set_state(user_index, vibration);
    } catch (...) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_real_xinput != nullptr && reserved == nullptr) {
            FreeLibrary(g_real_xinput);
            g_real_xinput = nullptr;
            g_real_xinput_get_state = nullptr;
            g_real_xinput_set_state = nullptr;
        }
    }
    return TRUE;
}
