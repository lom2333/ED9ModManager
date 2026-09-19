#pragma once

#include <filesystem>

namespace sora_console::crash_logger {

void Install();
void NoteProcessExit();
void InstallProcessExitHooks();
void TriggerTestCrash();
std::filesystem::path GetLogDirectory();

}
