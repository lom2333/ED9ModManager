#include "dds_crypt.h"

#include <cstring>

namespace ed9loader {
namespace modkit {
namespace ddscrypt {
namespace {
constexpr uint32_t kP1 = 2654435761u;
constexpr uint32_t kP2 = 2246822519u;
constexpr uint32_t kP3 = 3266489917u;
constexpr uint32_t kP4 = 668265263u;
constexpr uint32_t kP5 = 374761393u;

inline uint32_t Rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
inline uint32_t Rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

uint32_t XxHash32(const uint8_t* p, size_t len, uint32_t seed) {
    const uint8_t* const end = p + len;
    uint32_t h;
    if (len >= 16) {
        const uint8_t* const limit = end - 16;
        uint32_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        do {
            v1 = Rotl(v1 + Rd32(p) * kP2, 13) * kP1; p += 4;
            v2 = Rotl(v2 + Rd32(p) * kP2, 13) * kP1; p += 4;
            v3 = Rotl(v3 + Rd32(p) * kP2, 13) * kP1; p += 4;
            v4 = Rotl(v4 + Rd32(p) * kP2, 13) * kP1; p += 4;
        } while (p <= limit);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
    } else {
        h = seed + kP5;
    }
    h += static_cast<uint32_t>(len);
    while (p + 4 <= end) { h = Rotl(h + Rd32(p) * kP3, 17) * kP4; p += 4; }
    while (p < end)      { h = Rotl(h + (*p) * kP5, 11) * kP1; ++p; }
    h ^= h >> 15; h *= kP2;
    h ^= h >> 13; h *= kP3;
    h ^= h >> 16;
    return h;
}

bool Lz4Decompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap, size_t* outLen) {
    const uint8_t* s = src;
    const uint8_t* const sEnd = src + srcLen;
    uint8_t* d = dst;
    uint8_t* const dEnd = dst + dstCap;
    while (s < sEnd) {
        const uint32_t token = *s++;
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do { if (s >= sEnd) return false; b = *s++; lit += b; } while (b == 255);
        }
        if (static_cast<size_t>(sEnd - s) < lit || static_cast<size_t>(dEnd - d) < lit) return false;
        memcpy(d, s, lit);
        s += lit; d += lit;
        if (s == sEnd) break;
        if (sEnd - s < 2) return false;
        const size_t off = static_cast<size_t>(s[0]) | (static_cast<size_t>(s[1]) << 8);
        s += 2;
        if (off == 0 || off > static_cast<size_t>(d - dst)) return false;
        size_t match = token & 0x0F;
        if (match == 15) {
            uint8_t b;
            do { if (s >= sEnd) return false; b = *s++; match += b; } while (b == 255);
        }
        match += 4;
        if (static_cast<size_t>(dEnd - d) < match) return false;
        const uint8_t* m = d - off;
        while (match-- > 0) *d++ = *m++;
    }
    *outLen = static_cast<size_t>(d - dst);
    return true;
}

constexpr int kHashLog = 16;
inline size_t Lz4Bound(size_t n) { return n + n / 255 + 16; }

size_t Lz4Compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap) {
    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    const uint8_t* const iend = src + srcSize;
    uint8_t* op = dst;
    uint8_t* const oend = dst + dstCap;

    auto emitLastLiterals = [&]() -> bool {
        const size_t lit = static_cast<size_t>(iend - anchor);
        if (static_cast<size_t>(oend - op) < lit + lit / 255 + 16) return false;
        if (lit >= 15) {
            *op++ = static_cast<uint8_t>(15u << 4);
            size_t r = lit - 15;
            while (r >= 255) { *op++ = 255; r -= 255; }
            *op++ = static_cast<uint8_t>(r);
        } else {
            *op++ = static_cast<uint8_t>(lit << 4);
        }
        if (lit) memcpy(op, anchor, lit);
        op += lit;
        return true;
    };

    if (srcSize >= 13) {
        std::vector<uint32_t> table(static_cast<size_t>(1) << kHashLog, 0);
        const uint8_t* const mflimit  = iend - 12;
        const uint8_t* const matchEnd = iend - 5;
        ++ip;
        bool done = false;
        while (!done) {
            const uint8_t* match = nullptr;
            for (;;) {
                if (ip >= mflimit) { done = true; break; }
                uint32_t seq; memcpy(&seq, ip, 4);
                const uint32_t h = (seq * kP1) >> (32 - kHashLog);
                const uint32_t cand = table[h];
                table[h] = static_cast<uint32_t>(ip - src) + 1;
                if (cand) {
                    const uint8_t* m = src + (cand - 1);
                    uint32_t mseq; memcpy(&mseq, m, 4);
                    if (mseq == seq && static_cast<size_t>(ip - m) <= 65535) { match = m; break; }
                }
                ++ip;
            }
            if (done) break;
            while (ip > anchor && match > src && ip[-1] == match[-1]) { --ip; --match; }

            const size_t lit = static_cast<size_t>(ip - anchor);
            if (static_cast<size_t>(oend - op) < lit + lit / 255 + 16) return 0;
            uint8_t* const token = op;
            if (lit >= 15) {
                *op++ = static_cast<uint8_t>(15u << 4);
                size_t r = lit - 15;
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = static_cast<uint8_t>(r);
            } else {
                *op++ = static_cast<uint8_t>(lit << 4);
            }
            if (lit) memcpy(op, anchor, lit);
            op += lit;

            const uint16_t off = static_cast<uint16_t>(ip - match);
            *op++ = static_cast<uint8_t>(off & 0xFF);
            *op++ = static_cast<uint8_t>(off >> 8);

            const uint8_t* mp = match + 4;
            const uint8_t* xp = ip + 4;
            while (xp < matchEnd && *xp == *mp) { ++xp; ++mp; }
            size_t mlen = static_cast<size_t>(xp - ip) - 4;
            if (mlen >= 15) {
                *token = static_cast<uint8_t>(*token | 15);
                size_t r = mlen - 15;
                if (static_cast<size_t>(oend - op) < r / 255 + 16) return 0;
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = static_cast<uint8_t>(r);
            } else {
                *token = static_cast<uint8_t>(*token | mlen);
            }
            ip = xp;
            anchor = ip;
            if (ip >= mflimit) break;
            {
                uint32_t seq; memcpy(&seq, ip - 2, 4);
                table[(seq * kP1) >> (32 - kHashLog)] = static_cast<uint32_t>(ip - 2 - src) + 1;
            }
        }
    }
    if (!emitLastLiterals()) return 0;
    return static_cast<size_t>(op - dst);
}

uint8_t PickBd(uint64_t rawSize) {
    if (rawSize <= (64u << 10))   return 4;
    if (rawSize <= (256u << 10))  return 5;
    if (rawSize <= (1u << 20))    return 6;
    return 7;
}
size_t BdBlockSize(uint8_t bd) {
    switch (bd) {
        case 4:  return 64u << 10;
        case 5:  return 256u << 10;
        case 6:  return 1u << 20;
        default: return 4u << 20;
    }
}

inline void Put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

constexpr uint32_t kFourccDx10 = 0x30315844u;
constexpr uint32_t kDdsdMipCount = 0x00020000u;
constexpr uint32_t kDdpfFourcc   = 0x00000004u;

std::string FourccStr(uint32_t f) {
    char b[5] = {};
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((f >> (i * 8)) & 0xFF);
        b[i] = (c >= 32 && c < 127) ? c : '?';
    }
    return std::string(b);
}

