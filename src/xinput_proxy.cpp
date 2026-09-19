#include "crash_logger.h"
#include "command_console.h"
#include "plugin_loader.h"

#include <Windows.h>
#include <Xinput.h>
#include <imm.h>

#include <cstdint>
#include <cstring>

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
        ed9loader::plugin_loader::LoadAll();
        sora_console::crash_logger::InstallProcessExitHooks();
        sora_console::command_console::Start();
    });
}

int g_imm_disable_blocked = 0;

BOOL WINAPI ImmDisableIME_Stub(DWORD) {
    ++g_imm_disable_blocked;
    return TRUE;
}

bool PatchIatEntry(HMODULE module, const char* dll_name, const char* func_name,
                   void* replacement) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;

    const auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name != 0; ++imp) {
        const char* name = reinterpret_cast<const char*>(base + imp->Name);
        if (_stricmp(name, dll_name) != 0) continue;
        const DWORD thunk_rva = imp->OriginalFirstThunk != 0 ? imp->OriginalFirstThunk
                                                             : imp->FirstThunk;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + thunk_rva);
        auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData != 0; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + names[i].u1.AddressOfData);
            if (strcmp(by_name->Name, func_name) != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&addrs[i], sizeof(addrs[i]), PAGE_READWRITE, &old) == 0) {
                return false;
            }
            addrs[i].u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&addrs[i], sizeof(addrs[i]), old, &old);
            return true;
        }
    }
    return false;
}

bool ReadIniFlagRaw(HINSTANCE self, const char* key) {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return false;
    *slash = 0;
    if (wcslen(path) + 32 >= MAX_PATH) return false;
    wcscat_s(path, MAX_PATH, L"\\ED9Loader\\config\\ED9Loader.ini");

    const HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096] = {};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return false;
    buf[got] = 0;
    for (const char* p = buf; *p != 0;) {
        while (*p == ' ' || *p == '\t') ++p;
        const char* line = p;
        while (*p != 0 && *p != '\n') ++p;
        const char* end = p;
        if (*p != 0) ++p;
        if (*line == ';' || *line == '#') continue;
        const size_t klen = strlen(key);
        if (static_cast<size_t>(end - line) <= klen) continue;
        if (_strnicmp(line, key, klen) != 0) continue;
        const char* q = line + klen;
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
        if (q >= end || *q != '=') continue;
        ++q;
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
        return q < end && *q >= '1' && *q <= '9';
    }
    return false;
}

HIMC WINAPI ImmAssociateContext_Stub(HWND hwnd, HIMC) {
    return ImmAssociateContext(hwnd, nullptr);
}

void BlockImmDisableIME(HINSTANCE self) {
    if (!ReadIniFlagRaw(self, "ime_support")) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!PatchIatEntry(exe, "IMM32.dll", "ImmDisableIME",
                       reinterpret_cast<void*>(&ImmDisableIME_Stub))) {
        return;
    }
    if (!PatchIatEntry(exe, "IMM32.dll", "ImmAssociateContext",
                       reinterpret_cast<void*>(&ImmAssociateContext_Stub))) {
        PatchIatEntry(exe, "IMM32.dll", "ImmDisableIME",
                      reinterpret_cast<void*>(&ImmDisableIME));
    }
}

DWORD WINAPI BootstrapThread(LPVOID) {
    try { InitializePluginRuntime(); } catch (...) {}
    return 0;
}

}

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
        BlockImmDisableIME(instance);
        const HANDLE t = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (t != nullptr) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        sora_console::crash_logger::NoteProcessExit();
        if (g_real_xinput != nullptr && reserved == nullptr) {
            FreeLibrary(g_real_xinput);
            g_real_xinput = nullptr;
            g_real_xinput_get_state = nullptr;
            g_real_xinput_set_state = nullptr;
        }
    }
    return TRUE;
}
