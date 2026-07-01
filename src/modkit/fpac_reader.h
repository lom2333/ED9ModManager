// modkit: FPAC(.pac)读取——按内部名取单条目原版字节。蓝本 extract_pac.py。
// FPAC 格式:magic "FPAC" + u32(count,header_size,unk) + count×[4×u64:hash,name_off,size,location]
//           name 在 name_off 处 null 结尾;数据在 location,长 size。全 LE。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

class FpacReader {
public:
    // 解析 pac 索引(header+条目表+名字)。成功 true。
    bool Open(const std::wstring& pacPath);
    // 按内部名(如 "scene/mp4000_sys.json")读出完整字节。成功 true。
    bool ReadEntry(const std::string& name, std::vector<uint8_t>& out) const;
    bool Has(const std::string& name) const;
    size_t Count() const { return entries_.size(); }

private:
    struct Entry { std::string name; uint64_t hash = 0, size = 0, location = 0; };
    std::wstring path_;
    std::vector<Entry> entries_;
};

} // namespace modkit
} // namespace ed9loader
