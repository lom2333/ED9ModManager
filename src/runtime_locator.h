#pragma once

#include <cstdint>
#include <string_view>

namespace sora_console::runtime_locator {
std::uintptr_t FindVtableByType(std::string_view fragment);

std::uintptr_t FindInstanceByVtable(std::uintptr_t vtable_va);

}
