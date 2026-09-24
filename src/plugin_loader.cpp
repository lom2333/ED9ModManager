#include "plugin_loader.h"

#include "command_console.h"
#include "ed9loader_api.h"
#include "runtime_locator.h"
#include "safe_scan.h"
#include "symbol_table.h"
#include "modkit/mod_merge_orchestrator.h"

#include <MinHook.h>
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace ed9loader::plugin_loader {
namespace {

std::mutex g_log_mutex;

[[nodiscard]] std::filesystem::path ExeDir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD count = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buffer).parent_path();
}

bool g_console_ready = false;

void EnsureConsole() {
    const auto cfg_dir = ExeDir() / "ED9Loader" / "config";
    std::error_code error;
    std::filesystem::create_directories(cfg_dir, error);
    const auto ini = (cfg_dir / "ED9Loader.ini").string();
    if (!std::filesystem::exists(ini, error)) {
        WritePrivateProfileStringA("Settings", "console", "1", ini.c_str());
    }
    if (GetPrivateProfileIntA("Settings", "console", 1, ini.c_str()) == 0) {
        return;
    }
    bool created_own = false;
    if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) {
        created_own = (AllocConsole() != FALSE);
    }
    SetConsoleOutputCP(CP_UTF8);
    if (created_own) {
        SetConsoleTitleW(L"ED9Loader 日志");
        CONSOLE_FONT_INFOEX fi = {};
        fi.cbSize = sizeof(fi);
        fi.dwFontSize.Y = 16;
        fi.FontFamily = FF_DONTCARE;
        fi.FontWeight = FW_NORMAL;
        wcscpy_s(fi.FaceName, L"NSimSun");
        SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &fi);
    }
    g_console_ready = true;
}

void ConsoleWrite(const std::string& line) {
    if (!g_console_ready) {
        return;
    }
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string s = line + "\n";
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) {
        return;
    }
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), wlen);
    DWORD written = 0;
    WriteConsoleW(out, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
}

void WriteLog(const std::string& line) {
    std::scoped_lock lock(g_log_mutex);
    const auto path = ExeDir() / "ED9Loader" / "loader.log";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (file.is_open()) {
        file << line << "\n";
        file.flush();
    }
    ConsoleWrite(line);
}

void ApiLog(const char* msg) {
    if (msg != nullptr) {
        WriteLog(std::string("  [plugin] ") + msg);
    }
}

void* ApiGetModuleBase() {
    return reinterpret_cast<void*>(GetModuleHandleW(nullptr));
}

int ApiInstallHook(void* target, void* detour, void** original) {
    if (target == nullptr || detour == nullptr) {
        return -1;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        return -2;
    }
    if (MH_EnableHook(target) != MH_OK) {
        return -3;
    }
    return 0;
}

