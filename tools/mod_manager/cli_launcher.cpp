#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>

int wmain() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    size_t slash = dir.find_last_of(L"\\/");
    std::wstring target = (slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1)) + L"ED9ModManager.exe";

    std::wstring cl = GetCommandLineW();
    size_t i = 0;
    if (!cl.empty() && cl[0] == L'"') { size_t q = cl.find(L'"', 1); i = (q == std::wstring::npos ? cl.size() : q + 1); }
    else { size_t sp = cl.find(L' '); i = (sp == std::wstring::npos ? cl.size() : sp); }
    std::wstring rest = (i < cl.size() ? cl.substr(i) : std::wstring());
    std::wstring full = L"\"" + target + L"\"" + rest;

    STARTUPINFOW si{}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(full.begin(), full.end()); buf.push_back(0);

    if (!CreateProcessW(target.c_str(), buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        fwprintf(stderr, L"[CLI壳] 无法启动 %ls (err=%lu)\n", target.c_str(), GetLastError());
        return 3;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
