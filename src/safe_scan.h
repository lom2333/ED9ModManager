#pragma once

#include <cstddef>
#include <cstdint>

namespace sora_console::safe_scan {

struct ScanHit {
    std::uintptr_t address = 0;
    std::uintptr_t vtable = 0;
};

int SafeScanRegionForVtables(std::uintptr_t begin, std::uintptr_t end,
                             const std::uintptr_t* vtables, int vtable_count,
                             ScanHit* out, int out_cap);

bool SafeReadBytes(std::uintptr_t addr, void* dst, std::size_t n);

bool SafeWriteBytes(std::uintptr_t addr, const void* src, std::size_t n);

}
