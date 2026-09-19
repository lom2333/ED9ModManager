#include "crash_logger.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <MinHook.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace sora_console::crash_logger {
namespace {

std::once_flag g_install_once;

std::filesystem::path GetExeDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD count = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (count == 0 || count == MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buffer).parent_path();
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string TimestampForFileName() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &now_time);

    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y%m%d_%H%M%S");
    return stream.str();
}

std::string TimestampForLog() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &now_time);

    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

const char* ExceptionCodeName(const DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:
            return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW:
            return "EXCEPTION_STACK_OVERFLOW";
        default:
            return "UNKNOWN_EXCEPTION";
    }
}

void DescribeAddress(const void* address, char* out, size_t cap) {
    HMODULE module = nullptr;
    if (address != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) != 0 &&
        module != nullptr) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = (name != nullptr) ? name + 1 : path;
        _snprintf_s(out, cap, _TRUNCATE, "%ls+0x%llX", name,
                    static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(address) -
                        reinterpret_cast<uintptr_t>(module)));
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "0x%llX <不在任何已加载模块内>",
                    reinterpret_cast<unsigned long long>(address));
    }
}

int CaptureStack(const CONTEXT& start, void** frames, int cap) {
    CONTEXT ctx = start;
    int count = 0;
    while (count < cap && ctx.Rip != 0) {
        frames[count++] = reinterpret_cast<void*>(ctx.Rip);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (entry == nullptr) {
            DWORD64 ret = 0;
            __try {
                ret = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (ret == 0) {
                break;
            }
            ctx.Rip = ret;
            ctx.Rsp += 8;
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, entry, &ctx,
                         &handler_data, &establisher, nullptr);
    }
    return count;
}

wchar_t g_report_path[MAX_PATH] = {};

void CacheReportPath() {
    const std::wstring p = (GetLogDirectory() / L"crash_report.log").wstring();
    wcsncpy_s(g_report_path, p.c_str(), _TRUNCATE);
}

void StampNow(char* out, size_t cap) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    _snprintf_s(out, cap, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void AppendReport(const char* text) {
    if (g_report_path[0] == L'\0') {
        return;
    }
    const HANDLE file = CreateFileW(g_report_path, FILE_APPEND_DATA,
                                    FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(file);
}

void WriteCrashReport(const char* tag, EXCEPTION_POINTERS* exception) {
    if (exception == nullptr || exception->ExceptionRecord == nullptr) {
        return;
    }
    const EXCEPTION_RECORD& rec = *exception->ExceptionRecord;

    char buf[4096];
    char stamp[32];
    StampNow(stamp, sizeof(stamp));
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "\r\n======== %s  %s ========\r\n"
                        "exception   = 0x%08lX %s\r\n"
                        "thread      = %lu\r\n",
                        tag, stamp,
                        rec.ExceptionCode, ExceptionCodeName(rec.ExceptionCode),
                        GetCurrentThreadId());

    if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
        const ULONG_PTR kind = rec.ExceptionInformation[0];
        const ULONG_PTR addr = rec.ExceptionInformation[1];
        n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                         "访问违例    = %s 地址 0x%llX%s\r\n",
                         kind == 0 ? "读取" : (kind == 1 ? "写入" : "执行"),
                         static_cast<unsigned long long>(addr),
                         addr < 0x10000 ? "   ← 空指针解引用,这个数就是成员偏移" : "");
    }

    char desc[256];
    DescribeAddress(rec.ExceptionAddress, desc, sizeof(desc));
    n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "崩溃点      = %s\r\n", desc);

    if (exception->ContextRecord != nullptr) {
        const CONTEXT& c = *exception->ContextRecord;
        n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                         "寄存器      = RAX=%016llX RCX=%016llX RDX=%016llX\r\n"
                         "              R8 =%016llX R9 =%016llX RBX=%016llX\r\n"
                         "              RSP=%016llX RBP=%016llX RSI=%016llX RDI=%016llX\r\n",
                         c.Rax, c.Rcx, c.Rdx, c.R8, c.R9, c.Rbx,
                         c.Rsp, c.Rbp, c.Rsi, c.Rdi);

        void* frames[24] = {};
        const int count = CaptureStack(c, frames, 24);
        n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "调用栈:\r\n");
        for (int i = 0; i < count && n < static_cast<int>(sizeof(buf)) - 300; ++i) {
            DescribeAddress(frames[i], desc, sizeof(desc));
            n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                             "  #%-2d %s\r\n", i, desc);
        }
    }
    AppendReport(buf);
}

