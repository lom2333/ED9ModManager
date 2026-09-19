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
    nlohmann::json RenderValue(uint32_t offset);
    bool PatchRoot(const std::string& rootName, const nlohmann::json& editedValue);
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
    std::vector<uint8_t> data_;
    std::string error_;
    bool errored_ = false;
};

}
}
