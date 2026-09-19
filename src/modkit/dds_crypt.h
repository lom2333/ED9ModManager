#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
namespace ddscrypt {
enum class Form {
    Unknown,
    Game,
    Plain,
};

Form Detect(const uint8_t* p, size_t n);
Form Detect(const std::vector<uint8_t>& v);

struct DdsInfo {
    bool        ok = false;
    uint32_t    width = 0;
    uint32_t    height = 0;
    uint32_t    mips = 0;
    uint32_t    fourcc = 0;
    uint32_t    dxgi = 0;
    std::string format;
    std::vector<std::string> warns;
};

DdsInfo Inspect(const uint8_t* p, size_t n);

bool Decrypt(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err);

bool Encrypt(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err);

}
}
}
