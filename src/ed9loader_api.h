// ED9Loader 插件 SDK —— 第三方插件唯一需要的头文件。
// 纯 C ABI,任何编译器/语言写的插件都能加载。
#pragma once

#define ED9LOADER_ABI_VERSION 6

#ifdef __cplusplus
extern "C" {
#endif

// 控制台命令回调:argv[0]=命令名,argv[1..]=参数。回调里用 console_print 回显结果。
typedef void (*Ed9CommandFn)(int argc, const char** argv);

// 框架提供给插件的接口表。追加字段只在末尾加、并提升 ED9LOADER_ABI_VERSION,
// 保证旧插件二进制兼容。插件用前先查 abi_version >= 自己需要的版本。
typedef struct Ed9Api {
    // ---- ABI v1 ----
    int    abi_version;                 // = 框架的 ED9LOADER_ABI_VERSION
    void (*log)(const char* msg);       // 写一行到 ED9Loader/loader.log
    void*  (*get_module_base)(void);    // 游戏主模块基址(配合 RVA 定位函数/数据)

    // ---- ABI v2: inline hook(MinHook 封装,BepInEx 的 Harmony 等价物)----
    int  (*install_hook)(void* target, void* detour, void** original);  // 0 成功

    // ---- ABI v3: 逆向能力封装(定位 + 安全内存读写)----
    void* (*find_vtable)(const char* type_fragment);                       // 按 RTTI 类名找 vtable VA
    int  (*safe_read)(const void* addr, void* out, unsigned long size);    // SEH 保护读,非0成功
    int  (*safe_write)(void* addr, const void* src, unsigned long size);   // SEH 保护写,非0成功

    // ---- ABI v4: 配置(INI,存 ED9Loader/config/<cfg_name>.ini, section [Settings])----
    int  (*cfg_get_int)(const char* cfg_name, const char* key, int def_value);
    int  (*cfg_get_str)(const char* cfg_name, const char* key, const char* def_value, char* out, int out_size);
    void (*cfg_set_int)(const char* cfg_name, const char* key, int value);
    void (*cfg_set_str)(const char* cfg_name, const char* key, const char* value);

    // ---- ABI v5: 单例实例定位 ----
    void* (*find_instance)(void* vtable);  // 扫主模块 .data 找首个 *P==vtable 的 P(单例实例)

    // ---- ABI v6: 控制台命令(BepInEx 的 console command 等价物)----
    // 注册一个控制台命令。玩家在控制台输入 name 即触发 fn。返回 0 成功。
    int  (*register_command)(const char* name, const char* help, Ed9CommandFn fn);
    // 在控制台输出(命令回调里回显结果用)。
    void (*console_print)(const char* msg);
} Ed9Api;

// 每个插件必须导出这个函数:
//   extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api);
typedef void (*Plugin_Load_t)(const Ed9Api* api);

#ifdef __cplusplus
}
#endif
