#pragma once
#include "modkit/patch_config.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

struct SceneInjectResult {
    bool ok = false;
    std::string err;
    std::vector<uint8_t> bytes;
};

bool SupportsTemplatelessInject(const std::string& type);

SceneInjectResult InjectMonsterArea(const std::vector<uint8_t>& scene, const AddActor& a);

}
}
