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

typedef uint32_t (__fastcall* Rewrite_t)(void* fpm, void* out, uint16_t* ioFlags, const char* src, bool backslashToSlash);
static Rewrite_t g_rewrite = nullptr;
static void**    g_ppFpm   = nullptr;
static uint8_t** g_ppSave  = nullptr;
static uint32_t  g_langOff = 0;
static bool      g_koIsKo  = true;
static volatile LONG g_sniff = 0;

static bool IsLangCode(const char* s, size_t n) {
    static const char* const k[] = { "sc", "tc", "ko", "kr", "en", "de", "fr", "es" };
    for (const char* c : k) if (n == 2 && s[0] == c[0] && s[1] == c[1]) return true;
    return false;
}

static bool CodeFromEngineName(const char* n, char code[4]) {
    const char* rest = nullptr;
    if (strncmp(n, "table", 5) == 0) rest = n + 5;
    else if (strncmp(n, "script", 6) == 0) rest = n + 6;
    else return false;
    if (rest[0] == '/') { code[0] = 'j'; code[1] = 'p'; code[2] = 0; return true; }
    if (rest[0] != '_') return false;
    const char* slash = strchr(rest + 1, '/');
    if (slash == nullptr || !IsLangCode(rest + 1, (size_t)(slash - rest - 1))) return false;
    code[0] = rest[1]; code[1] = rest[2]; code[2] = 0;
    return true;
}

static bool CurrentLang(const char* name, unsigned mode, unsigned flags, unsigned short p5, char code[4]) {
    if (g_rewrite != nullptr && g_ppFpm != nullptr) {
        void* fpm = *g_ppFpm;
        if (fpm == nullptr || (flags & 4) != 0 || mode == 1) { code[0] = 'j'; code[1] = 'p'; code[2] = 0; return true; }
        struct { char s[0x200]; uint32_t len; } out = {};
        uint16_t f = p5;
        g_rewrite(fpm, &out, &f, name, false);
        out.s[sizeof(out.s) - 1] = 0;
        if (CodeFromEngineName(out.s, code)) return true;
    }
    if (g_ppSave != nullptr && *g_ppSave != nullptr) {
        static const char* const kCodes[8] = { "jp", "en", "de", "fr", "es", "tc", "sc", "ko" };
        uint8_t idx = 0;
        if (g_api->safe_read(*g_ppSave + g_langOff, &idx, 1) == 1 && idx < 8) {
            const char* c = (idx == 7 && !g_koIsKo) ? "kr" : kCodes[idx];
            code[0] = c[0]; code[1] = c[1]; code[2] = 0;
            return true;
        }
    }
    const LONG s = g_sniff;
    if (s != 0) { memcpy(code, &s, 4); code[3] = 0; return true; }
    return false;
}

static void SniffLang(const char* name) {
    if (g_sniff != 0 || name == nullptr || (g_rewrite != nullptr && g_ppFpm != nullptr) || g_ppSave != nullptr) return;
    char code[4] = {};
    if (CodeFromEngineName(name, code) && !(code[0] == 'j' && code[1] == 'p')) {
        LONG v = 0; memcpy(&v, code, 4);
        InterlockedExchange(&g_sniff, v);
    }
}

static uintptr_t FindUnique(uintptr_t from, uintptr_t to, const int16_t* pat, size_t n) {
    uintptr_t hit = 0;
    int count = 0;
    for (uintptr_t p = from; p + n <= to; ++p) {
        const uint8_t* q = reinterpret_cast<const uint8_t*>(p);
        if (q[0] != (uint8_t)pat[0]) continue;
        size_t i = 1;
        for (; i < n; ++i) if (pat[i] >= 0 && q[i] != (uint8_t)pat[i]) break;
        if (i == n) { hit = p; if (++count > 1) return 0; }
    }
    return count == 1 ? hit : 0;
}
static bool TextRange(uintptr_t base, uintptr_t& beg, uintptr_t& end) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && s->Misc.VirtualSize != 0) {
            beg = base + s->VirtualAddress; end = beg + s->Misc.VirtualSize; return true;
        }
    return false;
}

