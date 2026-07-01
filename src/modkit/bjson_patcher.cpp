#include "modkit/bjson_patcher.h"

#include <algorithm>
#include <cstring>

namespace ed9loader {
namespace modkit {

using nlohmann::json;

static void put_u32(std::vector<uint8_t>& d, size_t pos, uint32_t v) { std::memcpy(d.data() + pos, &v, 4); }
static void push_u32(std::vector<uint8_t>& d, uint32_t v) { uint8_t b[4]; std::memcpy(b, &v, 4); d.insert(d.end(), b, b + 4); }

BjsonPatcher::BjsonPatcher(BjsonDecoder& dec) : dec_(dec) { data_ = dec_.Data(); }

// ---- 值渲染(clean 模式,复刻 _render_node_value)----
nlohmann::json BjsonPatcher::RenderValue(uint32_t offset) {
    const BjNode& n = dec_.ParseNode(offset);
    switch (n.kind) {
    case BJ_STRING:  return json(n.strValue);
    case BJ_NUMBER:  return json(n.numValue);
    case BJ_FLAG:    return (n.flagValue == 0 || n.flagValue == 1) ? json((bool)n.flagValue) : json(n.flagValue);
    case BJ_PACKED_ID: { json o = json::object(); o["id"] = n.pidPrimary; o["aux"] = n.pidAux; return o; }
    case BJ_OBJECT:
    case BJ_COMPOUND: {
        json o = json::object();
        for (uint32_t childOff : n.children) {
            const BjNode& c = dec_.ParseNode(childOff);
            json v = RenderValue(childOff);
            if (!c.name.empty()) {
                if (!o.contains(c.name)) o[c.name] = v;
                else if (o[c.name].is_array()) o[c.name].push_back(v);
                else { json arr = json::array(); arr.push_back(o[c.name]); arr.push_back(v); o[c.name] = arr; }
            } else {
                o["unnamed"].push_back(v);
            }
        }
        return o;
    }
    case BJ_ARRAY:
    case BJ_ROOT: {
        json a = json::array();
        for (uint32_t childOff : n.children) a.push_back(RenderValue(childOff));
        return a;
    }
    case BJ_LABELED: {
        json child = n.children.empty() ? json() : RenderValue(n.children[0]);
        if (child.is_object()) { child["label"] = n.strValue; return child; }
        json o = json::object(); o["label"] = n.strValue; o["value"] = child; return o;
    }
    default: { json o = json::object(); o["type"] = "unknown"; return o; }
    }
}

// ---- signature / similarity(复刻 Python)----
static std::string typeStr(const json& v) {
    if (v.contains("type") && v["type"].is_string()) return v["type"].get<std::string>();
    return "None"; // type 缺失 → Python None 渲染为 "None"
}
static std::string signature(const json& v) {
    if (v.is_object()) {
        if (v.contains("name") && v["name"].is_string())
            return "name:" + typeStr(v) + ":" + v["name"].get<std::string>();
        if (v.contains("id"))
            return "id:" + typeStr(v) + ":" + v["id"].dump();
    }
    return v.dump(); // 紧凑 + 排序键,等价 json.dumps(sort_keys,separators,ensure_ascii=False)
}
static int similarityScore(const json& a, const json& b) {
    if (a.type() != b.type()) return -1000;
    if (a.is_object()) {
        int score = 0;
        json at = a.contains("type") ? a["type"] : json(), bt = b.contains("type") ? b["type"] : json();
        if (at == bt) score += 10;
        // key set 相等
        std::vector<std::string> ka, kb;
        for (auto it = a.begin(); it != a.end(); ++it) ka.push_back(it.key());
        for (auto it = b.begin(); it != b.end(); ++it) kb.push_back(it.key());
        std::sort(ka.begin(), ka.end()); std::sort(kb.begin(), kb.end());
        if (ka == kb) score += 5;
        static const char* excl[] = { "name","id","outline_order","translation","rotation","scale" };
        for (const std::string& k : ka) {
            if (!b.contains(k)) continue;
            bool isExcl = false; for (auto e : excl) if (k == e) { isExcl = true; break; }
            if (isExcl) continue;
            if (a[k] == b[k]) score += 1;
        }
        return score;
    }
    return (a == b) ? 3 : 0;
}

uint32_t BjsonPatcher::append(const std::vector<uint8_t>& blob) {
    uint32_t off = (uint32_t)data_.size();
    data_.insert(data_.end(), blob.begin(), blob.end());
    return off;
}

std::vector<uint8_t> BjsonPatcher::serializeContainer(uint8_t kind, uint32_t token, const std::vector<uint32_t>& childOffsets) {
    std::vector<uint8_t> p;
    p.push_back(kind);
    if (kind == BJ_ARRAY || kind == BJ_OBJECT) push_u32(p, token);
    push_u32(p, (uint32_t)childOffsets.size());
    for (uint32_t o : childOffsets) push_u32(p, o);
    return p;
}

uint32_t BjsonPatcher::patchSubtree(uint32_t prototypeOffset, const json& orig, const json& edited) {
    if (orig == edited) return prototypeOffset;
    const BjNode& node = dec_.ParseNode(prototypeOffset);
    const uint8_t kind = node.kind;

    if (kind == BJ_STRING) {
        std::vector<uint8_t> b; b.push_back(kind); push_u32(b, node.token);
        std::string s = edited.get<std::string>();
        b.insert(b.end(), s.begin(), s.end()); b.push_back(0);
        return append(b);
    }
    if (kind == BJ_NUMBER) {
        std::vector<uint8_t> b; b.push_back(kind); push_u32(b, node.token);
        double v = edited.get<double>(); uint8_t db[8]; std::memcpy(db, &v, 8);
        b.insert(b.end(), db, db + 8);
        return append(b);
    }
    if (kind == BJ_FLAG) {
        std::vector<uint8_t> b; b.push_back(kind); push_u32(b, node.token);
        int raw = edited.is_boolean() ? (edited.get<bool>() ? 1 : 0) : edited.get<int>();
        b.push_back((uint8_t)raw);
        return append(b);
    }
    if (kind == BJ_PACKED_ID) {
        std::vector<uint8_t> b; b.push_back(kind);
        uint32_t primary = (uint32_t)edited["id"].get<int64_t>();
        b.push_back((uint8_t)(primary >> 24)); b.push_back((uint8_t)(primary >> 16));
        b.push_back((uint8_t)(primary >> 8)); b.push_back((uint8_t)primary);
        push_u32(b, (uint32_t)edited["aux"].get<int64_t>());
        return append(b);
    }
    if (kind == BJ_OBJECT || kind == BJ_COMPOUND) {
        std::vector<uint32_t> newOffsets; bool changed = false;
        // 检测重复子名(首版不支持)
        for (uint32_t childOff : node.children) {
            const BjNode& c = dec_.ParseNode(childOff);
            const std::string& key = c.name;
            if (key.empty()) { fail("unnamed child unsupported"); return prototypeOffset; }
            uint32_t patched = patchSubtree(childOff, orig.at(key), edited.at(key));
            changed = changed || (patched != childOff);
            newOffsets.push_back(patched);
        }
        if (!changed) return prototypeOffset;
        return append(serializeContainer(kind, node.token, newOffsets));
    }
    if (kind == BJ_ARRAY) {
        std::vector<uint32_t> origOffsets = node.children;
        std::vector<uint32_t> newOffsets = patchArrayChildren(origOffsets, orig, edited);
        if (newOffsets == origOffsets) return prototypeOffset;
        return append(serializeContainer(kind, node.token, newOffsets));
    }
    fail("unsupported node kind in patchSubtree");
    return prototypeOffset;
}

std::vector<uint32_t> BjsonPatcher::patchArrayChildren(const std::vector<uint32_t>& origOffsets,
                                                       const json& origVals, const json& editVals) {
    const size_t N = origVals.size(), M = editVals.size();
    std::vector<std::string> os(N), es(M);
    for (size_t i = 0; i < N; ++i) os[i] = signature(origVals[i]);
    for (size_t j = 0; j < M; ++j) es[j] = signature(editVals[j]);

    // 简化但对"等长逐元素修改 / 末尾追加"等价于 difflib 的匹配:公共前缀 + 公共后缀 + 中段
    size_t P = 0; while (P < N && P < M && os[P] == es[P]) ++P;
    size_t S = 0; while (S < N - P && S < M - P && os[N - 1 - S] == es[M - 1 - S]) ++S;

    std::vector<uint32_t> result;
    // equal 前缀
    for (size_t i = 0; i < P; ++i)
        result.push_back(patchSubtree(origOffsets[i], origVals[i], editVals[i]));
    // 中段
    const size_t mo1 = P, mo2 = N - S, me1 = P, me2 = M - S;
    if (mo1 < mo2 && me1 < me2) {           // replace
        const size_t overlap = std::min(mo2 - mo1, me2 - me1);
        for (size_t k = 0; k < overlap; ++k)
            result.push_back(patchSubtree(origOffsets[mo1 + k], origVals[mo1 + k], editVals[me1 + k]));
        if (me2 - me1 > overlap) {
            size_t insertAt = mo1 + overlap;
            for (size_t e = me1 + overlap; e < me2; ++e)
                result.push_back(insertFromPrototype(origOffsets, origVals, editVals[e], insertAt));
        }
        // origMid 多出的部分 → delete(跳过)
    } else if (me1 < me2) {                 // insert(origMid 空)
        for (size_t e = me1; e < me2; ++e)
            result.push_back(insertFromPrototype(origOffsets, origVals, editVals[e], mo1));
    } // else delete-only(editMid 空)→ 跳过
    // equal 后缀
    for (size_t k = 0; k < S; ++k)
        result.push_back(patchSubtree(origOffsets[N - S + k], origVals[N - S + k], editVals[M - S + k]));
    return result;
}

uint32_t BjsonPatcher::insertFromPrototype(const std::vector<uint32_t>& origOffsets, const json& origVals,
                                           const json& editedValue, size_t insertAt) {
    std::vector<std::pair<int, size_t>> cand;
    if (insertAt > 0) cand.push_back({ similarityScore(origVals[insertAt - 1], editedValue), insertAt - 1 });
    if (insertAt < origVals.size()) cand.push_back({ similarityScore(origVals[insertAt], editedValue), insertAt });
    for (size_t i = 0; i < origVals.size(); ++i) cand.push_back({ similarityScore(origVals[i], editedValue), i });
    std::stable_sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    size_t best = cand.front().second;
    return patchSubtree(origOffsets[best], origVals[best], editedValue);
}

bool BjsonPatcher::PatchRoot(const std::string& rootName, const json& editedValue) {
    int slot = -1; uint32_t rootChildOff = 0;
    const BjNode& root = dec_.Root();
    for (size_t i = 0; i < root.children.size(); ++i) {
        const BjNode& c = dec_.ParseNode(root.children[i]);
        if (c.name == rootName) { slot = (int)i; rootChildOff = root.children[i]; break; }
    }
    if (slot < 0) return fail("root child not found: " + rootName);
    json original = RenderValue(rootChildOff);
    uint32_t newOff = patchSubtree(rootChildOff, original, editedValue);
    if (!error_.empty()) return false;
    size_t pos = (size_t)dec_.RootOffset() + 5 + (size_t)slot * 4;
    put_u32(data_, pos, newOff);
    return true;
}

} // namespace modkit
} // namespace ed9loader
