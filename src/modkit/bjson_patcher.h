// modkit: Falcom binary json append-only 补丁器。蓝本 falcom_binary_json_preserve_patch.py。
// 以原二进制为母本,只 append 新/改节点、改 ROOT slot;_patch_subtree 值相等则复用原偏移。
#pragma once
#include "modkit/bjson_decoder.h"
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

class BjsonPatcher {
public:
    explicit BjsonPatcher(BjsonDecoder& dec);
    // 渲染某节点为 json 值(复刻 _render_node_value 的 clean 模式)
    nlohmann::json RenderValue(uint32_t offset);
    // patch 一个顶层 root(如 "Actor"/"ActorIDs"):original 自动从原二进制渲染,edited 由调用方给。
    bool PatchRoot(const std::string& rootName, const nlohmann::json& editedValue);
    // 当前(可能已 append/改写的)字节
    const std::vector<uint8_t>& Bytes() const { return data_; }
    const std::string& Error() const { return error_; }

private:
    bool fail(const std::string& m) { error_ = m; return false; }
    uint32_t append(const std::vector<uint8_t>& blob);
    uint32_t patchSubtree(uint32_t prototypeOffset, const nlohmann::json& orig, const nlohmann::json& edited);
    std::vector<uint32_t> patchArrayChildren(const std::vector<uint32_t>& origOffsets,
                                             const nlohmann::json& origVals, const nlohmann::json& editVals);
    uint32_t insertFromPrototype(const std::vector<uint32_t>& origOffsets, const nlohmann::json& origVals,
                                 const nlohmann::json& editedValue, size_t insertAt);
    std::vector<uint8_t> serializeContainer(uint8_t kind, uint32_t token, const std::vector<uint32_t>& childOffsets);

    BjsonDecoder& dec_;
    std::vector<uint8_t> data_;   // 母本副本 + append
    std::string error_;
    bool errored_ = false;
};

} // namespace modkit
} // namespace ed9loader
