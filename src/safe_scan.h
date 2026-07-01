#pragma once

#include <cstddef>
#include <cstdint>

namespace sora_console::safe_scan {

struct ScanHit {
    std::uintptr_t address = 0;  // 命中地址（其首 8 字节等于某个 vtable）
    std::uintptr_t vtable = 0;   // 命中的 vtable 值
};

// 在 [begin,end) 内逐 8 字节扫描，找首 8 字节等于 vtables[] 中任意值的地址。
// 全程 SEH 保护：若区域中途被释放/改保护属性导致访问违例，捕获并返回已收集结果，
// 绝不让访问违例传播为崩溃。返回写入 out 的命中数（上限 out_cap）。
int SafeScanRegionForVtables(std::uintptr_t begin, std::uintptr_t end,
                             const std::uintptr_t* vtables, int vtable_count,
                             ScanHit* out, int out_cap);

// SEH 保护的内存拷贝读取。成功返回 true；访问违例返回 false（dst 内容未定义）。
bool SafeReadBytes(std::uintptr_t addr, void* dst, std::size_t n);

// SEH 保护的内存写入(自动 VirtualProtect 临时改可写再恢复)。成功返回 true。
bool SafeWriteBytes(std::uintptr_t addr, const void* src, std::size_t n);

}  // namespace sora_console::safe_scan