bool WriteMiniDumpFile(const std::filesystem::path& dump_path, EXCEPTION_POINTERS* exception) {
    HANDLE file = CreateFileW(
        dump_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception_info{};
    exception_info.ThreadId = GetCurrentThreadId();
    exception_info.ExceptionPointers = exception;
    exception_info.ClientPointers = FALSE;

    const MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo |
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData
    );

    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        dump_type,
        &exception_info,
        nullptr,
        nullptr
    );
    CloseHandle(file);
    return ok == TRUE;
}

void WriteCrashLogFile(
    const std::filesystem::path& log_path,
    const std::filesystem::path& dump_path,
    EXCEPTION_POINTERS* exception
) {
    std::ofstream file(log_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return;
    }

    const DWORD code = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionCode : 0;
    const void* address = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionAddress : nullptr;

    file << "time=" << TimestampForLog() << "\n";
    file << "process_id=" << GetCurrentProcessId() << "\n";
    file << "thread_id=" << GetCurrentThreadId() << "\n";
    file << "exception_code=0x" << std::hex << std::uppercase << code << std::dec << "\n";
    file << "exception_name=" << ExceptionCodeName(code) << "\n";
    file << "exception_address=" << address << "\n";
    file << "dump_path=" << dump_path.string() << "\n";
}

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exception) {
    std::error_code error;
    const auto directory = GetLogDirectory();
    std::filesystem::create_directories(directory, error);

    const std::string stamp = TimestampForFileName();
    const auto dump_path = directory / ("crash_" + stamp + ".dmp");
    const auto log_path = directory / ("crash_" + stamp + ".log");

    WriteCrashReport("[unhandled] 无人处理,进程即将结束", exception);
    WriteMiniDumpFile(dump_path, exception);
    WriteCrashLogFile(log_path, dump_path, exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool IsFatalCode(const DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

LONG CALLBACK VectoredCrashHandler(EXCEPTION_POINTERS* exception) {
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        !IsFatalCode(exception->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static uintptr_t seen[16] = {};
    static LONG seen_count = 0;
    const uintptr_t site = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    const LONG have = seen_count;
    for (LONG i = 0; i < have && i < 16; ++i) {
        if (seen[i] == site) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (have < 16) {
        seen[have] = site;
        InterlockedIncrement(&seen_count);
    }
    WriteCrashReport("[first-chance] 刚抛出,游戏可能自行处理", exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

}

std::filesystem::path GetLogDirectory() {
    return GetExeDirectory() / "ED9Loader" / "console_logs";
}

void Install() {
    std::call_once(g_install_once, []() {
        std::error_code error;
        std::filesystem::create_directories(GetLogDirectory(), error);
        CacheReportPath();
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
        SetUnhandledExceptionFilter(UnhandledExceptionHandler);
        AddVectoredExceptionHandler(1UL, VectoredCrashHandler);

        char head[256];
        char stamp[32];
        StampNow(stamp, sizeof(stamp));
        _snprintf_s(head, sizeof(head), _TRUNCATE,
                    "\r\n==== 崩溃记录启动 %s  pid=%lu ====\r\n",
                    stamp, GetCurrentProcessId());
        AppendReport(head);
    });
}

void NoteProcessExit() {
    void* frames[24] = {};
    const USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);

    char buf[3072];
    char stamp[32];
    StampNow(stamp, sizeof(stamp));
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "\r\n======== [进程退出] %s  pid=%lu ========\r\n"
                        "(正常关游戏也会有这一条;只有当它出现在一次「崩溃」之后、\r\n"
                        " 而上面又没有任何 exception 记录时,才说明游戏是主动退出的)\r\n"
                        "退出调用栈:\r\n",
                        stamp, GetCurrentProcessId());
    char desc[256];
    for (USHORT i = 0; i < count && n < static_cast<int>(sizeof(buf)) - 300; ++i) {
        DescribeAddress(frames[i], desc, sizeof(desc));
        n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "  #%-2d %s\r\n", i, desc);
    }
    AppendReport(buf);
}

namespace {
void NoteExitCall(const char* tag, unsigned code) {
    void* frames[24] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    char buf[3072], stamp[32], desc[256];
    StampNow(stamp, sizeof(stamp));
    int k = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "\r\n======== [%s] %s  pid=%lu  退出码=0x%X ========\r\n"
                        "调用栈(从这里往下就是喊停的人):\r\n",
                        tag, stamp, GetCurrentProcessId(), code);
    for (USHORT i = 0; i < n && k < static_cast<int>(sizeof(buf)) - 300; ++i) {
        DescribeAddress(frames[i], desc, sizeof(desc));
        k += _snprintf_s(buf + k, sizeof(buf) - k, _TRUNCATE, "  #%-2d %s\r\n", i, desc);
    }
    AppendReport(buf);
}

using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
using ExitProcess_t = VOID(WINAPI*)(UINT);
using RaiseFailFast_t = VOID(WINAPI*)(PEXCEPTION_RECORD, PCONTEXT, DWORD);
TerminateProcess_t o_TerminateProcess = nullptr;
ExitProcess_t o_ExitProcess = nullptr;
RaiseFailFast_t o_RaiseFailFast = nullptr;

BOOL WINAPI hk_TerminateProcess(HANDLE proc, UINT code) {
    if (proc == GetCurrentProcess() ||
        GetProcessId(proc) == GetCurrentProcessId()) {
        NoteExitCall("TerminateProcess 自杀", code);
    }
    return o_TerminateProcess(proc, code);
}

VOID WINAPI hk_ExitProcess(UINT code) {
    NoteExitCall("ExitProcess", code);
    o_ExitProcess(code);
}

VOID WINAPI hk_RaiseFailFast(PEXCEPTION_RECORD rec, PCONTEXT ctx, DWORD flags) {
    NoteExitCall("__fastfail", rec ? rec->ExceptionCode : 0);
    o_RaiseFailFast(rec, ctx, flags);
}

}

void InstallProcessExitHooks() {
    struct { const wchar_t* mod; const char* fn; void* detour; void** orig; } targets[] = {
        {L"kernel32.dll", "TerminateProcess", &hk_TerminateProcess, reinterpret_cast<void**>(&o_TerminateProcess)},
        {L"kernel32.dll", "ExitProcess", &hk_ExitProcess, reinterpret_cast<void**>(&o_ExitProcess)},
        {L"kernel32.dll", "RaiseFailFastException", &hk_RaiseFailFast, reinterpret_cast<void**>(&o_RaiseFailFast)},
    };
    char buf[256];
    for (const auto& t : targets) {
        const HMODULE h = GetModuleHandleW(t.mod);
        void* addr = h ? reinterpret_cast<void*>(GetProcAddress(h, t.fn)) : nullptr;
        MH_STATUS st = MH_UNKNOWN;
        if (addr != nullptr) {
            st = MH_CreateHook(addr, t.detour, t.orig);
            if (st == MH_OK) {
                st = MH_EnableHook(addr);
            }
        }
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "==== 退出拦截 %-24s %s ====\r\n", t.fn,
                    (addr && st == MH_OK) ? "已装" : "装载失败");
        AppendReport(buf);
    }
}

void TriggerTestCrash() {
    volatile int* crash = nullptr;
    *crash = 1;
}

}
