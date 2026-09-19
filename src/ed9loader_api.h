#pragma once

#define ED9LOADER_ABI_VERSION 7

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Ed9CommandFn)(int argc, const char** argv);

typedef struct Ed9Api {
    int    abi_version;
    void (*log)(const char* msg);
    void*  (*get_module_base)(void);

    int  (*install_hook)(void* target, void* detour, void** original);

    void* (*find_vtable)(const char* type_fragment);
    int  (*safe_read)(const void* addr, void* out, unsigned long size);
    int  (*safe_write)(void* addr, const void* src, unsigned long size);

    int  (*cfg_get_int)(const char* cfg_name, const char* key, int def_value);
    int  (*cfg_get_str)(const char* cfg_name, const char* key, const char* def_value, char* out, int out_size);
    void (*cfg_set_int)(const char* cfg_name, const char* key, int value);
    void (*cfg_set_str)(const char* cfg_name, const char* key, const char* value);

    void* (*find_instance)(void* vtable);

    int  (*register_command)(const char* name, const char* help, Ed9CommandFn fn);
    void (*console_print)(const char* msg);

    void* (*resolve_symbol)(const char* name);
} Ed9Api;

typedef void (*Plugin_Load_t)(const Ed9Api* api);

#ifdef __cplusplus
}
#endif
