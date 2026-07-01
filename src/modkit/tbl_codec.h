// modkit: Falcom .tbl 读写(t_lookpoint / LookPointTableData)。蓝本 sora_tbl_tool.py。
// #TBL + u32 header_count + 每 header 80B(name[64]+crc32+start+length+count);行固定区 + 字符串/数组池。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

struct LpRow {
    std::string text1, text2, text3, empty;
    std::vector<uint16_t> arr1;
    uint32_t uint1 = 0;
    std::vector<uint16_t> arr2;
    uint32_t uint2 = 0;
};

class TblCodec {
public:
    // 解码 LookPointTableData 的所有行。err 填错误。
    static bool DecodeLookPoint(const std::vector<uint8_t>& bytes, std::vector<LpRow>& rows, std::string& err);
    // 完整重建(单 LookPointTableData header),布局与 sora_tbl_tool json2tbl 一致。
    static std::vector<uint8_t> EncodeLookPoint(const std::vector<LpRow>& rows);
};

} // namespace modkit
} // namespace ed9loader
