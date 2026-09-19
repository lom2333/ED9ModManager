#include "modkit/fpac_writer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace ed9loader {
namespace modkit {
namespace {
std::vector<char>& IoBuf() {
    static std::vector<char> b(8u << 20);
    return b;
}

bool CopyRange(std::ifstream& in, uint64_t off, uint64_t size, std::ostream& out,
               const FpacProgress& prog, std::string& err) {
    if (size == 0) return true;
    in.clear();
    in.seekg(static_cast<std::streamoff>(off));
    if (!in) { err = "定位源数据失败"; return false; }
    auto& buf = IoBuf();
    uint64_t left = size;
    while (left > 0) {
        const std::streamsize n =
            static_cast<std::streamsize>(left < buf.size() ? left : buf.size());
        in.read(buf.data(), n);
        if (in.gcount() != n) { err = "读取源数据失败(文件被截断?)"; return false; }
        out.write(buf.data(), n);
        if (!out) { err = "写入失败(磁盘满?)"; return false; }
        left -= static_cast<uint64_t>(n);
        if (prog) prog(static_cast<uint64_t>(n));
    }
    return true;
}

bool CopyDisk(const std::wstring& src, std::ostream& out, const FpacProgress& prog, std::string& err) {
    std::ifstream in(src, std::ios::binary);
    if (!in) { err = "打不开导入的文件"; return false; }
    auto& buf = IoBuf();
    for (;;) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        out.write(buf.data(), n);
        if (!out) { err = "写入失败(磁盘满?)"; return false; }
        if (prog) prog(static_cast<uint64_t>(n));
    }
    return true;
}

}

uint32_t FpacHash(const std::string& internalPath) {
    static uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : internalPath) c = tbl[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c;
}

bool FpacWrite(const std::wstring& srcPac,
               std::vector<FpacItem> items,
               const std::wstring& target,
               const FpacProgress& onProgress,
               std::string& err) {
    err.clear();
    if (items.size() > 0xFFFFFFFFull) { err = "条目太多"; return false; }

    std::vector<uint32_t> hashes(items.size());
    for (size_t i = 0; i < items.size(); ++i) hashes[i] = FpacHash(items[i].path);
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (hashes[a] != hashes[b]) return hashes[a] < hashes[b];
        return items[a].path < items[b].path;
    });

    const fs::path tgt = target;
    const fs::path tmp = fs::path(target).concat(L".pac_tmp");
    const fs::path bak = fs::path(target).concat(L".bak");

    std::error_code ec;
    fs::create_directories(tgt.parent_path(), ec);

    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) { err = "建不了临时文件"; return false; }
        std::ifstream in;
        bool needSrc = false;
        for (const auto& it : items) if (it.srcFile.empty() && it.size > 0) { needSrc = true; break; }
        if (needSrc) {
            in.open(srcPac, std::ios::binary);
            if (!in) { err = "打不开源 pac"; return false; }
        }

        std::string names;
        std::vector<uint64_t> nameRel(order.size());
        for (size_t k = 0; k < order.size(); ++k) {
            nameRel[k] = names.size();
            names += items[order[k]].path;
            names.push_back('\0');
        }
        const uint32_t count = static_cast<uint32_t>(order.size());
        const uint64_t namesBase = 16ull + static_cast<uint64_t>(count) * 32ull;
        const uint64_t headerSize64 = namesBase + names.size();
        if (headerSize64 > 0xFFFFFFFFull) { err = "头部太大(名字区超 4GB)"; return false; }
        const uint32_t headerSize = static_cast<uint32_t>(headerSize64);

        char hdr[16];
        std::memcpy(hdr, "FPAC", 4);
        std::memcpy(hdr + 4, &count, 4);
        std::memcpy(hdr + 8, &headerSize, 4);
        const uint32_t unk = 1;
        std::memcpy(hdr + 12, &unk, 4);
        out.write(hdr, 16);

        std::vector<char> tbl(static_cast<size_t>(count) * 32, 0);
        uint64_t dataOff = headerSize;
        for (uint32_t i = 0; i < count; ++i) {
            const FpacItem& it = items[order[i]];
            char* e = tbl.data() + static_cast<size_t>(i) * 32;
            const uint32_t h = hashes[order[i]];
            const uint32_t zero = 0;
            const uint64_t nameOff = namesBase + nameRel[i];
            std::memcpy(e + 0,  &h, 4);
            std::memcpy(e + 4,  &zero, 4);
            std::memcpy(e + 8,  &nameOff, 8);
            std::memcpy(e + 16, &it.size, 8);
            std::memcpy(e + 24, &dataOff, 8);
            dataOff += it.size;
        }
        out.write(tbl.data(), static_cast<std::streamsize>(tbl.size()));
        out.write(names.data(), static_cast<std::streamsize>(names.size()));
        if (!out) { err = "写头部失败"; }

        for (size_t k = 0; err.empty() && k < order.size(); ++k) {
            const FpacItem& it = items[order[k]];
            if (!it.srcFile.empty()) CopyDisk(it.srcFile, out, onProgress, err);
            else                     CopyRange(in, it.srcOffset, it.size, out, onProgress, err);
        }
        out.flush();
        if (!out) err = "写数据失败";
        out.close();
        in.close();
    }

    if (!err.empty()) { fs::remove(tmp, ec); return false; }

    if (fs::exists(tgt, ec)) {
        fs::remove(bak, ec);
        fs::rename(tgt, bak, ec);
        if (ec) { err = "原文件改名备份失败(被别的程序占用?)"; fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, tgt, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(bak, tgt, ec2);
        err = "替换目标文件失败";
        fs::remove(tmp, ec2);
        return false;
    }
    return true;
}

bool FpacExtractor::Open(const std::wstring& pacPath, std::string& err) {
    in_.close();
    in_.clear();
    in_.open(pacPath, std::ios::binary);
    opened_ = in_.good();
    if (!opened_) err = "打不开源 pac";
    return opened_;
}

bool FpacExtractor::ExtractTo(const FpacItem& it, const std::wstring& outFile,
                              const FpacProgress& onProgress, std::string& err) {
    std::error_code ec;
    fs::create_directories(fs::path(outFile).parent_path(), ec);
    std::ofstream out(outFile, std::ios::binary);
    if (!out) { err = "写不出去(路径太长或没权限?)"; return false; }
    if (!it.srcFile.empty()) return CopyDisk(it.srcFile, out, onProgress, err);
    if (!opened_) { err = "源 pac 未打开"; return false; }
    return CopyRange(in_, it.srcOffset, it.size, out, onProgress, err);
}

}
}
