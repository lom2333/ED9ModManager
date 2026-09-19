#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static const Ed9Api* g_api = nullptr;
static std::wstring g_redirect_root;
static uintptr_t g_base = 0;

typedef void* (__fastcall* Open_t)(void* self, const char* name, unsigned p3, unsigned p4, unsigned short p5);
typedef void* (__fastcall* MsOpen_t)(void* ms, const char* s, unsigned p3, unsigned p4);
typedef void* (__fastcall* Alloc_t)(void* heap, size_t n);
typedef uint64_t (__fastcall* FindFile_t)(void* mgr, const char* name, uint64_t* off, uint64_t* sz);
static Open_t o_Open = nullptr;
static FindFile_t o_FindFile = nullptr;

static char g_lang[8] = {};

static void detect_lang(const char* name) {
    if (g_lang[0] != 0 || !name) return;
    const char* l = nullptr;
    if      (strstr(name, "_sc/")) l = "sc";
    else if (strstr(name, "_tc/")) l = "tc";
    else if (strstr(name, "_kr/")) l = "kr";
    if (l) {
        g_lang[0] = l[0]; g_lang[1] = l[1]; g_lang[2] = 0;
    }
}

static char g_trace[64] = {};
static wchar_t g_trace_path[MAX_PATH] = {};

static void TraceReq(const char* tag, const char* name, const char* result) {
    if (g_trace[0] == 0 || name == nullptr || strstr(name, g_trace) == nullptr) return;
    char line[512];
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%8u] %-8s %s%s\r\n",
                              GetTickCount(), tag, name, result ? result : "");
    const HANDLE f = CreateFileW(g_trace_path, FILE_APPEND_DATA, FILE_SHARE_READ,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, line, (DWORD)n, &w, nullptr);
    CloseHandle(f);
}

static uint64_t __fastcall hk_FindFile(void* mgr, const char* name, uint64_t* off, uint64_t* sz) {
    detect_lang(name);
    const uint64_t r = o_FindFile(mgr, name, off, sz);
    TraceReq("FindFile", name, r ? "  -> 命中 pac" : "  -> pac 里没有");
    return r;
}

static void*    g_ms_vftable   = nullptr;
static void*    g_memmgr       = nullptr;
static Alloc_t  g_engine_alloc = nullptr;

static void* eng_alloc(size_t n) {
    if (g_engine_alloc && g_memmgr) return g_engine_alloc((char*)g_memmgr + 8, n);
    return malloc(n);
}

static void* load_redirect(const char* relname, uint32_t* out_size) {
    std::wstring p = g_redirect_root;
    for (const char* c = relname; *c; ++c) p += (*c == '/') ? L'\\' : (wchar_t)*c;
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    DWORD sz = GetFileSize(h, nullptr);
    void* buf = eng_alloc(sz);
    DWORD rd = 0;
    BOOL ok = buf && ReadFile(h, buf, sz, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd != sz) return nullptr;
    *out_size = sz;
    return buf;
}

static const int kLogMax = 64;
static char g_logged[kLogMax][160];
static int  g_loggedN = 0;
static void LogHit(const char* name, uint32_t sz) {
    if (g_api == nullptr || g_api->log == nullptr || name == nullptr) return;
    for (int i = 0; i < g_loggedN; ++i) if (strcmp(g_logged[i], name) == 0) return;
    if (g_loggedN >= kLogMax) return;
    strncpy_s(g_logged[g_loggedN++], name, _TRUNCATE);
    char b[224];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "[SceneRedirect] 命中 %s (%u 字节)", name, sz);
    g_api->log(b);
}