static void ResolveLangSources(void* open) {
    static const int16_t kRw[] = { 0x48, 0x8B, 0x0D, -1, -1, -1, -1, 0x48, 0x85, 0xC9, 0x74, -1, 0xC6, 0x44, 0x24, 0x20, 0x00,
                                   0x4C, 0x8D, 0x8D, -1, -1, -1, -1, 0x4C, 0x8D, 0x85, -1, -1, -1, -1, 0x48, 0x8D, 0x54, 0x24, 0x40, 0xE8 };
    const uintptr_t o = reinterpret_cast<uintptr_t>(open);
    if (const uintptr_t h = FindUnique(o, o + 0x400, kRw, sizeof(kRw) / sizeof(kRw[0]))) {
        int32_t d1 = 0, d2 = 0;
        memcpy(&d1, reinterpret_cast<const void*>(h + 3), 4);
        memcpy(&d2, reinterpret_cast<const void*>(h + 0x25), 4);
        g_ppFpm = reinterpret_cast<void**>(h + 7 + d1);
        g_rewrite = reinterpret_cast<Rewrite_t>(h + 0x29 + d2);
    }
    static const int16_t kLang[] = { 0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x0F, 0xB6, 0x90, -1, -1, -1, -1, 0x0F, 0xB6, 0x98, -1, -1, -1, -1 };
    uintptr_t beg = 0, end = 0;
    if (TextRange(g_base, beg, end)) {
        if (const uintptr_t h = FindUnique(beg, end, kLang, sizeof(kLang) / sizeof(kLang[0]))) {
            int32_t d = 0; uint32_t off = 0;
            memcpy(&d, reinterpret_cast<const void*>(h + 3), 4);
            memcpy(&off, reinterpret_cast<const void*>(h + 10), 4);
            g_ppSave = reinterpret_cast<uint8_t**>(h + 7 + d);
            g_langOff = off;
        }
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
    SniffLang(name);
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
        const bool isTable = strncmp(name, "table/", 6) == 0;
        const bool isScena = strncmp(name, "script/scena/", 13) == 0;
        const bool isAi    = strncmp(name, "script/ai/", 10) == 0;
        const bool isAni   = strncmp(name, "script/ani/", 11) == 0;
        char lang[4] = {};
        if ((isTable || isScena || isAi || isAni) && CurrentLang(name, p3, p4, p5, lang)) {
            if (isTable)      _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "table_%s/%s", lang, name + 6);
            else if (isScena) _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "script_%s/%s", lang, name + 13);
            else if (isAi)    _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "script_%s/ai/%s", lang, name + 10);
            else              _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "script_%s/ani/%s", lang, name + 11);
            langLookup = langBuf;
        }
        if (isScena) { _snprintf_s(neuBuf, sizeof(neuBuf), _TRUNCATE, "script/%s", name + 13); neuLookup = neuBuf; }
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
    ResolveLangSources(open);
    { wchar_t ko[MAX_PATH];
      _snwprintf_s(ko, MAX_PATH, _TRUNCATE, L"%s\\pac\\steam\\table_ko.pac", exe);
      g_koIsKo = GetFileAttributesW(ko) != INVALID_FILE_ATTRIBUTES; }
    if (g_rewrite == nullptr)
        api->log(g_ppSave != nullptr ? "[SceneRedirect] 没找到游戏的路径改写函数,当前语言改为读游戏设置"
                                     : "[SceneRedirect] 没找到游戏的路径改写函数和语言设置,当前语言改为从读到的文件名推断");
    api->install_hook(open,     (void*)hk_Open,     (void**)&o_Open);
    api->install_hook(findfile, (void*)hk_FindFile, (void**)&o_FindFile);
    api->log("[SceneRedirect] 已按当前游戏版本自适应装载");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
