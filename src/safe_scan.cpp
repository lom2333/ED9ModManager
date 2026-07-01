#include "safe_scan.h"

#include <Windows.h>
#include <cstring>

// 本文件用 SEH（__try/__except）保护内存访问。函数体内不放任何需要栈展开的
// C++ 对象，以满足 MSVC 对 __try 的限制。
namespace sora_console::safe_scan {

int SafeScanRegionForVtables(std::uintptr_t begin, std::uintptr_t end,
                             const std::uintptr_t* vtables, int vtable_count,
                             ScanHit* out, int out_cap) {
    int n = 0;
    if (vtables == nullptr || vtable_count <= 0 || out == nullptr || out_cap <= 0) {
        return 0;
    }
    __try {
        for (std::uintptr_t addr = begin; addr + sizeof(std::uintptr_t) <= end;
             addr += sizeof(std::uintptr_t)) {
            const std::uintptr_t value = *reinterpret_cast<const std::uintptr_t*>(addr);
            for (int i = 0; i < vtable_count; ++i) {
                if (value == vtables[i]) {
                    if (n < out_cap) {
                        out[n].address = addr;
                        out[n].vtable = value;
                        ++n;
                    }
                    break;
                }
            }
            if (n >= out_cap) {
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 该区域中途变得不可读：返回此前已收集的命中。
    }
    return n;
}

bool SafeReadBytes(std::uintptr_t addr, void* dst, std::size_t n) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteBytes(std::uintptr_t addr, const void* src, std::size_t n) {
    if (addr == 0 || src == nullptr || n == 0) {
        return false;
    }
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old_protect) == 0) {
        return false;
    }
    bool ok = false;
    __try {
        memcpy(reinterpret_cast<void*>(addr), src, n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD restored = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), n, old_protect, &restored);
    return ok;
}

}  // namespace sora_console::safe_scan