void* ApiFindVtable(const char* type_fragment) {
    if (type_fragment == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(sora_console::runtime_locator::FindVtableByType(type_fragment));
}

int ApiSafeRead(const void* addr, void* out, unsigned long size) {
    return sora_console::safe_scan::SafeReadBytes(reinterpret_cast<std::uintptr_t>(addr), out, size) ? 1 : 0;
}

int ApiSafeWrite(void* addr, const void* src, unsigned long size) {
    return sora_console::safe_scan::SafeWriteBytes(reinterpret_cast<std::uintptr_t>(addr), src, size) ? 1 : 0;
}

[[nodiscard]] std::string CfgPathStr(const char* cfg_name) {
    return (ExeDir() / "ED9Loader" / "config" / (std::string(cfg_name) + ".ini")).string();
}

void EnsureCfgDir() {
    std::error_code error;
    std::filesystem::create_directories(ExeDir() / "ED9Loader" / "config", error);
}

int ApiCfgGetInt(const char* cfg_name, const char* key, int def_value) {
    if (cfg_name == nullptr || key == nullptr) {
        return def_value;
    }
    return static_cast<int>(GetPrivateProfileIntA("Settings", key, def_value, CfgPathStr(cfg_name).c_str()));
}

int ApiCfgGetStr(const char* cfg_name, const char* key, const char* def_value, char* out, int out_size) {
    if (cfg_name == nullptr || key == nullptr || out == nullptr || out_size <= 0) {
        return 0;
    }
    return static_cast<int>(GetPrivateProfileStringA(
        "Settings", key, def_value != nullptr ? def_value : "",
        out, static_cast<DWORD>(out_size), CfgPathStr(cfg_name).c_str()));
}

void ApiCfgSetInt(const char* cfg_name, const char* key, int value) {
    if (cfg_name == nullptr || key == nullptr) {
        return;
    }
    EnsureCfgDir();
    WritePrivateProfileStringA("Settings", key, std::to_string(value).c_str(), CfgPathStr(cfg_name).c_str());
}

void ApiCfgSetStr(const char* cfg_name, const char* key, const char* value) {
    if (cfg_name == nullptr || key == nullptr) {
        return;
    }
    EnsureCfgDir();
    WritePrivateProfileStringA("Settings", key, value != nullptr ? value : "", CfgPathStr(cfg_name).c_str());
}

void* ApiFindInstance(void* vtable) {
    if (vtable == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(
        sora_console::runtime_locator::FindInstanceByVtable(reinterpret_cast<std::uintptr_t>(vtable)));
}

int ApiRegisterCommand(const char* name, const char* help, Ed9CommandFn fn) {
    if (name == nullptr || fn == nullptr) {
        return -1;
    }
    sora_console::command_console::RegisterCommand(
        name, help, reinterpret_cast<sora_console::command_console::CommandFn>(fn));
    return 0;
}

void ApiConsolePrint(const char* msg) {
    sora_console::command_console::ConsolePrint(msg);
}

void* ApiResolveSymbol(const char* name) {
    return reinterpret_cast<void*>(sora_console::symbol_table::Resolve(name));
}

Ed9Api g_api = {
    ED9LOADER_ABI_VERSION,
    ApiLog,
    ApiGetModuleBase,
    ApiInstallHook,
    ApiFindVtable,
    ApiSafeRead,
    ApiSafeWrite,
    ApiCfgGetInt,
    ApiCfgGetStr,
    ApiCfgSetInt,
    ApiCfgSetStr,
    ApiFindInstance,
    ApiRegisterCommand,
    ApiConsolePrint,
    ApiResolveSymbol,
};

[[nodiscard]] std::string SafeName(const std::filesystem::path& path) {
    try {
        return path.filename().string();
    } catch (...) {
        return "<name?>";
    }
}

void DoLoadAll() {
    EnsureConsole();
    const auto plugins_dir = ExeDir() / "ED9Loader" / "plugins";
    WriteLog("==== ED9Loader start (abi=" + std::to_string(ED9LOADER_ABI_VERSION) + ") ====");

    try {
        const auto paths = modkit::orchestrator::FromGameDir(ExeDir().wstring());
        const auto result = modkit::orchestrator::Run(paths, false);
        WriteLog(result.log);
    } catch (...) {
        WriteLog("[modkit] orchestrator threw, skipped");
    }

    sora_console::symbol_table::Load();
    WriteLog(std::string("[symbols] ") + sora_console::symbol_table::StatusText());

    const MH_STATUS mh = MH_Initialize();
    WriteLog(std::string("MinHook init: ") +
             (mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED
                  ? "OK"
                  : "FAILED status=" + std::to_string(static_cast<int>(mh))));

    std::error_code error;
    if (!std::filesystem::exists(plugins_dir, error)) {
        std::filesystem::create_directories(plugins_dir, error);
        WriteLog("ED9Loader\\plugins not found -> created empty dir, nothing to load");
        return;
    }

    std::vector<std::filesystem::path> dlls;
    for (const auto& entry : std::filesystem::directory_iterator(plugins_dir, error)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto& path = entry.path();
        if (_wcsicmp(path.extension().c_str(), L".dll") != 0) {
            continue;
        }
        dlls.push_back(path);
    }
    static const wchar_t* const kLoadFirst[] = {L"SceneRedirect.dll", L"ScriptInject.dll"};
    const auto rank = [](const std::filesystem::path& p) {
        const std::wstring n = p.filename().wstring();
        for (size_t i = 0; i < std::size(kLoadFirst); ++i) {
            if (_wcsicmp(n.c_str(), kLoadFirst[i]) == 0) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(std::size(kLoadFirst));
    };
    std::stable_sort(dlls.begin(), dlls.end(), [&rank](const std::filesystem::path& a, const std::filesystem::path& b) {
        const int ra = rank(a);
        const int rb = rank(b);
        if (ra != rb) {
            return ra < rb;
        }
        return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
    });

    int found = 0;
    int loaded = 0;
    for (const auto& path : dlls) {
        ++found;
        const std::string name = SafeName(path);

        const HMODULE module = LoadLibraryW(path.wstring().c_str());
        if (module == nullptr) {
            WriteLog("FAILED LoadLibrary: " + name + " (err=" + std::to_string(GetLastError()) + ")");
            continue;
        }

        const auto plugin_load = reinterpret_cast<Plugin_Load_t>(GetProcAddress(module, "Plugin_Load"));
        if (plugin_load == nullptr) {
            WriteLog("SKIP (no Plugin_Load export): " + name);
            continue;
        }

        try {
            plugin_load(&g_api);
            ++loaded;
            WriteLog("LOADED: " + name);
        } catch (...) {
            WriteLog("EXCEPTION in Plugin_Load: " + name);
        }
    }

    WriteLog("==== done: found=" + std::to_string(found) + " loaded=" + std::to_string(loaded) + " ====");
}

}

void LoadAll() {
    static std::once_flag once;
    std::call_once(once, DoLoadAll);
}

}
