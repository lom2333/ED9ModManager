#include "modkit/fpac_reader.h"

#include <fstream>
#include <cstring>

namespace ed9loader {
namespace modkit {

// host 是 x64 Windows(LE),直接 memcpy
static uint32_t rd_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t rd_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

bool FpacReader::Open(const std::wstring& pacPath) {
    path_ = pacPath;
    entries_.clear();
    std::ifstream f(pacPath, std::ios::binary);
    if (!f) return false;

    uint8_t hdr[16];
    f.read(reinterpret_cast<char*>(hdr), 16);
    if (!f) return false;
    if (std::memcmp(hdr, "FPAC", 4) != 0) return false;
    const uint32_t count = rd_u32(hdr + 4);
    // hdr+8 header_size, hdr+12 unk(未用)

    std::vector<uint8_t> table(static_cast<size_t>(count) * 32);
    f.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
    if (!f) return false;

    entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = table.data() + static_cast<size_t>(i) * 32;
        Entry en;
        en.hash = rd_u64(e + 0);
        const uint64_t name_off = rd_u64(e + 8);
        en.size = rd_u64(e + 16);
        en.location = rd_u64(e + 24);
        // 读 name(null 结尾)
        f.clear();
        f.seekg(static_cast<std::streamoff>(name_off));
        std::string nm;
        char c;
        while (f.get(c)) { if (c == '\0') break; nm.push_back(c); }
        en.name = std::move(nm);
        entries_.push_back(std::move(en));
    }
    return true;
}

bool FpacReader::Has(const std::string& name) const {
    for (const auto& e : entries_) if (e.name == name) return true;
    return false;
}

bool FpacReader::ReadEntry(const std::string& name, std::vector<uint8_t>& out) const {
    const Entry* found = nullptr;
    for (const auto& e : entries_) if (e.name == name) { found = &e; break; }
    if (!found) return false;
    std::ifstream f(path_, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(found->location));
    out.resize(static_cast<size_t>(found->size));
    if (found->size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(found->size));
        if (!f) return false;
    }
    return true;
}

} // namespace modkit
} // namespace ed9loader