const char* DxgiName(uint32_t f) {
    switch (f) {
        case 28: return "R8G8B8A8_UNORM";
        case 29: return "R8G8B8A8_UNORM_SRGB";
        case 71: return "BC1_UNORM";
        case 72: return "BC1_UNORM_SRGB";
        case 74: return "BC2_UNORM";
        case 77: return "BC3_UNORM";
        case 80: return "BC4_UNORM";
        case 83: return "BC5_UNORM";
        case 87: return "B8G8R8A8_UNORM";
        case 95: return "BC6H_UF16";
        case 98: return "BC7_UNORM";
        case 99: return "BC7_UNORM_SRGB";
        default: return nullptr;
    }
}

bool IsBlockCompressed(uint32_t fourcc, uint32_t dxgi) {
    if (fourcc == kFourccDx10)
        return (dxgi >= 70 && dxgi <= 84) || (dxgi >= 94 && dxgi <= 99);
    switch (fourcc) {
        case 0x31545844u:
        case 0x33545844u:
        case 0x35545844u:
        case 0x31495441u:
        case 0x32495441u:
        case 0x55344342u:
        case 0x55354342u:
            return true;
        default:
            return false;
    }
}

}

Form Detect(const uint8_t* p, size_t n) {
    if (n >= 4 && p[0] == 0x04 && p[1] == 0x22 && p[2] == 0x4D && p[3] == 0x18) return Form::Game;
    if (n >= 4 && memcmp(p, "DDS ", 4) == 0) return Form::Plain;
    return Form::Unknown;
}
Form Detect(const std::vector<uint8_t>& v) { return Detect(v.data(), v.size()); }

