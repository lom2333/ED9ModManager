#pragma once

namespace sora_console::command_console {

void Start();

// 插件命令系统(供 plugin_loader 暴露成 Ed9Api 的 register_command / console_print)。
using CommandFn = void (*)(int argc, const char** argv);
void RegisterCommand(const char* name, const char* help, CommandFn fn);  // 线程安全
void ConsolePrint(const char* msg);                                      // 输出到控制台

}  // namespace sora_console::command_console
