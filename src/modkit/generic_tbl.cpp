#include "modkit/generic_tbl.h"

#include <cstring>

namespace ed9loader {
namespace modkit {

// ---- 小端读 ----
static uint32_t rdU32(const std::vector<uint8_t>& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); return v; }
static uint64_t rdU64(const std::vector<uint8_t>& d, size_t o) { uint64_t v = 0; if (o + 8 <= d.size()) std::memcpy(&v, d.data() + o, 8); return v; }

static std::string cstrAt(const std::vector<uint8_t>& d, uint64_t off) {
    if (off == 0 || off >= d.size()) return std::string();
    std::string s;
    for (size_t i = (size_t)off; i < d.size() && d[i] != 0; ++i) s.push_back((char)d[i]);
    return s;
}

// 仅可打印 ASCII(空格~~)。池表的 ref 字段解析出的「干净串」(对话/动作/语音/行为字母码)
// 全是 ASCII;tagged 头部那种带二进制前缀(\xb4\x36…)的会含非打印字节 → 判否,只留裸偏移,
// 既保证 JSON dump 不会因非法 UTF-8 抛异常,又自动只展示有意义的串。
static bool isCleanAscii(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7e) return false;
    return true;
}

// ---- CRC32(zlib),头 crc = zlib.crc32(name) ^ 0xFFFFFFFF ----
static uint32_t crc32_std(const std::string& s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : s) { crc ^= c; for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1))); }
    return crc ^ 0xFFFFFFFFu;
}
static uint32_t header_crc(const std::string& name) { return crc32_std(name) ^ 0xFFFFFFFFu; }

// ---- 字段类型分类(统一来自 tbl_schema)----
static bool isScalarType(const std::string& t) { return TblTypeIsScalar(t); }
static bool isSignedScalar(const std::string& t) { return TblTypeIsSignedScalar(t); }
static bool isToffset(const std::string& t) { return TblTypeIsToffset(t); }
static bool isArray(const std::string& t) { return TblTypeIsArray(t); }
static uint32_t arrayElemWidth(const std::string& t) { return TblArrayElemWidth(t); }
static bool isData(const std::string& t) { return TblTypeIsData(t); }

// ===================== DECODE =====================
bool DecodeTblG(const std::vector<uint8_t>& d, const std::wstring& schemasDir,
                const std::string& preferredGame, TblFileG& out, std::string& err) {
    out.tables.clear();
    if (d.size() < 8 || std::memcmp(d.data(), "#TBL", 4) != 0) { err = "bad #TBL magic"; return false; }
    uint32_t hcount = rdU32(d, 4);
    if (8 + (size_t)hcount * 80 > d.size()) { err = "header table out of range"; return false; }

    for (uint32_t i = 0; i < hcount; ++i) {
        size_t ho = 8 + (size_t)i * 80;
        std::string name(reinterpret_cast<const char*>(d.data() + ho), strnlen(reinterpret_cast<const char*>(d.data() + ho), 64));
        uint32_t start = rdU32(d, ho + 68);
        uint32_t length = rdU32(d, ho + 72);
        uint32_t count = rdU32(d, ho + 76);

        TblTableG tbl;
        tbl.name = name;
        if (!ResolveTblSchema(schemasDir, name, length, preferredGame, tbl.schema, err)) {
            err = "table '" + name + "': " + err;
            return false;
        }
        if (TblSchemaSize(tbl.schema) != length) { err = "table '" + name + "' schema size != row length"; return false; }

        // 池表 ref 可读化:单表头 + schema 无 toffset/array(=池未被建模,如 NPCParam)+ 行区后有数据
        // → 行内 8 字节标量若指向尾部池则是字符串指针(load-time fixup 的文件偏移)。
        // 解析成可读串,以 <字段名>__s 展示字段附加(纯增量:schema 类型不变、EncodeTblG 只认 schema.fields
        // 故忽略 __s、TblHasUnmodeledPool 护栏不受影响、写路径走 CloneRowsPoolTable 不变)。
        bool poolTable = false;
        uint64_t poolStart = 0;
        if (hcount == 1) {
            bool hasVar = false;
            for (const TblField& fld : tbl.schema.fields)
                if (isToffset(fld.type) || isArray(fld.type)) { hasVar = true; break; }
            if (!hasVar) {
                poolStart = (uint64_t)start + (uint64_t)length * count;
                if (d.size() > poolStart + 8) poolTable = true;
            }
        }

        for (uint32_t r = 0; r < count; ++r) {
            size_t pos = (size_t)start + (size_t)r * length;
            TblRowG row;
            for (const TblField& f : tbl.schema.fields) {
                TblValue val;
                const std::string& t = f.type;
                if (isToffset(t)) {
                    val.kind = TblValue::K::Str;
                    val.s = cstrAt(d, rdU64(d, pos));
                    pos += 8;
                } else if (isArray(t)) {
                    val.kind = TblValue::K::Arr;
                    uint64_t off = rdU64(d, pos); uint32_t n = rdU32(d, pos + 8);
                    uint32_t ew = arrayElemWidth(t);
                    for (uint32_t k = 0; k < n && ew; ++k) {
                        uint64_t e = 0;
                        for (uint32_t b = 0; b < ew && (size_t)off + k * ew + b < d.size(); ++b)
                            e |= (uint64_t)d[(size_t)off + k * ew + b] << (8 * b);
                        val.arr.push_back(e);
                    }
                    pos += 12;
                } else if (t == "float") {
                    val.kind = TblValue::K::Flt;
                    float fv = 0; std::memcpy(&fv, d.data() + pos, 4); val.f = (double)fv;
                    pos += 4;
                } else if (isScalarType(t)) {
                    val.kind = TblValue::K::Int;
                    uint32_t w = TblTypeWidth(t);
                    uint64_t v = 0;
                    for (uint32_t b = 0; b < w; ++b) v |= (uint64_t)d[pos + b] << (8 * b);
                    if (isSignedScalar(t) && w < 8) { uint64_t sb = 1ull << (w * 8 - 1); if (v & sb) v |= ~((1ull << (w * 8)) - 1); }
                    val.i = (int64_t)v;
                    pos += w;
                } else if (isData(t)) {
                    val.kind = TblValue::K::Raw;
                    uint32_t w = TblTypeWidth(t);
                    for (uint32_t b = 0; b < w; ++b) val.raw.push_back(d[pos + b]);
                    pos += w;
                } else { err = "unknown field type '" + t + "'"; return false; }

                // 池表 ref 可读化:8 字节标量且值落在尾部池 → 解析 C 串作 <name>__s 附加。
                std::string poolStr;
                if (poolTable && val.kind == TblValue::K::Int && TblTypeWidth(t) == 8) {
                    uint64_t off = (uint64_t)val.i;
                    if (off >= poolStart && off < d.size()) {
                        std::string s = cstrAt(d, off);
                        if (isCleanAscii(s)) poolStr = std::move(s);
                    }
                }
                row.fields.emplace_back(f.name, std::move(val));
                if (!poolStr.empty()) {
                    TblValue sv; sv.kind = TblValue::K::Str; sv.s = std::move(poolStr);
                    row.fields.emplace_back(f.name + "__s", std::move(sv));
                }
            }
            tbl.rows.push_back(std::move(row));
        }
        out.tables.push_back(std::move(tbl));
    }
    return true;
}

