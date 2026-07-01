#include "modkit/patch_config.h"
#include "json.hpp"

#include <fstream>

namespace ed9loader {
namespace modkit {

std::string PatchConfig::MapName() const {
    if (!map.empty()) return map;
    // target "mp4000_sys" -> "mp4000"
    const std::string suffix = "_sys";
    if (target.size() > suffix.size() && target.compare(target.size() - suffix.size(), suffix.size(), suffix) == 0)
        return target.substr(0, target.size() - suffix.size());
    return target;
}

bool LoadPatchConfig(const std::wstring& path, PatchConfig& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open patch json"; return false; }
    nlohmann::json j;
    try { f >> j; } catch (const std::exception& e) { err = std::string("json parse: ") + e.what(); return false; }

    out = PatchConfig{};
    out.mod = j.value("_mod", std::string());
    out.target = j.value("target", std::string());
    out.map = j.value("map", std::string());
    // target 可省:调用方(scene_add_json\<target>.json)按文件名推断后回填。map 空时 MapName() 从 target 去 _sys 推断。

    if (j.contains("add_actors") && j["add_actors"].is_array()) {
        for (const auto& a : j["add_actors"]) {
            AddActor act;
            act.type = a.value("type", std::string());
            act.name = a.value("name", std::string());
            act.label = a.value("label", std::string("\xe2\x97\x86"));
            act.group = a.value("group", std::string());
            if (a.contains("translation") && a["translation"].is_object()) {
                const auto& t = a["translation"];
                act.hasTranslation = true;
                act.tx = t.value("x", 0.0); act.ty = t.value("y", 0.0); act.tz = t.value("z", 0.0);
            }
            if (a.contains("lp_radius")) { act.hasLpRadius = true; act.lpRadius = a["lp_radius"].get<double>(); }
            if (a.contains("lp_height")) { act.hasLpHeight = true; act.lpHeight = a["lp_height"].get<double>(); }
            // 其余键 → 通用字段覆盖(scale/rotation/btl_height/... 等任意 actor 字段);跳过已类型化处理的键与 _ 注释键
            if (a.is_object())
                for (auto it = a.begin(); it != a.end(); ++it) {
                    const std::string& k = it.key();
                    if (k == "type" || k == "name" || k == "label" || k == "group" ||
                        k == "translation" || k == "lp_radius" || k == "lp_height") continue;
                    if (!k.empty() && k[0] == '_') continue;
                    act.fields[k] = it.value();
                }
            out.addActors.push_back(std::move(act));
        }
    }
    return true;
}

} // namespace modkit
} // namespace ed9loader
