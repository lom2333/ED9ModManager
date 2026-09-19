#pragma once

namespace sora_console::command_console {

void Start();

using CommandFn = void (*)(int argc, const char** argv);
void RegisterCommand(const char* name, const char* help, CommandFn fn);
void ConsolePrint(const char* msg);

}
