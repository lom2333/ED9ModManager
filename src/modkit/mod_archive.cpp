#include "modkit/mod_archive.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <set>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "miniz.h"

namespace fs = std::filesystem;

namespace {
std::string wtou8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring u8tow(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

const wchar_t* const kZipExts[] = { L".zip", L".ed9mod" };
const wchar_t* const kExtExts[] = {
    L".7z", L".rar", L".tar", L".tgz", L".txz", L".tbz2", L".tbz", L".cab", L".iso",
    L".gz", L".xz", L".bz2", L".zst", L".lzh", L".lha",
};

bool hasExt(const std::wstring& lowerName, const wchar_t* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const std::wstring e = list[i];
        if (lowerName.size() > e.size() &&
            lowerName.compare(lowerName.size() - e.size(), e.size(), e) == 0) return true;
    }
    return false;
}

bool readWholeFileW(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    size_t off = 0;
    bool ok = true;
    while (off < out.size()) {
        DWORD want = (DWORD)((out.size() - off) > (1u << 20) ? (1u << 20) : (out.size() - off));
        DWORD rd = 0;
        if (!ReadFile(h, out.data() + off, want, &rd, nullptr) || rd == 0) { ok = false; break; }
        off += rd;
    }
    CloseHandle(h);
    return ok && off == out.size();
}

std::string stampOf(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &fad)) return {};
    unsigned long long size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    unsigned long long time = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)
                              | fad.ftLastWriteTime.dwLowDateTime;
    return std::to_string(size) + "|" + std::to_string(time);
}

