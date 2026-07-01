#pragma once

namespace ed9loader::plugin_loader {

// 扫描游戏目录下 ED9Loader\plugins\*.dll,逐个 LoadLibrary 并调用其 Plugin_Load 导出。
// 幂等:多次调用只执行一次。任何单个插件失败都被隔离、不影响游戏与其它插件。
void LoadAll();

}  // namespace ed9loader::plugin_loader
