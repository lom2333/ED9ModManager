#include "modkit/bjson_decoder.h"

#include <cstring>

namespace ed9loader {
namespace modkit {

static uint32_t u32le(const std::vector<uint8_t>& d, size_t o) { uint32_t v; std::memcpy(&v, d.data() + o, 4); return v; }
static uint64_t u64le(const std::vector<uint8_t>& d, size_t o) { uint64_t v; std::memcpy(&v, d.data() + o, 8); return v; }
static double   f64le(const std::vector<uint8_t>& d, size_t o) { double v; std::memcpy(&v, d.data() + o, 8); return v; }

// 从 off 找下一个 \0(限 [off,limit)),返回其位置;找不到返回 SIZE_MAX
static size_t findNul(const std::vector<uint8_t>& d, size_t off, size_t limit) {
    for (size_t i = off; i < limit && i < d.size(); ++i) if (d[i] == 0) return i;
    return SIZE_MAX;
}

bool BjsonDecoder::Parse(std::vector<uint8_t> data) {
    data_ = std::move(data);
    cache_.clear(); names_.clear(); tokenToName_.clear(); error_.clear();
    if (data_.size() < 24) return fail("file too small");
    if (std::memcmp(data_.data(), "JSON", 4) != 0) return fail("bad magic");
    nameTableHashStart_ = u64le(data_, 8);
    nameTableEnd_ = nameTableHashStart_ + 4;
    if (nameTableEnd_ > data_.size()) return fail("name table end past EOF");

    // name 表 @ 0x18
    size_t off = 0x18; uint32_t index = 0;
    while (off < nameTableEnd_) {
        size_t end = findNul(data_, off, (size_t)nameTableEnd_);
        if (end == SIZE_MAX) return fail("name string unterminated");
        if (end + 4 >= data_.size()) return fail("name hash past EOF");
        std::string nm(reinterpret_cast<const char*>(data_.data() + off), end - off);
        uint32_t h = u32le(data_, end + 1);
        names_.push_back({ (uint32_t)off, nm, h });
        if (off >= 4) tokenToName_[(uint32_t)off - 4] = nm;
        if (index == 0 && off >= 3) tokenToName_[(uint32_t)off - 3] = nm;
        off = end + 5; ++index;
    }

    rootOffset_ = (uint32_t)nameTableEnd_;
    root_ = parseAt(rootOffset_);
    if (root_.kind != BJ_ROOT) return fail("root node not found");
    return error_.empty();
}

std::string BjsonDecoder::TokenName(uint32_t token) const {
    auto it = tokenToName_.find(token);
    return it == tokenToName_.end() ? std::string() : it->second;
}

BjNode BjsonDecoder::parseAt(uint32_t offset) {
    BjNode n; n.offset = offset;
    if (offset >= data_.size()) { fail("node offset OOR"); return n; }
    const uint8_t kind = data_[offset];
    n.kind = kind;
    switch (kind) {
    case BJ_ROOT: {
        uint32_t count = u32le(data_, offset + 1);
        n.children.reserve(count);
        for (uint32_t i = 0; i < count; ++i) n.children.push_back(u32le(data_, offset + 5 + i * 4));
        n.size = 5 + count * 4;
        break;
    }
    case BJ_STRING: {
        n.hasToken = true; n.token = u32le(data_, offset + 1);
        size_t end = findNul(data_, offset + 5, data_.size());
        n.strValue.assign(reinterpret_cast<const char*>(data_.data() + offset + 5), end - (offset + 5));
        n.name = TokenName(n.token);
        n.size = (uint32_t)(end + 1 - offset);
        break;
    }
    case BJ_NUMBER: {
        n.hasToken = true; n.token = u32le(data_, offset + 1);
        n.numValue = f64le(data_, offset + 5);
        n.name = TokenName(n.token);
        n.size = 13;
        break;
    }
    case BJ_OBJECT:
    case BJ_ARRAY: {
        n.hasToken = true; n.token = u32le(data_, offset + 1);
        uint32_t count = u32le(data_, offset + 5);
        n.children.reserve(count);
        for (uint32_t i = 0; i < count; ++i) n.children.push_back(u32le(data_, offset + 9 + i * 4));
        n.name = TokenName(n.token);
        n.size = 9 + count * 4;
        break;
    }
    case BJ_FLAG: {
        n.hasToken = true; n.token = u32le(data_, offset + 1);
        n.flagValue = data_[offset + 5];
        n.name = TokenName(n.token);
        n.size = 6;
        break;
    }
    case BJ_LABELED: {
        size_t end = findNul(data_, offset + 1, data_.size());
        n.strValue.assign(reinterpret_cast<const char*>(data_.data() + offset + 1), end - (offset + 1));
        uint32_t childOff = (uint32_t)(end + 1);
        n.children.push_back(childOff);
        const BjNode& c = ParseNode(childOff);
        n.size = (childOff - offset) + c.size;
        break;
    }
    case BJ_PACKED_ID: {
        n.pidPrimary = ((uint32_t)data_[offset + 1] << 24) | ((uint32_t)data_[offset + 2] << 16)
                     | ((uint32_t)data_[offset + 3] << 8) | (uint32_t)data_[offset + 4]; // big-endian
        n.pidAux = u32le(data_, offset + 5);
        n.size = 9;
        break;
    }
    case BJ_COMPOUND: {
        uint32_t count = u32le(data_, offset + 1);
        n.children.reserve(count);
        for (uint32_t i = 0; i < count; ++i) n.children.push_back(u32le(data_, offset + 5 + i * 4));
        n.size = 5 + count * 4;
        break;
    }
    default:
        n.size = 1; // unknown
        break;
    }
    return n;
}

const BjNode& BjsonDecoder::ParseNode(uint32_t offset) {
    auto it = cache_.find(offset);
    if (it != cache_.end()) return it->second;
    BjNode n = parseAt(offset);
    auto res = cache_.emplace(offset, std::move(n));
    return res.first->second;
}

bool BjsonDecoder::FindRootChild(const std::string& name, uint32_t& outChildOffset) const {
    for (uint32_t childOff : root_.children) {
        auto it = const_cast<BjsonDecoder*>(this)->cache_.find(childOff);
        const BjNode* c = (it != cache_.end()) ? &it->second
                                               : &const_cast<BjsonDecoder*>(this)->ParseNode(childOff);
        if (c->name == name) { outChildOffset = childOff; return true; }
    }
    return false;
}

bool BjsonDecoder::FindNamedChild(const BjNode& obj, const std::string& name, uint32_t& outOffset) {
    for (uint32_t childOff : obj.children) {
        const BjNode& c = ParseNode(childOff);
        if (c.name == name) { outOffset = childOff; return true; }
    }
    return false;
}

} // namespace modkit
} // namespace ed9loader
