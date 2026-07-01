// ED9Loader 示范插件:玩家坐标。两个出口:
//   1) 后台线程实时写 ED9Loader/PlayerPosGet.pos.txt
//   2) 控制台命令 -get pos (ABI v6 注册)
// 坐标精确链全部走框架 Ed9Api,零硬编码地址:
//   find_vtable("Manager@fieldmap@sora") -> find_instance() 得 Manager 单例
//   -> *(Manager + 0x640) 得 Player -> float[3] @ Player + 0x290
// 偏移可在 ED9Loader/config/PlayerPosGet.ini 调(十进制:0x640=1600, 0x290=656)。
#include "ed9loader_api.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static const Ed9Api* g_api = nullptr;
static volatile bool g_running = false;
static unsigned g_poll_ms = 1000;
static unsigned g_mgr_to_player = 0x640;
static unsigned g_pos_off = 0x290;
static void* g_player_vt = nullptr;

// 坐标精确链,全部通过框架 API。成功填 out[3] 并返回 true。
static bool ReadPlayerPos(float* out) {
    if (g_api == nullptr || g_api->find_vtable == nullptr ||
        g_api->find_instance == nullptr || g_api->safe_read == nullptr) {
        return false;
    }
    void* mgr_vt = g_api->find_vtable("Manager@fieldmap@sora");
    void* manager = (mgr_vt != nullptr) ? g_api->find_instance(mgr_vt) : nullptr;
    if (manager == nullptr) {
        return false;
    }
    uintptr_t player = 0;
    if (g_api->safe_read(reinterpret_cast<char*>(manager) + g_mgr_to_player,
                         &player, sizeof(player)) == 0 || player == 0) {
        return false;
    }
    void* pvt = nullptr;
    g_api->safe_read(reinterpret_cast<void*>(player), &pvt, sizeof(pvt));
    if (g_player_vt != nullptr && pvt != g_player_vt) {
        return false;
    }
    return g_api->safe_read(reinterpret_cast<char*>(player) + g_pos_off, out, sizeof(float) * 3) != 0;
}

static void GetOutPath(char* out, size_t n) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash != nullptr) {
        *slash = '\0';
    }
    _snprintf_s(out, n, _TRUNCATE, "%s\\ED9Loader\\PlayerPosGet.pos.txt", exe);
}

static DWORD WINAPI ThreadMain(LPVOID) {
    char outpath[MAX_PATH] = {};
    GetOutPath(outpath, sizeof(outpath));
    while (g_running) {
        Sleep(g_poll_ms);
        float pos[3] = {0.0f, 0.0f, 0.0f};
        const bool ok = ReadPlayerPos(pos);
        char line[128] = {};
        if (ok) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "X=%.2f  Y=%.2f  Z=%.2f", pos[0], pos[1], pos[2]);
        } else {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "(waiting for field scene)");
        }
        FILE* file = nullptr;
        if (fopen_s(&file, outpath, "w") == 0 && file != nullptr) {
            fputs(line, file);
            fputc('\n', file);
            fclose(file);
        }
    }
    return 0;
}

// 控制台命令: -get pos
static void Cmd_Get(int argc, const char** argv) {
    if (g_api == nullptr || g_api->console_print == nullptr) {
        return;
    }
    if (argc < 2 || strcmp(argv[1], "pos") != 0) {
        g_api->console_print("usage: -get pos\n");
        return;
    }
    float pos[3] = {0.0f, 0.0f, 0.0f};
    char buf[128] = {};
    if (ReadPlayerPos(pos)) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "X=%.2f  Y=%.2f  Z=%.2f\n", pos[0], pos[1], pos[2]);
    } else {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[ERR] position unavailable (enter a field scene first)\n");
    }
    g_api->console_print(buf);
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) {
        return;
    }
    if (api->abi_version < 5) {
        return;
    }
    g_api = api;

    if (api->cfg_get_int != nullptr) {
        g_poll_ms = static_cast<unsigned>(api->cfg_get_int("PlayerPosGet", "poll_ms", 1000));
        g_mgr_to_player = static_cast<unsigned>(api->cfg_get_int("PlayerPosGet", "manager_to_player_offset", 0x640));
        g_pos_off = static_cast<unsigned>(api->cfg_get_int("PlayerPosGet", "pos_offset", 0x290));
        if (api->cfg_set_int != nullptr) {
            api->cfg_set_int("PlayerPosGet", "poll_ms", static_cast<int>(g_poll_ms));
            api->cfg_set_int("PlayerPosGet", "manager_to_player_offset", static_cast<int>(g_mgr_to_player));
            api->cfg_set_int("PlayerPosGet", "pos_offset", static_cast<int>(g_pos_off));
        }
    }
    g_player_vt = (api->find_vtable != nullptr) ? api->find_vtable("Player@fieldmap@sora") : nullptr;

    // 注册控制台命令 -get pos(需要 ABI v6)
    if (api->abi_version >= 6 && api->register_command != nullptr) {
        api->register_command("-get", "print player coords (usage: -get pos)", Cmd_Get);
    }

    g_running = true;
    CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
