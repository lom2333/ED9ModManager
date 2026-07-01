#include "modkit/scene_inject.h"
#include "modkit/bjson_decoder.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace ed9loader {
namespace modkit {

using nlohmann::json;

// ---- 字节读写小工具 ----
static uint32_t rd32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; }
static void     wr32(std::vector<uint8_t>& d, size_t o, uint32_t v) { std::memcpy(&d[o], &v, 4); }
static void     wr64(std::vector<uint8_t>& d, size_t o, uint64_t v) { std::memcpy(&d[o], &v, 8); }

// MonsterArea 字段名 -> game_hash(内置;来源 mp4000_sys,与修正后的 python 解码器一致)
static uint32_t MaHash(const std::string& nm) {
    static const std::map<std::string, uint32_t> m = {
        {"scale",0x13b9da7b},{"shape",0x22cf0027},{"flag",0x2e0b1465},{"stroll_margin",0x3c769213},
        {"id",0x40c698af},{"translation",0x4b96ba90},{"craft_pos_z",0x56429164},{"btl_height",0x727eb91b},
        {"type",0x7321a8d6},{"name",0xa1dc81f9},{"craft_pos_x",0xb84cf048},{"btl_width",0xb88ff398},
        {"btl_radius",0xbc4f324e},{"outline_order",0xc23dbdee},{"craft_pos_y",0xcf4bc0de},
        {"rotation",0xd683670e},{"node_hash",0xe9e310d1},{"navi_name",0xf47153c1},
        {"x",0x7323e97c},{"y",0x0424d9ea},{"z",0x9d2d8850},
    };
    auto it = m.find(nm); return it != m.end() ? it->second : 0;
}

// MonsterArea 的 18 字段名(顺序 = python MA_VALUES 插入序,决定"缺失名"的扩名表字节顺序)
static const char* kMaFieldOrder[] = {
    "scale","rotation","translation","shape","flag","stroll_margin","id",
    "craft_pos_x","craft_pos_y","craft_pos_z","btl_height","btl_width","btl_radius",
    "type","name","outline_order","node_hash","navi_name"
};

bool SupportsTemplatelessInject(const std::string& type) { return type == "MonsterArea"; }

// 在 SceneTree 里按分组名找 hash(Battle_Section 等)。找不到返回 false。
static bool FindGroupHash(BjsonDecoder& d, const std::string& group, double& outHash) {
    uint32_t stOff;
    if (!d.FindRootChild("SceneTree", stOff)) return false;
    BjNode st = d.ParseNode(stOff);
    uint32_t nodesOff;
    if (!d.FindNamedChild(st, "Nodes", nodesOff)) return false;
    BjNode nodes = d.ParseNode(nodesOff);
    std::function<bool(const std::vector<uint32_t>&)> scan = [&](const std::vector<uint32_t>& secs) -> bool {
        for (uint32_t secOff : secs) {
            BjNode sec = d.ParseNode(secOff);
            uint32_t nameOff = 0, hashOff = 0;
            if (d.FindNamedChild(sec, "name", nameOff) && d.FindNamedChild(sec, "hash", hashOff)) {
                BjNode nameN = d.ParseNode(nameOff);
                if (nameN.strValue == group) { outHash = d.ParseNode(hashOff).numValue; return true; }
            }
            uint32_t subOff;
            if (d.FindNamedChild(sec, "Nodes", subOff)) {
                BjNode sub = d.ParseNode(subOff);
                if (scan(sub.children)) return true;
            }
        }
        return false;
    };
    return scan(nodes.children);
}

