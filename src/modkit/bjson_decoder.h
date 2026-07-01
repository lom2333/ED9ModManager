// modkit: Falcom binary json 解码器。蓝本 falcom_binary_json_decoder.py。
// 解析 header / name 表 / 节点树为 DOM,保留 offset/token/kind/children,供 patcher 用。
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
    uint32_t token = 0;             // string/number/object/array/flag
    bool     hasToken = false;
    std::string name;              // resolve_token(token),可空
    // 值(按 kind 取):
    std::string strValue;          // string / labeled(label)
    double      numValue = 0.0;    // number
    int         flagValue = 0;     // flag
    uint32_t    pidPrimary = 0, pidAux = 0;  // packed_id
    std::vector<uint32_t> children; // object/array/root/compound/labeled
};

struct BjName { uint32_t offset; std::string name; uint32_t hashLe; };

class BjsonDecoder {
public:
    bool Parse(std::vector<uint8_t> data);              // 拷入并解析
    const std::vector<uint8_t>& Data() const { return data_; }
    uint32_t RootOffset() const { return rootOffset_; }
    const BjNode& Root() const { return root_; }
    const BjNode& ParseNode(uint32_t offset);           // 解析+缓存
    std::string TokenName(uint32_t token) const;
    // root child(顶层 key)按名找:返回其节点 offset,找不到返回 false
    bool FindRootChild(const std::string& name, uint32_t& outChildOffset) const;
    // object/容器节点里按 resolved-name 找直接子节点 offset
    bool FindNamedChild(const BjNode& obj, const std::string& name, uint32_t& outOffset);
    size_t NameCount() const { return names_.size(); }
    const std::vector<BjName>& Names() const { return names_; }   // 名表条目(offset=字符串偏移;token=offset-4)
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

} // namespace modkit
} // namespace ed9loader
