#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

enum BjKind : uint8_t {
    BJ_ROOT = 0x00, BJ_STRING = 0x02, BJ_NUMBER = 0x03, BJ_OBJECT = 0x04,
    BJ_ARRAY = 0x05, BJ_FLAG = 0x06, BJ_LABELED = 0x12, BJ_PACKED_ID = 0x13,
    BJ_COMPOUND = 0x14
};

struct BjNode {
    uint32_t offset = 0;
    uint8_t  kind = 0;
    uint32_t size = 0;
    uint32_t token = 0;
    bool     hasToken = false;
    std::string name;
    std::string strValue;
    double      numValue = 0.0;
    int         flagValue = 0;
    uint32_t    pidPrimary = 0, pidAux = 0;
    std::vector<uint32_t> children;
};

struct BjName { uint32_t offset; std::string name; uint32_t hashLe; };

class BjsonDecoder {
public:
    bool Parse(std::vector<uint8_t> data);
    const std::vector<uint8_t>& Data() const { return data_; }
    uint32_t RootOffset() const { return rootOffset_; }
    const BjNode& Root() const { return root_; }
    const BjNode& ParseNode(uint32_t offset);
    std::string TokenName(uint32_t token) const;
    bool FindRootChild(const std::string& name, uint32_t& outChildOffset) const;
    bool FindNamedChild(const BjNode& obj, const std::string& name, uint32_t& outOffset);
    size_t NameCount() const { return names_.size(); }
    const std::vector<BjName>& Names() const { return names_; }
    uint64_t NameTableHashStart() const { return nameTableHashStart_; }
    uint64_t NameTableEnd() const { return nameTableEnd_; }
    const std::string& Error() const { return error_; }

private:
    bool fail(const std::string& m) { error_ = m; return false; }
    BjNode parseAt(uint32_t offset);

    std::vector<uint8_t> data_;
    uint64_t nameTableHashStart_ = 0, nameTableEnd_ = 0;
    uint32_t rootOffset_ = 0;
    std::map<uint32_t, std::string> tokenToName_;
    std::vector<BjName> names_;
    std::map<uint32_t, BjNode> cache_;
    BjNode root_;
    std::string error_;
};

}
}