SceneInjectResult InjectMonsterArea(const std::vector<uint8_t>& scene, const AddActor& a) {
    SceneInjectResult res;
    BjsonDecoder d;
    if (!d.Parse(scene)) { res.err = "scene decode: " + d.Error(); return res; }
    const std::vector<uint8_t>& data = d.Data();
    const uint32_t hashstart = (uint32_t)d.NameTableHashStart();

    // 名表: 名 -> token(=字符串偏移-4);game_hash = 该图名表里读(已有名)或内置表(新名)
    std::map<std::string, uint32_t> tok;   // 已有名 -> token
    for (const BjName& n : d.Names())
        if (n.offset >= 4) tok[n.name] = n.offset - 4;
    auto ghash = [&](const std::string& nm) -> uint32_t {
        auto it = tok.find(nm);
        if (it != tok.end()) return rd32(data, it->second);
        return MaHash(nm);
    };

    // Battle_Section(或 a.group)的 node_hash
    std::string group = a.group.empty() ? std::string("Battle_Section") : a.group;
    double nodeHash = 0;
    if (!FindGroupHash(d, group, nodeHash)) { res.err = "SceneTree 无分组 \"" + group + "\"(MonsterArea 需挂 Battle_Section)"; return res; }

    // ActorIDs 里 MonsterArea 的发号 serial(用它作 id,然后 +1)
    uint32_t aidOff;
    if (!d.FindRootChild("ActorIDs", aidOff)) { res.err = "no ActorIDs root"; return res; }
    BjNode aid = d.ParseNode(aidOff);
    int serial = -1; uint32_t serialValPos = 0;   // 在原scene坐标
    for (uint32_t entOff : aid.children) {
        BjNode ent = d.ParseNode(entOff);
        uint32_t nameOff = 0, idOff = 0;
        if (d.FindNamedChild(ent, "name", nameOff) && d.FindNamedChild(ent, "id", idOff)) {
            if (d.ParseNode(nameOff).strValue == "MonsterArea") {
                serial = (int)d.ParseNode(idOff).numValue;
                serialValPos = d.ParseNode(idOff).offset + 5;   // number 值@+5
                break;
            }
        }
    }
    if (serial < 0) { res.err = "no ActorIDs entry for MonsterArea"; return res; }

    // 字段取值(a.fields 覆盖,否则默认)
    auto fnum = [&](const char* k, double def) { return (a.fields.contains(k) && a.fields[k].is_number()) ? a.fields[k].get<double>() : def; };
    auto fstr = [&](const char* k, const char* def) { return (a.fields.contains(k) && a.fields[k].is_string()) ? a.fields[k].get<std::string>() : std::string(def); };
    auto fxyz = [&](const char* k, double dx, double dy, double dz, double& ox, double& oy, double& oz) {
        ox = dx; oy = dy; oz = dz;
        if (a.fields.contains(k) && a.fields[k].is_object()) {
            const json& o = a.fields[k];
            if (o.contains("x")) ox = o["x"].get<double>();
            if (o.contains("y")) oy = o["y"].get<double>();
            if (o.contains("z")) oz = o["z"].get<double>();
        }
    };
    const double tx = a.tx, ty = a.ty, tz = a.tz;
    double sx, sy, sz, rx, ry, rz; fxyz("scale", 1, 1, 1, sx, sy, sz); fxyz("rotation", 0, 0, 0, rx, ry, rz);
    const double idVal = (double)(((uint32_t)0x29 << 24) | (uint32_t)serial);

    struct Field { std::string name; int kind; double num; std::string str; double ox, oy, oz; };  // kind 0=num 1=str 2=obj
    std::vector<Field> fields = {
        {"scale", 2, 0, "", sx, sy, sz},
        {"shape", 0, fnum("shape", 0), "", 0, 0, 0},
        {"flag", 0, fnum("flag", 1), "", 0, 0, 0},
        {"stroll_margin", 1, 0, fstr("stroll_margin", "EVENT"), 0, 0, 0},
        {"id", 0, idVal, "", 0, 0, 0},
        {"translation", 2, 0, "", tx, ty, tz},
        {"craft_pos_z", 0, tz, "", 0, 0, 0},
        {"btl_height", 1, 0, fstr("btl_height", "M"), 0, 0, 0},
        {"type", 1, 0, "MonsterArea", 0, 0, 0},
        {"name", 1, 0, a.name, 0, 0, 0},
        {"craft_pos_x", 0, tx, "", 0, 0, 0},
        {"btl_width", 1, 0, fstr("btl_width", "M"), 0, 0, 0},
        {"btl_radius", 1, 0, fstr("btl_radius", "M"), 0, 0, 0},
        {"outline_order", 0, 0, "", 0, 0, 0},
        {"craft_pos_y", 0, ty, "", 0, 0, 0},
        {"rotation", 2, 0, "", rx, ry, rz},
        {"node_hash", 0, nodeHash, "", 0, 0, 0},
        {"navi_name", 1, 0, fstr("navi_name", "navi0"), 0, 0, 0},
    };
    std::stable_sort(fields.begin(), fields.end(), [&](const Field& A, const Field& B) { return ghash(A.name) < ghash(B.name); });

    // ---- Phase1: 扩名表(缺失字段名,按 kMaFieldOrder 顺序),移位,修偏移 ----
    std::vector<std::string> missing;
    for (const char* nm : kMaFieldOrder) if (!tok.count(nm)) missing.push_back(nm);
    // (x/y/z 几乎总在场景里;若缺也补)
    for (const char* nm : {"x", "y", "z"}) if (!tok.count(nm)) missing.push_back(nm);

    std::vector<uint8_t> extra;
    std::map<std::string, uint32_t> newTok;
    for (const std::string& nm : missing) {
        newTok[nm] = hashstart + (uint32_t)extra.size();
        uint32_t h = MaHash(nm);
        for (int i = 0; i < 4; ++i) extra.push_back((uint8_t)(h >> (i * 8)));
        for (char c : nm) extra.push_back((uint8_t)c);
        extra.push_back(0);
    }
    const uint32_t delta = (uint32_t)extra.size();

    // 收集所有子偏移表字段(原scene坐标,均指向 >= hashstart 的节点)
    std::vector<std::pair<uint32_t, uint32_t>> offFields;   // (表位置, 子偏移值)
    {
        std::set<uint32_t> seen;
        std::function<void(uint32_t)> walk = [&](uint32_t off) {
            if (seen.count(off)) return; seen.insert(off);
            BjNode n = d.ParseNode(off);
            uint32_t tb = 0xffffffffu;
            if (n.kind == BJ_ROOT) tb = off + 5;                       // 旧模型 root(@hashstart+4) 子表在 +5(=hashstart+9)
            else if (n.kind == BJ_OBJECT || n.kind == BJ_ARRAY) tb = off + 9;
            else if (n.kind == BJ_COMPOUND) tb = off + 5;
            if (tb != 0xffffffffu)
                for (size_t i = 0; i < n.children.size(); ++i) offFields.push_back({ tb + (uint32_t)i * 4, n.children[i] });
            for (uint32_t c : n.children) walk(c);
        };
        for (uint32_t c : d.Root().children) walk(c);
        const BjNode& r = d.Root();
        for (size_t i = 0; i < r.children.size(); ++i) offFields.push_back({ r.offset + 5 + (uint32_t)i * 4, r.children[i] });
    }

    std::vector<uint8_t> nd;
    nd.reserve(data.size() + delta + 256);
    nd.insert(nd.end(), data.begin(), data.begin() + hashstart);
    nd.insert(nd.end(), extra.begin(), extra.end());
    nd.insert(nd.end(), data.begin() + hashstart, data.end());
    wr64(nd, 8, (uint64_t)hashstart + delta);
    for (auto& pf : offFields) wr32(nd, (size_t)pf.first + delta, pf.second + delta);

    // tokOf: 已有名 token 不变(名表前段未动);新名用 newTok。
    auto tokOf = [&](const std::string& nm) -> uint32_t {
        auto it = newTok.find(nm); if (it != newTok.end()) return it->second;
        return tok.at(nm);
    };

    // ---- Phase2: 在 nd 上重解码定位 Actor 数组/槽位,追加 compound + 新数组,重定向,bump ----
    BjsonDecoder d2;
    if (!d2.Parse(nd)) { res.err = "phase1 re-decode: " + d2.Error(); return res; }
    uint32_t actorOff;
    if (!d2.FindRootChild("Actor", actorOff)) { res.err = "no Actor root(phase2)"; return res; }
    BjNode actorNode = d2.ParseNode(actorOff);
    std::vector<uint32_t> oldChildren = actorNode.children;
    const BjNode& root2 = d2.Root();
    uint32_t actorSlotPos = 0; bool slotFound = false;
    for (size_t i = 0; i < root2.children.size(); ++i)
        if (root2.children[i] == actorOff) { actorSlotPos = root2.offset + 5 + (uint32_t)i * 4; slotFound = true; break; }
    if (!slotFound) { res.err = "Actor slot not in root"; return res; }

    std::vector<uint8_t> out = nd;
    auto put8 = [&](uint8_t b) { out.push_back(b); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (i * 8))); };
    auto putf64 = [&](double v) { uint8_t b[8]; std::memcpy(b, &v, 8); for (int i = 0; i < 8; ++i) out.push_back(b[i]); };
    auto emitNum = [&](uint32_t t, double v) -> uint32_t { uint32_t o = (uint32_t)out.size(); put8(0x03); put32(t); putf64(v); return o; };
    auto emitStr = [&](uint32_t t, const std::string& s) -> uint32_t { uint32_t o = (uint32_t)out.size(); put8(0x02); put32(t); for (char c : s) put8((uint8_t)c); put8(0); return o; };
    auto emitContainer = [&](uint8_t kind, bool named, uint32_t t, const std::vector<uint32_t>& kids) -> uint32_t {
        uint32_t o = (uint32_t)out.size(); put8(kind); if (named) put32(t); put32((uint32_t)kids.size()); for (uint32_t k : kids) put32(k); return o; };

    auto emitField = [&](const Field& f) -> uint32_t {
        if (f.kind == 0) return emitNum(tokOf(f.name), f.num);
        if (f.kind == 1) return emitStr(tokOf(f.name), f.str);
        // object: 子 x/y/z 按 game_hash 升序
        struct Sub { std::string nm; double v; };
        std::vector<Sub> xyz = { {"x", f.ox}, {"y", f.oy}, {"z", f.oz} };
        std::stable_sort(xyz.begin(), xyz.end(), [&](const Sub& A, const Sub& B) { return ghash(A.nm) < ghash(B.nm); });
        std::vector<uint32_t> kids;
        for (auto& s : xyz) kids.push_back(emitNum(tokOf(s.nm), s.v));
        return emitContainer(BJ_OBJECT, true, tokOf(f.name), kids);
    };

    std::vector<uint32_t> fieldOffs;
    for (const Field& f : fields) fieldOffs.push_back(emitField(f));
    uint32_t maOff = emitContainer(BJ_COMPOUND, false, 0, fieldOffs);
    std::vector<uint32_t> newActorChildren = oldChildren; newActorChildren.push_back(maOff);
    uint32_t newActorArr = emitContainer(BJ_ARRAY, true, tokOf("Actor"), newActorChildren);

    wr32(out, actorSlotPos, newActorArr);            // 重定向 root.Actor
    if (serialValPos) wr64(out, (size_t)serialValPos + delta, 0);  // 先清(下行写f64)
    if (serialValPos) { double nv = (double)(serial + 1); std::memcpy(&out[(size_t)serialValPos + delta], &nv, 8); }  // ActorIDs id +1(原坐标+delta)

    res.bytes = std::move(out);
    res.ok = true;
    return res;
}

} // namespace modkit
} // namespace ed9loader