std::string readTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string sanitizeEntry(const char* raw) {
    std::string name(raw ? raw : "");
    for (auto& c : name) if (c == '\\') c = '/';
    if (name.empty()) return {};
    if (name[0] == '/') return {};
    if (name.size() >= 2 && name[1] == ':') return {};
    std::string out;
    size_t i = 0;
    while (i < name.size()) {
        size_t j = name.find('/', i);
        std::string seg = name.substr(i, (j == std::string::npos ? name.size() : j) - i);
        if (seg == "..") return {};
        if (!seg.empty() && seg != ".") {
            if (!out.empty()) out += '/';
            out += seg;
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

void ensureDirW(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
}

bool writeFileW(const fs::path& target, const void* data, size_t size) {
    ensureDirW(target.parent_path());
    HANDLE h = CreateFileW(target.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t off = 0;
    const unsigned char* p = (const unsigned char*)data;
    while (off < size) {
        DWORD want = (DWORD)((size - off) > (1u << 20) ? (1u << 20) : (size - off));
        DWORD wr = 0;
        if (!WriteFile(h, p + off, want, &wr, nullptr) || wr == 0) { ok = false; break; }
        off += wr;
    }
    CloseHandle(h);
    return ok;
}

std::wstring findSystemTar() {
    wchar_t sys[MAX_PATH] = {};
    if (GetSystemDirectoryW(sys, MAX_PATH) == 0) return {};
    std::wstring p = std::wstring(sys) + L"\\tar.exe";
    return (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) ? p : std::wstring();
}

std::wstring findSevenZip() {
    const wchar_t* cands[] = {
        L"C:\\Program Files\\7-Zip\\7z.exe",
        L"C:\\Program Files (x86)\\7-Zip\\7z.exe",
    };
    for (const wchar_t* c : cands)
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
    wchar_t buf[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"7z.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    return {};
}

int runSilent(const std::wstring& cmdline, const fs::path& workDir) {
    std::wstring cl = cmdline;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const std::wstring wd = workDir.wstring();
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, wd.empty() ? nullptr : wd.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

std::wstring quoteArg(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

bool extractExternal(const fs::path& archivePath, const fs::path& outDir, std::string& err) {
    const std::wstring tar = findSystemTar();
    if (!tar.empty()) {
        const std::wstring cl = quoteArg(tar) + L" -xf " + quoteArg(archivePath) + L" -C " + quoteArg(outDir);
        if (runSilent(cl, outDir) == 0) return true;
        err = "系统 tar.exe 解压失败";
    } else {
        err = "系统没有 tar.exe(需要 Win10 1803 以上)";
    }
    const std::wstring z7 = findSevenZip();
    if (!z7.empty()) {
        const std::wstring cl = quoteArg(z7) + L" x " + quoteArg(archivePath) + L" -o\"" + outDir.wstring() + L"\" -y";
        if (runSilent(cl, outDir) == 0) { err.clear(); return true; }
        err += ";7z.exe 也失败";
    } else if (!tar.empty()) {
        err += "(未装 7-Zip,无法兜底)";
    }
    return false;
}

bool extractZip(const fs::path& archivePath, const fs::path& outDir, std::string& err) {
    std::vector<unsigned char> data;
    if (!readWholeFileW(archivePath.wstring(), data) || data.empty()) { err = "读取压缩包失败"; return false; }
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) { err = "不是有效的 zip"; return false; }
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    int written = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        const std::string rel = sanitizeEntry(st.m_filename);
        if (rel.empty()) continue;
        size_t usize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &usize, 0);
        if (!p) continue;
        std::wstring w = u8tow(rel);
        for (auto& c : w) if (c == L'/') c = L'\\';
        if (writeFileW(outDir / w, p, usize)) ++written;
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    if (written == 0) { err = "解压后没有任何文件"; return false; }
    return true;
}

void stripShellDir(const fs::path& outDir) {
    std::error_code ec;
    std::vector<fs::path> dirs, files;
    for (const auto& e : fs::directory_iterator(outDir, ec)) {
        if (e.path().filename() == L".ed9stamp") continue;
        if (e.is_directory(ec)) dirs.push_back(e.path());
        else files.push_back(e.path());
    }
    if (dirs.size() != 1 || !files.empty()) return;
    const fs::path shell = dirs[0];
    for (const auto& e : fs::directory_iterator(shell, ec)) {
        fs::path dst = outDir / e.path().filename();
        fs::rename(e.path(), dst, ec);
        if (ec) { ec.clear(); return; }
    }
    fs::remove_all(shell, ec);
}

}

namespace ed9loader {
namespace modkit {
namespace archive {

bool IsArchiveFile(const fs::path& p) {
    const std::wstring name = lowerW(p.filename().wstring());
    if (hasExt(name, kZipExts, sizeof(kZipExts) / sizeof(*kZipExts))) return true;
    return hasExt(name, kExtExts, sizeof(kExtExts) / sizeof(*kExtExts));
}

std::string ArchiveModName(const fs::path& p) {
    std::wstring stem = p.stem().wstring();
    const std::wstring low = lowerW(stem);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, L".tar") == 0)
        stem = stem.substr(0, stem.size() - 4);
    return wtou8(stem);
}

fs::path StagingRoot(const fs::path& modsDir) {
    fs::path game = fs::path(modsDir).parent_path();
    return game / L"ED9Loader" / L"cache" / L"mods";
}

fs::path FindArchive(const fs::path& modsDir, const std::wstring& name) {
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return {};
    const std::string want = wtou8(name);
    for (const auto& e : fs::directory_iterator(modsDir, ec)) {
        if (!e.is_regular_file(ec) || !IsArchiveFile(e.path())) continue;
        if (ArchiveModName(e.path()) == want) return e.path();
    }
    return {};
}

std::vector<std::string> ListArchiveMods(const fs::path& modsDir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return out;
    for (const auto& e : fs::directory_iterator(modsDir, ec)) {
        if (!e.is_regular_file(ec) || !IsArchiveFile(e.path())) continue;
        std::string n = ArchiveModName(e.path());
        if (!n.empty()) out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

fs::path EnsureStaged(const fs::path& archivePath, const fs::path& stagingRoot,
                      std::string& err, bool* changed) {
    if (changed) *changed = false;
    std::error_code ec;
    if (!fs::is_regular_file(archivePath, ec)) { err = "找不到压缩包"; return {}; }

    const fs::path out = stagingRoot / u8tow(ArchiveModName(archivePath));
    const fs::path stampFile = out / L".ed9stamp";
    const std::string want = stampOf(archivePath);
    if (!want.empty() && readTextFile(stampFile) == want) return out;

    fs::remove_all(out, ec);
    ensureDirW(out);

    const std::wstring name = lowerW(archivePath.filename().wstring());
    const bool isZip = hasExt(name, kZipExts, sizeof(kZipExts) / sizeof(*kZipExts));
    const bool ok = isZip ? extractZip(archivePath, out, err)
                          : extractExternal(archivePath, out, err);
    if (!ok) { fs::remove_all(out, ec); return {}; }

    stripShellDir(out);

    bool any = false;
    for (auto it = fs::recursive_directory_iterator(out, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) { any = true; break; }
    }
    if (!any) { err = "解压后没有任何文件"; fs::remove_all(out, ec); return {}; }

    if (!want.empty()) writeFileW(stampFile, want.data(), want.size());
    if (changed) *changed = true;
    return out;
}

int EnsureAllStaged(const fs::path& modsDir, std::string& log) {
    int ok = 0;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return 0;
    const fs::path root = StagingRoot(modsDir);

    std::set<std::wstring> live;
    bool announced = false;
    for (const auto& e : fs::directory_iterator(modsDir, ec)) {
        if (!e.is_regular_file(ec) || !IsArchiveFile(e.path())) continue;
        if (!announced) { log += "[modkit] archive backends: " + BackendSummary() + "\n"; announced = true; }
        const std::string mod = ArchiveModName(e.path());
        const std::wstring wmod = u8tow(mod);
        const std::string fileName = wtou8(e.path().filename().wstring());
        if (fs::is_directory(modsDir / wmod, ec)) {
            log += "[modkit] archive " + fileName + " skipped (同名文件夹存在,以文件夹为准)\n";
            continue;
        }
        live.insert(wmod);
        std::string err;
        bool changed = false;
        fs::path staged = EnsureStaged(e.path(), root, err, &changed);
        if (staged.empty()) {
            log += "[modkit] archive " + fileName + " FAILED: " + err + "\n";
            continue;
        }
        ++ok;
        if (changed) log += "[modkit] archive " + fileName + " extracted\n";
    }

    if (fs::is_directory(root, ec)) {
        std::vector<fs::path> dead;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory(ec)) continue;
            if (!live.count(e.path().filename().wstring())) dead.push_back(e.path());
        }
        for (const auto& d : dead) {
            fs::remove_all(d, ec);
            log += "[modkit] archive cache purged: " + wtou8(d.filename().wstring()) + "\n";
        }
    }
    return ok;
}

std::string BackendSummary() {
    std::string s = "内置 zip";
    if (!findSystemTar().empty()) s += " + 系统 tar.exe(7z/rar/tar/cab…)";
    if (!findSevenZip().empty()) s += " + 7-Zip";
    return s;
}

}
}
}
