#pragma once

#include <cstdint>
#include <string_view>

// 框架的定位能力:RTTI 类名→vtable、扫 .data 找单例实例。
// 供 plugin_loader 暴露成 Ed9Api 的 find_vtable / find_instance。坐标等具体功能已移到插件。
namespace sora_console::runtime_locator {

// 按 RTTI 类名片段找 vtable 运行时 VA(精确类型 .?AV<片段>@@ 优先)。找不到返回 0。
std::uintptr_t FindVtableByType(std::string_view fragment);

// 扫主模块 .data 找首个"首8字节==vtable_va"的对象指针(单例实例)。找不到返回 0。
std::uintptr_t FindInstanceByVtable(std::uintptr_t vtable_va);

}  // namespace sora_console::runtime_locator
