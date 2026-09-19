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

bool TryLoad(const char* text, std::size_t len, std::uint64_t size, std::uint32_t ts,
             std::string& why, int& taken, int& skipped) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(text, text + len); }
    catch (...) { why = "解析失败"; return false; }
    const auto& e = j["exe"];
    if (!e.is_object() || e.value("size", 0ull) != size || e.value("timestamp", 0u) != ts) {
        why = "与当前 exe 不匹配";
        return false;
    }
    const auto& s = j["symbols"];
    if (!s.is_object()) { why = "无 symbols 段"; return false; }
    std::map<std::string, std::uintptr_t> out;
    taken = skipped = 0;
    for (auto it = s.begin(); it != s.end(); ++it) {
        const auto& v = it.value();
        if (!v.is_object()) continue;
        const std::string conf = v.value("confidence", std::string("low"));
        const std::uint64_t rva = v.value("rva", 0ull);
        if (rva == 0) { ++skipped; continue; }
        if (conf != "reference" && conf != "high") { ++skipped; continue; }
        out[it.key()] = static_cast<std::uintptr_t>(rva);
        ++taken;
    }
    g_syms.swap(out);
    return true;
}

}

void Load() {
    g_loaded = true;
    g_syms.clear();

    std::uint64_t size = 0; std::uint32_t ts = 0;
    if (!ExeIdentity(size, ts)) { g_status = "无法取 exe 标识"; return; }

    const std::string stem = ExeStem();
    int taken = 0, skipped = 0;
    std::string why;

    {
        const auto path = ExeDir() / "ED9Loader" / "rva" / (stem + ".json");
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream f(path, std::ios::binary);
            std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!buf.empty() && TryLoad(buf.data(), buf.size(), size, ts, why, taken, skipped)) {
                g_status = "外部表 " + stem + ".json:" + std::to_string(taken) + " 个符号" +
                           (skipped ? (",跳过 " + std::to_string(skipped)) : "");
                return;
            }
            why = "外部表" + (why.empty() ? std::string("不可用") : why);
        }
    }

    for (int i = 0; i < ed9loader::kEmbeddedRvaTableCount; ++i) {
        const auto& t = ed9loader::kEmbeddedRvaTables[i];
        if (stem != t.name) continue;
        std::string why2;
        if (TryLoad(reinterpret_cast<const char*>(t.data), t.len, size, ts, why2, taken, skipped)) {
            g_status = "内置表 " + std::string(t.name) + ":" + std::to_string(taken) + " 个符号" +
                       (skipped ? (",跳过 " + std::to_string(skipped)) : "") +
                       (why.empty() ? "" : "(" + why + ")");
            return;
        }
        why = why.empty() ? ("内置表" + why2) : (why + ";内置表" + why2);
    }

    g_status = why.empty() ? "无符号表(插件将仅用反射)"
                           : (why + " —— 游戏更新过?请在管理器里重新锚定");
}

std::uintptr_t Resolve(const char* name) {
    if (!g_loaded || name == nullptr) return 0;
    const auto it = g_syms.find(name);
    if (it == g_syms.end()) return 0;
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + it->second;
}

const char* StatusText() { return g_status.c_str(); }

}
