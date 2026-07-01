#include "modkit/tbl_codec.h"

#include <cstring>

namespace ed9loader {
namespace modkit {

static const char* LP_HEADER = "LookPointTableData";
static const uint32_t LP_ROW_LEN = 64;

static uint32_t rd_u32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; std::memcpy(&v, d.data() + o, 4); return v; }
static uint64_t rd_u64(const std::vector<uint8_t>& d, size_t o) { uint64_t v; std::memcpy(&v, d.data() + o, 8); return v; }
static uint16_t rd_u16(const std::vector<uint8_t>& d, size_t o) { uint16_t v; std::memcpy(&v, d.data() + o, 2); return v; }

// 标准 CRC32(zlib 兼容)
static uint32_t crc32_std(const std::string& s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}
// 头 crc32 = zlib.crc32(name) ^ 0xFFFFFFFF
static uint32_t header_crc(const std::string& name) { return crc32_std(name) ^ 0xFFFFFFFFu; }

static std::string cstr_at(const std::vector<uint8_t>& d, uint64_t off) {
    if (off == 0 || off >= d.size()) return std::string();
    std::string s;
    for (size_t i = (size_t)off; i < d.size() && d[i] != 0; ++i) s.push_back((char)d[i]);
    return s;
}

bool TblCodec::DecodeLookPoint(const std::vector<uint8_t>& d, std::vector<LpRow>& rows, std::string& err) {
    rows.clear();
    if (d.size() < 8 || std::memcmp(d.data(), "#TBL", 4) != 0) { err = "bad #TBL magic"; return false; }
    uint32_t hcount = rd_u32(d, 4);
    size_t hpos = 8;
    for (uint32_t i = 0; i < hcount; ++i) {
        const uint8_t* h = d.data() + hpos + (size_t)i * 80;
        std::string name(reinterpret_cast<const char*>(h), strnlen(reinterpret_cast<const char*>(h), 64));
        uint32_t start = rd_u32(d, hpos + (size_t)i * 80 + 68);
        uint32_t length = rd_u32(d, hpos + (size_t)i * 80 + 72);
        uint32_t count = rd_u32(d, hpos + (size_t)i * 80 + 76);
        if (name != LP_HEADER) continue;
        if (length != LP_ROW_LEN) { err = "unexpected LookPoint row length"; return false; }
        for (uint32_t r = 0; r < count; ++r) {
            size_t base = (size_t)start + (size_t)r * LP_ROW_LEN;
            LpRow row;
            row.text1 = cstr_at(d, rd_u64(d, base + 0));
            row.text2 = cstr_at(d, rd_u64(d, base + 8));
            row.text3 = cstr_at(d, rd_u64(d, base + 16));
            row.empty = cstr_at(d, rd_u64(d, base + 24));
            { uint64_t off = rd_u64(d, base + 32); uint32_t n = rd_u32(d, base + 40);
              for (uint32_t k = 0; k < n; ++k) row.arr1.push_back(rd_u16(d, (size_t)off + k * 2)); }
            row.uint1 = rd_u32(d, base + 44);
            { uint64_t off = rd_u64(d, base + 48); uint32_t n = rd_u32(d, base + 56);
              for (uint32_t k = 0; k < n; ++k) row.arr2.push_back(rd_u16(d, (size_t)off + k * 2)); }
            row.uint2 = rd_u32(d, base + 60);
            rows.push_back(std::move(row));
        }
        return true;
    }
    err = "LookPointTableData header not found";
    return false;
}

std::vector<uint8_t> TblCodec::EncodeLookPoint(const std::vector<LpRow>& rows) {
    const uint32_t count = (uint32_t)rows.size();
    const uint32_t start = 8 + 80 * 1;                 // 单 header
    const size_t fixedSize = (size_t)LP_ROW_LEN * count;
    const size_t base = start + fixedSize;             // 池起点(extra_offset 初值)

    std::vector<uint8_t> out;
    auto pushU32 = [&](std::vector<uint8_t>& v, uint32_t x) { uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4); };
    // 文件头
    out.insert(out.end(), { '#', 'T', 'B', 'L' });
    pushU32(out, 1);
    // header 项(80B)
    std::string name = LP_HEADER;
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), 64 - name.size(), 0);        // name 补齐到 64
    pushU32(out, header_crc(name));
    pushU32(out, start);
    pushU32(out, LP_ROW_LEN);
    pushU32(out, count);
    // out.size()==88 == start

    std::vector<uint8_t> fixed(fixedSize, 0);
    std::vector<uint8_t> pool;
    size_t fp = 0;
    auto putU64f = [&](uint64_t v) { std::memcpy(fixed.data() + fp, &v, 8); fp += 8; };
    auto putU32f = [&](uint32_t v) { std::memcpy(fixed.data() + fp, &v, 4); fp += 4; };
    auto poolAbs = [&]() -> uint64_t { return base + pool.size(); };
    auto emitToffset = [&](const std::string& s) {
        putU64f(poolAbs());
        pool.insert(pool.end(), s.begin(), s.end());
        pool.push_back(0);                              // 含空串也写 \0(与 Python 一致)
    };
    auto emitU16arr = [&](const std::vector<uint16_t>& a) {
        if (poolAbs() % 2) pool.push_back(0);           // 2 字节对齐
        putU64f(poolAbs());
        putU32f((uint32_t)a.size());
        for (uint16_t x : a) { pool.push_back((uint8_t)(x & 0xFF)); pool.push_back((uint8_t)(x >> 8)); }
    };
    for (const LpRow& r : rows) {
        emitToffset(r.text1); emitToffset(r.text2); emitToffset(r.text3); emitToffset(r.empty);
        emitU16arr(r.arr1); putU32f(r.uint1); emitU16arr(r.arr2); putU32f(r.uint2);
    }
    out.insert(out.end(), fixed.begin(), fixed.end());
    out.insert(out.end(), pool.begin(), pool.end());
    return out;
}

} // namespace modkit
} // namespace ed9loader
