#pragma once
#include <cstdint>
#include <string>

namespace ed9loader {
namespace modkit {
inline uint32_t IdPrefixFor(const std::string& type) {
    if (type == "MapObject")        return 0x21;
    if (type == "EventBox")         return 0x27;
    if (type == "LookPoint")        return 0x28;
    if (type == "MonsterArea")      return 0x29;
    if (type == "Path")             return 0x2b;
    if (type == "EnvironmentSound") return 0x04;
    return 0;
}

}
}
