// modkit: 精简 scene_patch.json 解析。格式见计划文件。
#pragma once
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

struct AddActor {
    std::string type;                 // 如 "LookPoint" / "MonsterArea"
    std::string name;                 // scene actor name;LookPoint→t_lookpoint.text2;MonsterArea→战斗ID(=t_mon.battle)
    std::string label = "\xe2\x97\x86"; // t_lookpoint.text3,默认 "◆"(仅 LookPoint 用)
    std::string group;                // 可选:SceneTree 分组名(为空则复用同type现有actor的node_hash)
    bool hasTranslation = false; double tx = 0, ty = 0, tz = 0;
    bool hasLpRadius = false; double lpRadius = 0;
    bool hasLpHeight = false; double lpHeight = 0;
    // 通用字段覆盖:除上面这些「元/类型化」键外的所有键(如 MonsterArea 的 scale/rotation/btl_*),
    // 原样套到克隆模板对应字段上。让任意 actor 类型无需各自硬编码即可覆盖。
    nlohmann::json fields = nlohmann::json::object();
};

struct PatchConfig {
    std::string mod;
    std::string target;               // scene/<target>.json,如 "mp4000_sys"
    std::string map;                  // t_lookpoint.text1;为空则由 target 去尾 "_sys" 推导
    std::vector<AddActor> addActors;

    std::string MapName() const;      // 返回 map 或推导值
};

// 读 json 文件;失败填 err 返回 false
bool LoadPatchConfig(const std::wstring& path, PatchConfig& out, std::string& err);

} // namespace modkit
} // namespace ed9loader
