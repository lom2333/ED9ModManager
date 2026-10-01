#include "symbol_table.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "rva_tables_embedded.h"

namespace sora_console::symbol_table {
namespace {

std::map<std::string, std::uintptr_t> g_syms;
std::string g_status = "未加载";
bool g_loaded = false;

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
}

std::string ExeStem() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).stem().string();
}

bool ExeIdentity(std::uint64_t& size, std::uint32_t& timestamp) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::error_code ec;
    const auto sz = std::filesystem::file_size(buf, ec);
    if (ec) return false;
    size = sz;
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) return false;
    const auto lfanew = *reinterpret_cast<const std::int32_t*>(base + 0x3C);
    if (lfanew <= 0 || lfanew > 0x1000) return false;
    timestamp = *reinterpret_cast<const std::uint32_t*>(base + lfanew + 8);
    return true;
}

struct Sym {
    std::string name;
    std::uint32_t rva = 0;
    std::vector<std::uint8_t> val;
    std::vector<std::uint8_t> care;
};

struct Table {
    std::string label;
    bool sameExe = false;
    int skipped = 0;
    std::vector<Sym> syms;
    bool HasSig() const {
        for (const auto& s : syms) {
            if (!s.val.empty()) return true;
        }
        return false;
    }
};

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void ParseSig(const std::string& s, Sym& out) {
    if (s.empty() || s.size() % 2 != 0) return;
    std::vector<std::uint8_t> val(s.size() / 2), care(s.size() / 2);
    for (std::size_t i = 0; i < val.size(); ++i) {
        const char a = s[2 * i], b = s[2 * i + 1];
        if (a == '?' && b == '?') continue;
        const int hi = HexNibble(a), lo = HexNibble(b);
        if (hi < 0 || lo < 0) return;
        val[i] = static_cast<std::uint8_t>(hi * 16 + lo);
        care[i] = 1;
    }
    out.val.swap(val);
    out.care.swap(care);
}

bool Parse(const char* text, std::size_t len, std::uint64_t size, std::uint32_t ts, Table& t, std::string& why) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(text, text + len); }
    catch (...) { why = "解析失败"; return false; }
    const auto& s = j["symbols"];
    if (!s.is_object()) { why = "无 symbols 段"; return false; }
    const auto& e = j["exe"];
    t.sameExe = e.is_object() && e.value("size", 0ull) == size && e.value("timestamp", 0u) == ts;
    for (auto it = s.begin(); it != s.end(); ++it) {
        const auto& v = it.value();
        if (!v.is_object()) continue;
        const std::string conf = v.value("confidence", std::string("low"));
        const std::uint64_t rva = v.value("rva", 0ull);
        if (rva == 0 || rva > 0xFFFFFFFFull || (conf != "reference" && conf != "high")) { ++t.skipped; continue; }
        Sym sym;
        sym.name = it.key();
        sym.rva = static_cast<std::uint32_t>(rva);
        const auto sig = v.find("sig");
        if (sig != v.end() && sig->is_string()) ParseSig(sig->get<std::string>(), sym);
        t.syms.push_back(std::move(sym));
    }
    return true;
}

struct Image {
    const std::uint8_t* base = nullptr;
    std::uint32_t size = 0;
    const RUNTIME_FUNCTION* fn = nullptr;
    std::uint32_t nfn = 0;
};

bool GetImage(Image& im) {
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return false;
    const auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const std::uint32_t img = nt->OptionalHeader.SizeOfImage;
    if (d.VirtualAddress == 0 || d.Size < sizeof(RUNTIME_FUNCTION) || d.VirtualAddress + d.Size > img) return false;
    im.base = base;
    im.size = img;
    im.fn = reinterpret_cast<const RUNTIME_FUNCTION*>(base + d.VirtualAddress);
    im.nfn = d.Size / sizeof(RUNTIME_FUNCTION);
    return true;
}

