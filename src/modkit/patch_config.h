#pragma once
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

struct AddActor {
    std::string type;
    std::string name;
    std::string label = "\xe2\x97\x86";
    std::string group;
    bool hasTranslation = false; double tx = 0, ty = 0, tz = 0;
    bool hasLpRadius = false; double lpRadius = 0;
    bool hasLpHeight = false; double lpHeight = 0;
    nlohmann::json fields = nlohmann::json::object();
};

struct PatchConfig {
    std::string mod;
    std::string target;
    std::string map;
    std::vector<AddActor> addActors;

    std::string MapName() const;
};

bool LoadPatchConfig(const std::wstring& path, PatchConfig& out, std::string& err);

}
}