DdsInfo Inspect(const uint8_t* p, size_t n) {
    DdsInfo o;
    if (n < 128 || memcmp(p, "DDS ", 4) != 0) return o;
    auto u32 = [&](size_t off) { uint32_t v; memcpy(&v, p + off, 4); return v; };
    o.ok = true;
    const uint32_t hdrSize = u32(4);
    const uint32_t flags   = u32(8);
    o.height = u32(12);
    o.width  = u32(16);
    o.mips   = u32(28);
    if (o.mips == 0) o.mips = 1;
    const uint32_t pfFlags = u32(80);
    o.fourcc = u32(84);

    if (o.fourcc == kFourccDx10) {
        if (n < 148) {
            o.warns.push_back("DX10 头被截断(不足 148 字节),引擎会读到越界数据");
        } else {
            o.dxgi = u32(128);
            const char* nm = DxgiName(o.dxgi);
            o.format = std::string(nm ? nm : "dxgiFormat=?") + " (DX10)";
            if (!nm) o.format = "DX10 dxgiFormat=" + std::to_string(o.dxgi);
            if (o.dxgi != 98) {
                o.warns.push_back(
                    "DX10 头的 dxgiFormat=" + std::to_string(o.dxgi) +
                    ",引擎只认 98(BC7_UNORM) —— 其余格式的处理器指针是 NULL,加载时 call NULL 直接崩游戏" +
                    (o.dxgi == 99 ? "(格式白名单里没有任何 SRGB 变体)" : ""));
            }
        }
    } else if (o.fourcc != 0 && (pfFlags & kDdpfFourcc) != 0) {
        o.format = FourccStr(o.fourcc);
    } else {
        o.format = "未压缩(" + std::to_string(u32(88)) + " bpp)";
        o.warns.push_back("没有 FourCC(未压缩格式)。引擎的格式白名单以 BC 系为主,原版角色贴图一律是 BC7,建议转成 BC7+DX10");
    }

    if (hdrSize != 124) o.warns.push_back("dwSize=" + std::to_string(hdrSize) + "(标准是 124)");

    if (o.mips > 1 && (flags & kDdsdMipCount) == 0)
        o.warns.push_back("有 " + std::to_string(o.mips) + " 级 mip,dwFlags 却没打 DDSD_MIPMAPCOUNT(0x20000) —— "
                          "实机表现是读档无限加载 + 帧率崩,离线工具查不出来");

    if (IsBlockCompressed(o.fourcc, o.dxgi) && ((o.width & 3) || (o.height & 3)))
        o.warns.push_back("块压缩格式的宽高不是 4 的倍数(" + std::to_string(o.width) + "x" +
                          std::to_string(o.height) + ")");

    return o;
}