bool MaskedEqual(const std::uint8_t* p, const std::uint8_t* val, const std::uint8_t* care, std::size_t n) {
    __try {
        for (std::size_t i = 0; i < n; ++i) {
            if (care[i] && p[i] != val[i]) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SigAt(const Image& im, std::uint32_t begin, std::uint32_t end, const Sym& s) {
    if (end <= begin || end > im.size || end - begin != s.val.size()) return false;
    return MaskedEqual(im.base + begin, s.val.data(), s.care.data(), s.val.size());
}

int FindSig(const Image& im, const Sym& s, std::uint32_t& hit) {
    int n = 0;
    for (std::uint32_t i = 0; i < im.nfn; ++i) {
        const auto& f = im.fn[i];
        if (!SigAt(im, f.BeginAddress, f.EndAddress, s)) continue;
        hit = f.BeginAddress;
        if (++n >= 2) break;
    }
    return n;
}

bool SigAtRva(const Image& im, const Sym& s) {
    std::uint32_t lo = 0, hi = im.nfn;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (im.fn[mid].BeginAddress < s.rva) lo = mid + 1; else hi = mid;
    }
    return lo < im.nfn && im.fn[lo].BeginAddress == s.rva && SigAt(im, s.rva, im.fn[lo].EndAddress, s);
}

}

void Load() {
    g_loaded = true;
    g_syms.clear();

    std::uint64_t size = 0; std::uint32_t ts = 0;
    if (!ExeIdentity(size, ts)) { g_status = "无法取 exe 标识"; return; }

    const std::string stem = ExeStem();
    std::vector<Table> tables;
    std::string why;

    {
        const auto path = ExeDir() / "ED9Loader" / "rva" / (stem + ".json");
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream f(path, std::ios::binary);
            std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Table t;
            t.label = "外部表 " + stem + ".json";
            std::string w;
            if (!buf.empty() && Parse(buf.data(), buf.size(), size, ts, t, w)) tables.push_back(std::move(t));
            else why = "外部表" + (w.empty() ? std::string("不可用") : w);
        }
    }

    for (int i = 0; i < ed9loader::kEmbeddedRvaTableCount; ++i) {
        const auto& e = ed9loader::kEmbeddedRvaTables[i];
        if (stem != e.name) continue;
        Table t;
        t.label = "内置表 " + std::string(e.name);
        std::string w;
        if (Parse(reinterpret_cast<const char*>(e.data), e.len, size, ts, t, w)) tables.push_back(std::move(t));
        else why += (why.empty() ? "内置表" : ";内置表") + w;
    }

    Image im;
    const bool haveImage = GetImage(im);

    std::string skippedTables = why;
    for (const auto& t : tables) {
        if (!t.sameExe) {
            skippedTables += (skippedTables.empty() ? "" : ";") + t.label.substr(0, t.label.find(' ')) + "与当前 exe 不匹配";
            continue;
        }
        int taken = 0;
        std::string fixed, lost;
        for (const auto& s : t.syms) {
            std::uint32_t rva = s.rva;
            if (!s.val.empty() && haveImage && !SigAtRva(im, s)) {
                if (FindSig(im, s, rva) != 1) { lost += (lost.empty() ? "" : "、") + s.name; continue; }
                fixed += (fixed.empty() ? "" : "、") + s.name;
            }
            g_syms[s.name] = rva;
            ++taken;
        }
        g_status = t.label + ":" + std::to_string(taken) + " 个符号" +
                   (t.skipped ? (",跳过 " + std::to_string(t.skipped)) : "") +
                   (skippedTables.empty() ? "" : "(" + skippedTables + ")") +
                   (fixed.empty() ? "" : ";表里地址和特征对不上、按特征改正:" + fixed) +
                   (lost.empty() ? "" : ";表里地址和特征对不上、也没找回:" + lost);
        return;
    }

    for (const auto& t : tables) {
        if (!t.HasSig()) continue;
        if (!haveImage) {
            g_status = t.label + " 与当前 exe 不匹配(游戏更新过),读不到游戏的函数表,没法按特征找回";
            return;
        }
        int found = 0;
        std::string lost;
        for (const auto& s : t.syms) {
            std::uint32_t hit = 0;
            const int n = s.val.empty() ? 0 : FindSig(im, s, hit);
            if (n == 1) {
                g_syms[s.name] = hit;
                ++found;
                continue;
            }
            lost += (lost.empty() ? "" : "、") + s.name + (s.val.empty() ? "(表里没特征)" : (n > 1 ? "(不止一处)" : ""));
        }
        g_status = t.label + " 与当前 exe 不匹配(游戏更新过),按特征找回 " + std::to_string(found) + "/" +
                   std::to_string(t.syms.size()) + " 个符号" +
                   (lost.empty() ? "" : ";没找回:" + lost + " —— 游戏改了这些函数,要重新生成符号表");
        return;
    }

    for (const auto& t : tables) why += (why.empty() ? "" : ";") + t.label + " 与当前 exe 不匹配";
    g_status = why.empty() ? "无符号表(插件将仅用反射)"
                           : (why + " —— 游戏更新过,表里没有特征、没法自己找回,要重新生成符号表");
}

std::uintptr_t Resolve(const char* name) {
    if (!g_loaded || name == nullptr) return 0;
    const auto it = g_syms.find(name);
    if (it == g_syms.end()) return 0;
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + it->second;
}

const char* StatusText() { return g_status.c_str(); }

}