static void* __fastcall hk_Open(void* self, const char* name, unsigned p3, unsigned p4, unsigned short p5) {
    TraceReq("Open", name, nullptr);
    if (name) {
        uint32_t sz = 0;
        char langBuf[300], neuBuf[300];
        const char* langLookup = nullptr;
        const char* neuLookup = name;
        if (strncmp(name, "table/", 6) == 0) {
            if (g_lang[0]) { _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "table_%s/%s", g_lang, name + 6); langLookup = langBuf; }
        } else if (strncmp(name, "script/scena/", 13) == 0) {
            const char* rest = name + 13;
            _snprintf_s(neuBuf, sizeof(neuBuf), _TRUNCATE, "script/%s", rest); neuLookup = neuBuf;
            if (g_lang[0]) { _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "script_%s/%s", g_lang, rest); langLookup = langBuf; }
        }
        void* buf = nullptr;
        const char* hitName = langLookup;
        if (langLookup) { buf = load_redirect(langLookup, &sz); }
        if (!buf)       { buf = load_redirect(neuLookup,  &sz); hitName = neuLookup; }
        if (buf) {
            LogHit(hitName, sz);
            char* ms = (char*)eng_alloc(0x30);
            memset(ms, 0, 0x30);
            *(void**)(ms + 0x00) = g_ms_vftable;
            *(uint64_t*)(ms + 0x10) = sz;
            *(void**)(ms + 0x18) = buf;
            *(void**)((char*)self + 0x20) = nullptr;
            *(void**)((char*)self + 0x28) = ms;
            void* vftbl = *(void**)ms;
            MsOpen_t msopen = (MsOpen_t)(*(void**)((char*)vftbl + 0x28));
            return msopen(ms, "", p3, 0);
        }
    }
    return o_Open(self, name, p3, p4, p5);
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) return;
    g_api = api;
    if (api->abi_version < 2 || api->install_hook == nullptr || api->get_module_base == nullptr) {
        return;
    }
    g_base = (uintptr_t)api->get_module_base();
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* sl = wcsrchr(exe, L'\\'); if (sl) *sl = 0;
    g_redirect_root = std::wstring(exe) + L"\\ED9Loader\\cache\\merged\\";

    {
        wchar_t ini[MAX_PATH];
        _snwprintf_s(ini, MAX_PATH, _TRUNCATE, L"%s\\ED9Loader\\config\\SceneRedirect.ini", exe);
        wchar_t wtrace[64] = {};
        GetPrivateProfileStringW(L"Settings", L"trace", L"", wtrace, 64, ini);
        WideCharToMultiByte(CP_UTF8, 0, wtrace, -1, g_trace, sizeof(g_trace), nullptr, nullptr);
        if (g_trace[0] != 0) {
            _snwprintf_s(g_trace_path, MAX_PATH, _TRUNCATE,
                         L"%s\\ED9Loader\\console_logs\\resource_trace.log", exe);
            char b[192];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "[SceneRedirect] 资源请求跟踪已开启(过滤子串 \"%s\")"
                        " -> console_logs\\resource_trace.log", g_trace);
            api->log(b);
        }
    }

    if (api->abi_version < 7 || api->resolve_symbol == nullptr) {
        api->log("[SceneRedirect] 需要 ED9Loader ABI v7(符号表),已禁用");
        return;
    }
    g_ms_vftable = (api->find_vtable != nullptr) ? api->find_vtable("MemoryStream@fdk") : nullptr;
    void* mm_vt  = (api->find_vtable != nullptr) ? api->find_vtable("MemoryManager@fdk") : nullptr;
    g_memmgr     = (mm_vt != nullptr && api->find_instance != nullptr) ? api->find_instance(mm_vt) : nullptr;
    void* open     = api->resolve_symbol("FileStream_Open");
    void* findfile = api->resolve_symbol("FindFile");
    g_engine_alloc = (Alloc_t)api->resolve_symbol("engine_alloc");

    if (g_ms_vftable == nullptr || g_memmgr == nullptr || open == nullptr ||
        findfile == nullptr || g_engine_alloc == nullptr) {
        char b[256];
        _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "[SceneRedirect] 符号解析不全,已禁用(vft=%d memmgr=%d open=%d findfile=%d alloc=%d)"
                    " —— 游戏更新过?请在管理器里重新锚定。",
                    g_ms_vftable != nullptr, g_memmgr != nullptr, open != nullptr,
                    findfile != nullptr, g_engine_alloc != nullptr);
        api->log(b);
        return;
    }
    api->install_hook(open,     (void*)hk_Open,     (void**)&o_Open);
    api->install_hook(findfile, (void*)hk_FindFile, (void**)&o_FindFile);
    api->log("[SceneRedirect] 已按当前游戏版本自适应装载");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
