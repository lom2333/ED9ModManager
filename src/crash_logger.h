#pragma once

#include <filesystem>

namespace sora_console::crash_logger {

void Install();
void TriggerTestCrash();
std::filesystem::path GetLogDirectory();

}  // namespace sora_console::crash_logger
