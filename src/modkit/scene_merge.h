#pragma once
#include "modkit/patch_config.h"
#include "modkit/tbl_codec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

struct SceneMergeResult {
    bool ok = false;
    std::string err;
    std::vector<uint8_t> sceneBytes;
    std::vector<LpRow>   tblHooks;
};

SceneMergeResult MergeScene(const std::vector<uint8_t>& originalScene, const PatchConfig& cfg);

}
}
