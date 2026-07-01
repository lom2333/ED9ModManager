// modkit: 把精简 patch 合并进 scene(产 merged scene 字节 + 派生 t_lookpoint 钩子行)。
// 蓝本 apply_scene_patch.py。首版只支持 LookPoint。
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
    std::vector<uint8_t> sceneBytes;   // 合并后的 scene 二进制
    std::vector<LpRow>   tblHooks;     // 派生的 t_lookpoint 钩子(待并入全局表)
};

// originalScene = 原版 scene 二进制(从 pac 取);cfg = 解析好的精简配置
SceneMergeResult MergeScene(const std::vector<uint8_t>& originalScene, const PatchConfig& cfg);

} // namespace modkit
} // namespace ed9loader
