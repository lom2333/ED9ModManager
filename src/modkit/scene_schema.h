// modkit: actor 类型的引擎常量(编译期固化)。来源 Json_to_Json/scene_actor_schema.json。
// 首版聚焦 LookPoint;其余前缀备用(需从含其实例的图采样补默认字段)。
#pragma once
#include <cstdint>
#include <string>

namespace ed9loader {
namespace modkit {

// actor id 高位前缀(id = (prefix<<24) | serial)。未知类型返回 0。
inline uint32_t IdPrefixFor(const std::string& type) {
    if (type == "MapObject")        return 0x21;
    if (type == "EventBox")         return 0x27;
    if (type == "LookPoint")        return 0x28;
    if (type == "MonsterArea")      return 0x29;
    if (type == "Path")             return 0x2b;
    if (type == "EnvironmentSound") return 0x04;
    return 0;
}

} // namespace modkit
} // namespace ed9loader