// ===================== ENCODE(整表重建) =====================
std::vector<uint8_t> EncodeTblG(const TblFileG& tbl) {
    const uint32_t N = (uint32_t)tbl.tables.size();

    // 每表 rowLen/count/start
    std::vector<uint32_t> rowLen(N), count(N), start(N);
    uint32_t cur = 8 + 80 * N;
    for (uint32_t i = 0; i < N; ++i) {
        rowLen[i] = TblSchemaSize(tbl.tables[i].schema);
        count[i] = (uint32_t)tbl.tables[i].rows.size();
        start[i] = cur;
        cur += rowLen[i] * count[i];
    }
    const uint64_t poolBase = cur;  // extra_offset 初值(所有表定长区之后)

    std::vector<uint8_t> out;
    auto pushU32 = [](std::vector<uint8_t>& v, uint32_t x) { uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4); };
    // 文件头
    out.insert(out.end(), { '#', 'T', 'B', 'L' });
    pushU32(out, N);
    // header 条目
    for (uint32_t i = 0; i < N; ++i) {
        const std::string& name = tbl.tables[i].name;
        size_t nlen = name.size() < 64 ? name.size() : 63;
        out.insert(out.end(), name.begin(), name.begin() + nlen);
        out.insert(out.end(), 64 - nlen, 0);
        pushU32(out, header_crc(name));
        pushU32(out, start[i]);
        pushU32(out, rowLen[i]);
        pushU32(out, count[i]);
    }

    // 定长区(按表顺序顺序追加)+ 共享池
    std::vector<uint8_t> fixed;
    std::vector<uint8_t> pool;
    auto poolAbs = [&]() -> uint64_t { return poolBase + pool.size(); };
    auto fixU64 = [&](uint64_t v) { uint8_t b[8]; std::memcpy(b, &v, 8); fixed.insert(fixed.end(), b, b + 8); };
    auto fixU32 = [&](uint32_t v) { uint8_t b[4]; std::memcpy(b, &v, 4); fixed.insert(fixed.end(), b, b + 4); };
    auto fixBytes = [&](const uint8_t* p, size_t n) { fixed.insert(fixed.end(), p, p + n); };

    for (uint32_t ti = 0; ti < N; ++ti) {
        const TblTableG& T = tbl.tables[ti];
        for (const TblRowG& row : T.rows) {
            for (const TblField& f : T.schema.fields) {
                const std::string& t = f.type;
                const TblValue* v = row.find(f.name);
                TblValue def;  // 缺字段 → 默认(0/""/[])
                if (!v) v = &def;
                if (isToffset(t)) {
                    fixU64(poolAbs());
                    pool.insert(pool.end(), v->s.begin(), v->s.end());
                    pool.push_back(0);                        // 含空串也写 \0
                } else if (isArray(t)) {
                    uint32_t ew = arrayElemWidth(t);
                    while (ew && (poolAbs() % ew) != 0) pool.push_back(0);  // 元素宽对齐
                    fixU64(poolAbs());
                    fixU32((uint32_t)v->arr.size());
                    for (uint64_t e : v->arr)
                        for (uint32_t b = 0; b < ew; ++b) pool.push_back((uint8_t)((e >> (8 * b)) & 0xFF));
                } else if (t == "float") {
                    float fv = (float)v->f; uint8_t b[4]; std::memcpy(b, &fv, 4); fixBytes(b, 4);
                } else if (isScalarType(t)) {
                    uint32_t w = TblTypeWidth(t); uint64_t u = (uint64_t)v->i;
                    for (uint32_t b = 0; b < w; ++b) fixed.push_back((uint8_t)((u >> (8 * b)) & 0xFF));
                } else if (isData(t)) {
                    uint32_t w = TblTypeWidth(t);
                    for (uint32_t b = 0; b < w; ++b) fixed.push_back(b < v->raw.size() ? v->raw[b] : 0);
                }
            }
        }
    }

    out.insert(out.end(), fixed.begin(), fixed.end());
    out.insert(out.end(), pool.begin(), pool.end());
    return out;
}

} // namespace modkit
} // namespace ed9loader
