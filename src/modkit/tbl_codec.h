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
    static bool DecodeLookPoint(const std::vector<uint8_t>& bytes, std::vector<LpRow>& rows, std::string& err);
    static std::vector<uint8_t> EncodeLookPoint(const std::vector<LpRow>& rows);
};

}
}
