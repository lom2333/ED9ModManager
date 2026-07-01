// ED9Loader 资源重定向插件(hook FileStream::Open 自接管,构造 MemoryStream)。
// 复刻引擎缓存命中路径(FUN_1405264e0 内):
//   ms = engine_alloc(0x30); ms[0]=MemoryStream::vftable; ms[+0x10]=size; ms[+0x18]=data;
//   this[+0x20]=0(资源); this[+0x28]=ms(流); return MemoryStream::Open(ms,"",flags,0);
// data/ms 都用引擎分配器(匹配 close 时的 free)。原 scene.pac / pac 流程零改动。
// mod 文件: <游戏目录>/ED9Loader/redirect/<pac相对名>   例: redirect/scene/mp4000_sys.json
// 关键 RVA(ImageBase 0x140000000):
//   FileStream::Open 0x5264e0  MemoryStream::vftable 0x9daa20
//   engine_alloc 0x4a5050      MemoryManager 单例ptr 0xad4f10
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

// 当前游戏语言(""未知,"sc"/"tc"/"kr")。从引擎语言替换后的查找键嗅探(见 hk_FindFile)。
// 多语言 tbl:引擎对 Open 传语言无关的 table/X.tbl,真正的语言映射 table/→table_<lang>/ 发生在 Open 内部/
// FindFile 层(我们 Open hook 之后)。所以观察 FindFile 拿到的键即可得知当前语言,无需逆向语言全局。
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

static uint64_t __fastcall hk_FindFile(void* mgr, const char* name, uint64_t* off, uint64_t* sz) {
    detect_lang(name);                 // 只读嗅探,行为不变(passthrough),避免返回 0 触发崩溃
    return o_FindFile(mgr, name, off, sz);
}

// 引擎分配器(匹配引擎 free)
static void* eng_alloc(size_t n) {
    void** mmp = (void**)(g_base + 0xad4f10);
    void* mm = *mmp;
    if (mm) { Alloc_t a = (Alloc_t)(g_base + 0x4a5050); return a((char*)mm + 8, n); }
    return malloc(n);
}

// pac 相对名 -> redirect 散文件存在则读入引擎内存(返回 ptr,*out_size 填大小)
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

static void* __fastcall hk_Open(void* self, const char* name, unsigned p3, unsigned p4, unsigned short p5) {
    if (name) {
        uint32_t sz = 0;
        // 多语言:算两个候选 cache 名 —— 语言版(langLookup,g_lang 已知时)+ 语言中性(neuLookup)。
        //   table/X.tbl        → 语言 table_<lang>/X.tbl   中性 table/X.tbl(=原名)
        //   script/scena/X.dat → 语言 script_<lang>/X.dat  中性 script/X.dat(去掉引擎路径里的 scena 这层)
        //   其它(scene/asset)→ 无语言版,中性=原名。先查语言版,再查中性。
        char langBuf[300], neuBuf[300];
        const char* langLookup = nullptr;
        const char* neuLookup = name;
        if (strncmp(name, "table/", 6) == 0) {
            if (g_lang[0]) { _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "table_%s/%s", g_lang, name + 6); langLookup = langBuf; }
        } else if (strncmp(name, "script/scena/", 13) == 0) {
            const char* rest = name + 13;   // scena 下的文件名,如 "Test_point.dat"
            _snprintf_s(neuBuf, sizeof(neuBuf), _TRUNCATE, "script/%s", rest); neuLookup = neuBuf;       // 去 scena
            if (g_lang[0]) { _snprintf_s(langBuf, sizeof(langBuf), _TRUNCATE, "script_%s/%s", g_lang, rest); langLookup = langBuf; }
        }
        void* buf = nullptr;
        if (langLookup) { buf = load_redirect(langLookup, &sz); }
        if (!buf)       { buf = load_redirect(neuLookup,  &sz); }   // 回退语言中性
        if (buf) {
            char* ms = (char*)eng_alloc(0x30);
            memset(ms, 0, 0x30);
            *(void**)(ms + 0x00) = (void*)(g_base + 0x9daa20);  // MemoryStream::vftable
            *(uint64_t*)(ms + 0x10) = sz;                       // size
            *(void**)(ms + 0x18) = buf;                         // data
            *(void**)((char*)self + 0x20) = nullptr;            // 资源对象(close 不碰,因不设 +0x50 bit0)
            *(void**)((char*)self + 0x28) = ms;                 // 流
            void* vftbl = *(void**)ms;                                   // = MemoryStream::vftable
            MsOpen_t msopen = (MsOpen_t)(*(void**)((char*)vftbl + 0x28)); // *(vftable+0x28)
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
    api->install_hook((void*)(g_base + 0x5264e0), (void*)hk_Open, (void**)&o_Open);
    api->install_hook((void*)(g_base + 0x48b7b0), (void*)hk_FindFile, (void**)&o_FindFile);  // 只读嗅探语言
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
