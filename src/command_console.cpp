#include "command_console.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sora_console::command_console {
namespace {

std::once_flag g_start_once;
std::atomic<bool> g_running = false;

struct PluginCommand {
    std::string name;
    std::string help;
    CommandFn fn = nullptr;
};
std::mutex g_cmd_mutex;
std::vector<PluginCommand> g_commands;

std::vector<std::string> SplitArguments(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> out;
    std::string item;
    while (stream >> item) {
        out.push_back(item);
    }
    return out;
}

void PrintHelp() {
    std::cout
        << "Commands:\n"
        << "  help        Show this help.\n"
        << "  hide        Hide the console window.\n";
    std::scoped_lock lock(g_cmd_mutex);
    if (!g_commands.empty()) {
        std::cout << "Plugin commands:\n";
        for (const auto& command : g_commands) {
            std::cout << "  " << command.name;
            if (!command.help.empty()) {
                std::cout << "   -- " << command.help;
            }
            std::cout << "\n";
        }
    }
}

void HandleCommand(const std::string& line) {
    const auto args = SplitArguments(line);
    if (args.empty()) {
        return;
    }
    const std::string& command = args[0];
    if (command == "help") {
        PrintHelp();
        return;
    }
    if (command == "hide") {
        if (HWND console = GetConsoleWindow()) {
            ShowWindow(console, SW_HIDE);
        }
        return;
    }

    // 插件注册的命令
    CommandFn fn = nullptr;
    {
        std::scoped_lock lock(g_cmd_mutex);
        for (const auto& entry : g_commands) {
            if (entry.name == command) {
                fn = entry.fn;
                break;
            }
        }
    }
    if (fn != nullptr) {
        std::vector<const char*> argv;
        argv.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(arg.c_str());
        }
        fn(static_cast<int>(argv.size()), argv.data());
        return;
    }

    std::cout << "unknown command: " << command << "\n";
}

void AttachConsoleStreams() {
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONIN$", "r", stdin);
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cin.clear();
}

void ConsoleThreadMain() {
    // 控制台已由 plugin_loader 的 EnsureConsole 按 ini 决定是否创建(本线程在 LoadAll 之后才启动)。
    // 这里只接管「输入」:有控制台 → 把 stdin 接上、跑输入循环;没有(用户在 ini 里关了 console)→ 退出。
    // ⚠ 旧代码自己 AllocConsole——但 EnsureConsole 已先建控制台,AllocConsole 必失败 return,导致输入循环从不运行(终端打不了字)。
    if (GetConsoleWindow() == nullptr) {
        return;
    }
    AttachConsoleStreams();   // freopen CONIN$/CONOUT$ → 把 std::cin 接到现有控制台,getline 才能读到输入

    std::cout << "[ED9Loader] console ready (可输入命令,如 -get pos)\n";
    PrintHelp();
    std::cout << "\n";

    g_running = true;
    std::string line;
    while (g_running) {
        std::cout << "> ";
        std::cout.flush();
        if (!std::getline(std::cin, line)) {
            break;
        }
        HandleCommand(line);
    }

    std::cout << "[ED9Loader] console shutting down\n";
}

}  // namespace

void Start() {
    std::call_once(g_start_once, []() {
        std::thread(ConsoleThreadMain).detach();
    });
}

void RegisterCommand(const char* name, const char* help, CommandFn fn) {
    if (name == nullptr || fn == nullptr) {
        return;
    }
    std::scoped_lock lock(g_cmd_mutex);
    g_commands.push_back({name, help != nullptr ? help : "", fn});
}

void ConsolePrint(const char* msg) {
    if (msg != nullptr) {
        std::cout << msg;
        std::cout.flush();
    }
}

}  // namespace sora_console::command_console
