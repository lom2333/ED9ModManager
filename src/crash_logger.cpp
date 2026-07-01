#include "crash_logger.h"

#include <Windows.h>
#include <DbgHelp.h>

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

std::string ExceptionCodeName(const DWORD code) {
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

    WriteMiniDumpFile(dump_path, exception);
    WriteCrashLogFile(log_path, dump_path, exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

std::filesystem::path GetLogDirectory() {
    return GetExeDirectory() / "ED9Loader" / "console_logs";
}

void Install() {
    std::call_once(g_install_once, []() {
        std::error_code error;
        std::filesystem::create_directories(GetLogDirectory(), error);
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
        SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    });
}

void TriggerTestCrash() {
    volatile int* crash = nullptr;
    *crash = 1;
}

}  // namespace sora_console::crash_logger