bool Decrypt(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    const uint8_t* src = in.data();
    const size_t len = in.size();
    if (len < 7 || Detect(src, len) != Form::Game) { err = "不是游戏格式的 dds(开头不是 LZ4 帧)"; return false; }
    const uint8_t flg = src[4];
    if ((flg >> 6) != 1) { err = "LZ4 帧版本号不是 01"; return false; }
    const bool blockChecksum = (flg & 0x10) != 0;
    const bool hasSize       = (flg & 0x08) != 0;
    const bool hasDictId     = (flg & 0x01) != 0;

    size_t p = 6;
    uint64_t rawSize = 0;
    if (hasSize) {
        if (len < p + 8) { err = "帧头被截断"; return false; }
        memcpy(&rawSize, src + p, 8);
        p += 8;
    }
    if (hasDictId) p += 4;
    p += 1;
    if (len < p) { err = "帧头被截断"; return false; }

    if (hasSize) {
        if (rawSize == 0 || rawSize > (512ull << 20)) { err = "帧头声明的原始大小不合理"; return false; }
        out.reserve(static_cast<size_t>(rawSize));
    }

    std::vector<uint8_t> blockBuf;
    while (p + 4 <= len) {
        uint32_t bs = 0;
        memcpy(&bs, src + p, 4);
        p += 4;
        if (bs == 0) break;
        const bool stored = (bs & 0x80000000u) != 0;
        const size_t n = bs & 0x7FFFFFFFu;
        if (p + n > len) { err = "块数据超出文件末尾(文件不完整?)"; return false; }
        if (stored) {
            out.insert(out.end(), src + p, src + p + n);
        } else {
            const size_t cap = (4u << 20) + 64;
            blockBuf.assign(cap, 0);
            size_t got = 0;
            if (!Lz4Decompress(src + p, n, blockBuf.data(), cap, &got)) { err = "LZ4 块解压失败"; return false; }
            out.insert(out.end(), blockBuf.begin(), blockBuf.begin() + got);
        }
        p += n;
        if (blockChecksum) p += 4;
    }
    if (hasSize && out.size() != rawSize) {
        err = "解出来 " + std::to_string(out.size()) + " 字节,帧头却说是 " + std::to_string(rawSize);
        return false;
    }
    if (out.empty()) { err = "解出来是空的"; return false; }
    return true;
}

bool Encrypt(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (in.empty()) { err = "文件是空的"; return false; }
    if (Detect(in) != Form::Plain) { err = "不是普通 DDS(开头不是 'DDS ')"; return false; }
    if (in.size() > (512ull << 20)) { err = "文件太大(超过 512MB)"; return false; }

    const uint8_t bd = PickBd(in.size());
    const size_t blockMax = BdBlockSize(bd);

    out.reserve(in.size() / 2 + 64);
    out.insert(out.end(), { 0x04, 0x22, 0x4D, 0x18 });
    const uint8_t flg = 0x6C;
    const uint8_t bdByte = static_cast<uint8_t>(bd << 4);
    out.push_back(flg);
    out.push_back(bdByte);
    const uint64_t rawSize = in.size();
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((rawSize >> (i * 8)) & 0xFF));
    {
        const uint8_t* desc = out.data() + 4;
        const size_t descLen = out.size() - 4;
        out.push_back(static_cast<uint8_t>((XxHash32(desc, descLen, 0) >> 8) & 0xFF));
    }

    std::vector<uint8_t> comp(Lz4Bound(blockMax));
    size_t off = 0;
    while (off < in.size()) {
        const size_t n = (in.size() - off < blockMax) ? (in.size() - off) : blockMax;
        const size_t cn = Lz4Compress(in.data() + off, n, comp.data(), comp.size());
        if (cn > 0 && cn < n) {
            Put32(out, static_cast<uint32_t>(cn));
            out.insert(out.end(), comp.begin(), comp.begin() + cn);
        } else {
            Put32(out, static_cast<uint32_t>(n) | 0x80000000u);
            out.insert(out.end(), in.begin() + off, in.begin() + off + n);
        }
        off += n;
    }
    Put32(out, 0);
    Put32(out, XxHash32(in.data(), in.size(), 0));

    std::vector<uint8_t> back;
    std::string e2;
    if (!Decrypt(out, back, e2) || back.size() != in.size() ||
        memcmp(back.data(), in.data(), in.size()) != 0) {
        out.clear();
        err = "自检未通过(重新解开后与原文件不一致)" + (e2.empty() ? std::string() : ":" + e2);
        return false;
    }
    return true;
}

}
}
}
