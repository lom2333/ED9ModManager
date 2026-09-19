#pragma once

#include <cstdint>

namespace sora_console::symbol_table {
void Load();

std::uintptr_t Resolve(const char* name);

const char* StatusText();

}
