#include "modkit/tbl_merge.h"
#include "modkit/generic_tbl.h"

#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>

namespace ed9loader {
namespace modkit {
namespace tbl_merge {

using json = nlohmann::json;

// json 值 → TblValue(按字段类型)
static TblValue jsonToValue(const std::string& type, const json& jv) {
    TblValue v;
    if (TblTypeIsToffset(type)) {
        v.kind = TblValue::K::Str;
        if (jv.is_string()) v.s = jv.get<std::string>();
    } else if (TblTypeIsArray(type)) {
        v.kind = TblValue::K::Arr;
        if (jv.is_array()) for (const auto& e : jv) if (e.is_number()) v.arr.push_back((uint64_t)e.get<int64_t>());
    } else if (TblTypeIsFloat(type)) {
        v.kind = TblValue::K::Flt;
        if (jv.is_number()) v.f = jv.get<double>();
    } else {  // 整型标量(含未知 → 当 int 0)
        v.kind = TblValue::K::Int;
        if (jv.is_number()) v.i = jv.get<int64_t>();
        else if (jv.is_boolean()) v.i = jv.get<bool>() ? 1 : 0;
    }
    return v;
}

// 默认值(缺字段时)
static TblValue defaultValue(const std::string& type) {
    TblValue v;
    if (TblTypeIsToffset(type)) v.kind = TblValue::K::Str;
    else if (TblTypeIsArray(type)) v.kind = TblValue::K::Arr;
    else if (TblTypeIsFloat(type)) v.kind = TblValue::K::Flt;
    else v.kind = TblValue::K::Int;
    return v;
}

static bool valueMatches(const TblValue& v, const json& jv) {
    switch (v.kind) {
        case TblValue::K::Str: return jv.is_string() && v.s == jv.get<std::string>();
        case TblValue::K::Int: return jv.is_number() && v.i == jv.get<int64_t>();
        case TblValue::K::Flt: return jv.is_number() && v.f == jv.get<double>();
        default: return false;
    }
}

// 字段值字符串化(冲突报告 + 等值比较用)
static std::string valueToString(const TblValue& v) {
    switch (v.kind) {
        case TblValue::K::Str: return v.s;
        case TblValue::K::Int: return std::to_string(v.i);
        case TblValue::K::Flt: { char b[32]; snprintf(b, sizeof(b), "%g", v.f); return b; }
        case TblValue::K::Arr: {
            std::string s = "[";
            for (size_t i = 0; i < v.arr.size(); ++i) { if (i) s += ","; s += std::to_string(v.arr[i]); }
            return s + "]";
        }
        default: return "<raw " + std::to_string(v.raw.size()) + "B>";
    }
}

// 行可读标识:优先 name/id 字段,否则首个文本字段值,否则 #行号。
static std::string rowLabel(const TblRowG& row, size_t index) {
    if (const TblValue* p = row.find("name")) if (p->kind == TblValue::K::Str && !p->s.empty()) return p->s;
    if (const TblValue* p = row.find("id"))   return valueToString(*p);
    for (const auto& kv : row.fields) if (kv.second.kind == TblValue::K::Str && !kv.second.s.empty()) return kv.second.s;
    return "#" + std::to_string(index);
}

static TblTableG* findTable(TblFileG& g, const std::string& tableName, std::string& err) {
    if (g.tables.empty()) { err = "no tables in tbl"; return nullptr; }
    if (tableName.empty()) {
        if (g.tables.size() != 1) { err = "multi-header tbl needs explicit \"table\""; return nullptr; }
        return &g.tables[0];
    }
    for (auto& t : g.tables) if (t.name == tableName) return &t;
    err = "table '" + tableName + "' not found";
    return nullptr;
}

bool ApplyTblPatch(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                   const std::string& preferredGame, const std::string& tableName,
                   const std::vector<json>& addRows, const std::vector<EditOp>& edits,
                   std::vector<uint8_t>& out, std::string& err,
                   std::vector<TblConflict>* conflicts) {
    TblFileG g;
    if (!DecodeTblG(orig, schemasDir, preferredGame, g, err)) return false;
    TblTableG* T = findTable(g, tableName, err);
    if (!T) return false;

    auto typeOf = [&](const std::string& name) -> const std::string* {
        for (const auto& f : T->schema.fields) if (f.name == name) return &f.type;
        return nullptr;
    };

    // 1) edit_rows:对每条,命中所有 match 的行 → set 覆盖。
    //    记录每个 (行,字段) 最近写入者(值+mod):若被另一 mod 写成不同值 → 冲突。
    struct LastWrite { std::string value; std::string mod; };
    std::map<const TblRowG*, std::map<std::string, LastWrite>> writeLog;
    for (const EditOp& ed : edits) {
        if (!ed.match.is_object()) continue;
        size_t rowIdx = 0;
        for (TblRowG& row : T->rows) {
            size_t thisIdx = rowIdx++;
            bool m = true;
            for (auto it = ed.match.begin(); it != ed.match.end(); ++it) {
                const TblValue* fv = row.find(it.key());
                if (!fv || !valueMatches(*fv, it.value())) { m = false; break; }
            }
            if (!m) continue;
            if (ed.set.is_object()) for (auto it = ed.set.begin(); it != ed.set.end(); ++it) {
                const std::string* tp = typeOf(it.key());
                if (!tp) continue;  // 不在 schema 的字段忽略
                TblValue nv = jsonToValue(*tp, it.value());
                std::string nvStr = valueToString(nv);
                // 冲突检测:同行同字段已被别的 mod 写成不同值
                if (conflicts) {
                    auto& fieldLog = writeLog[&row];
                    auto wit = fieldLog.find(it.key());
                    if (wit != fieldLog.end() && wit->second.value != nvStr && wit->second.mod != ed.source) {
                        TblConflict c;
                        c.row = rowLabel(row, thisIdx); c.field = it.key();
                        c.oldValue = wit->second.value; c.newValue = nvStr;
                        c.fromMod = wit->second.mod; c.byMod = ed.source;
                        conflicts->push_back(std::move(c));
                    }
                    fieldLog[it.key()] = { nvStr, ed.source };
                }
                if (TblValue* fv = row.find(it.key())) *fv = nv;
                else row.fields.emplace_back(it.key(), nv);
            }
        }
    }

    // 2) add_rows:按 schema 顺序建行(缺字段补默认)
    for (const json& jr : addRows) {
        if (!jr.is_object()) continue;
        TblRowG row;
        for (const TblField& f : T->schema.fields) {
            TblValue v = jr.contains(f.name) ? jsonToValue(f.type, jr[f.name]) : defaultValue(f.type);
            row.fields.emplace_back(f.name, std::move(v));
        }
        T->rows.push_back(std::move(row));
    }

    out = EncodeTblG(g);
    return true;
}

// ---- 池表(带未建模字符串池,如 t_npc)字节级处理 ----

// 解析单表头 #TBL 的头部字段。返回 false 表示非单表头/非法。
static bool parseSingleHeader(const std::vector<uint8_t>& d, std::string& name,
                              uint32_t& start, uint32_t& length, uint32_t& count) {
    if (d.size() < 88 || std::memcmp(d.data(), "#TBL", 4) != 0) return false;
    uint32_t hc; std::memcpy(&hc, &d[4], 4);
    if (hc != 1) return false;                       // 仅单表头
    const uint8_t* h = &d[8];
    size_t n = 0; while (n < 64 && h[n]) ++n;
    name.assign((const char*)h, n);
    std::memcpy(&start, &d[8 + 64 + 4], 4);          // start @ header+0x44
    std::memcpy(&length, &d[8 + 64 + 8], 4);         // length(stride) @ +0x48
    std::memcpy(&count, &d[8 + 64 + 12], 4);         // count @ +0x4C
    return true;
}

// 取该表 schema 的字段布局(offset/type/width);含变长/未知字段(width0)→ 返回 false。
static bool fieldLayout(const std::wstring& schemasDir, const std::string& game,
                        const std::string& name, uint32_t length,
                        std::vector<std::pair<size_t, TblField>>& out, bool& hasVarField, std::string& err) {
    TblSchemaDef schema;
    if (!ResolveTblSchema(schemasDir, name, length, game, schema, err)) return false;
    hasVarField = false;
    size_t o = 0;
    for (const auto& f : schema.fields) {
        if (TblTypeIsToffset(f.type) || TblTypeIsArray(f.type)) hasVarField = true;
        uint32_t w = TblTypeWidth(f.type);
        if (w == 0) { err = "var/unknown field width in '" + name + "': " + f.type; return false; }
        out.push_back({ o, f });
        o += w;
    }
    if (o != length) { err = "schema size " + std::to_string(o) + " != stride " + std::to_string(length); return false; }
    return true;
}

bool TblHasUnmodeledPool(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                         const std::string& preferredGame) {
    std::string name; uint32_t start, length, count;
    if (!parseSingleHeader(orig, name, start, length, count)) return false;
    std::vector<std::pair<size_t, TblField>> layout; bool hasVar = false; std::string err;
    if (!fieldLayout(schemasDir, preferredGame, name, length, layout, hasVar, err)) return false;
    if (hasVar) return false;                         // 有 toffset/array → 池已被建模
    size_t poolStart = (size_t)start + (size_t)length * count;
    return orig.size() > poolStart + 16;              // 行区之后还有(非对齐填充的)数据 → 未建模池
}

// NPCParam 字段友好别名(中文/英文)→ 真实 schema 字段名。这是唯一来源:
// tbl_merge 的 aliasField 与 orchestrator 的 add_npc「已提供」追踪都走它,保证一致。
// 中文名按字段作用命名(已知作用的取语义名,作用未明的按 schema 诚实译名);真实字段名(packed_id 等)仍可直接用。
// 别名映射以 NPCParam 160 字节行(mp4000 等常见 NPC)为准;152 字节变体里不存在的字段名会在套用时被自动跳过。
std::string AliasNpcParamField(const std::string& key) {
    static const std::unordered_map<std::string, std::string> kMap = {
        // —— 常用 / 已知作用 ——
        { "模型", "packed_id" },        { "model", "packed_id" },        // 模型 = 角色 packed_id(= t_name 的 character_id)
        { "位置X", "pos_x" },           { "坐标X", "pos_x" },            // 世界坐标 X
        { "位置Y", "pos_y" },           { "坐标Y", "pos_y" },            // 世界坐标 Y(高度)
        { "位置Z", "pos_z" },           { "坐标Z", "pos_z" },            // 世界坐标 Z
        { "朝向", "yaw_deg" },          { "yaw", "yaw_deg" },            // 朝向(yaw,单位度;0=默认朝向)
        { "对话", "resource_ref_78" },  { "talk", "resource_ref_78" },   // 对话函数引用 "<模块>.TK_xxx"
        { "动作", "resource_ref_68" },  { "anim", "resource_ref_68" },   // 待机动作 npc_setting.AniEv*
        { "注视距离", "resource_ref_60" }, { "lookdist", "resource_ref_60" }, // npc_setting.LookDistance_*
        { "语音", "resource_ref_80" },  { "voice", "resource_ref_80" },  // 叫卖语音 map.YobikomiV_*
        { "商店类型", "param_98" },                                      // 0=普通 / 1=商人 / 2=商店
        { "启用标志", "flags_18" },                                      // =1 启用该 NPC
        { "交互类型", "type_28" },                                       // =1 普通交互
        { "资源90", "resource_ref_90" },                                 // 备用资源引用
        // —— 行为码块头(借 behavior_from 整块而来,通常不手填)——
        { "行为引用08", "tagged_ref_08" }, { "行为引用10", "tagged_ref_10" },
        { "行为引用20", "tagged_ref_20" }, { "行为引用58", "tagged_ref_58" },
        // —— 作用未明,按 schema 诚实译名 ——
        { "标志04", "flags_04" },       { "标志1C", "flags_1C" },
        { "浮点参数3C", "param_float_3C" }, { "浮点参数40", "param_float_40" },
        { "参数44", "param_44" },       { "浮点48", "float_48" },        { "浮点4C", "float_4C" },
        { "参数50", "param_50" },       { "参数54", "param_54" },
        { "参数70", "param_70" },       { "参数74", "param_74" },
        { "参数88", "param_88" },       { "参数8C", "param_8C" },        { "参数9C", "param_9C" },
    };
    auto it = kMap.find(key);
    return it == kMap.end() ? key : it->second;
}

// clone_rows.set 的友好别名 → 真实字段名(让配置可读,raw 名也仍可用)
static std::string aliasField(const std::string& table, const std::string& key) {
    if (table == "NPCParam") return AliasNpcParamField(key);
    return key;
}

static void writeScalar(std::vector<uint8_t>& b, size_t off, const std::string& type, uint32_t w, const json& jv) {
    if (TblTypeIsFloat(type)) { float f = jv.is_number() ? jv.get<float>() : 0.0f; std::memcpy(&b[off], &f, 4); return; }
    int64_t v = jv.is_number() ? jv.get<int64_t>() : (jv.is_boolean() ? (jv.get<bool>() ? 1 : 0) : 0);
    uint64_t u = (uint64_t)v;
    for (uint32_t i = 0; i < w && i < 8; ++i) b[off + i] = (uint8_t)(u >> (8 * i));
}

bool CloneRowsPoolTable(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                        const std::string& preferredGame, const std::string& tableName,
                        const std::vector<json>& cloneOps, std::vector<uint8_t>& out, std::string& err) {
    std::string name; uint32_t start, length, count;
    if (!parseSingleHeader(orig, name, start, length, count)) { err = "clone: 仅支持单表头 #TBL"; return false; }
    if (!tableName.empty() && tableName != name) { err = "clone: table '" + tableName + "' != header '" + name + "'"; return false; }
    std::vector<std::pair<size_t, TblField>> layout; bool hasVar = false;
    if (!fieldLayout(schemasDir, preferredGame, name, length, layout, hasVar, err)) return false;
    const size_t stride = length;
    const size_t poolStart = (size_t)start + stride * count;
    if (poolStart > orig.size()) { err = "clone: pool_start 越界"; return false; }
    const size_t added = cloneOps.size();
    const size_t delta = stride * added;

    std::vector<uint8_t> pool(orig.begin() + poolStart, orig.end());
    std::vector<uint8_t> rows(orig.begin() + start, orig.begin() + poolStart);  // 可变行区
    std::vector<uint8_t> appendBuf;                          // 追加到池尾的新字符串(自定义对话/资源 tag)
    const size_t newPoolStart = (size_t)start + stride * (count + added);  // 新文件里池起点
    const size_t origPoolSize = pool.size();

    auto gu64 = [](const std::vector<uint8_t>& b, size_t p) { uint64_t v; std::memcpy(&v, &b[p], 8); return v; };
    auto su64 = [](std::vector<uint8_t>& b, size_t p, uint64_t v) { std::memcpy(&b[p], &v, 8); };
    // 现有行:把指向池(>=poolStart)的 8 字节字段整体 +delta
    for (uint32_t ri = 0; ri < count; ++ri) {
        size_t bo = (size_t)ri * stride;
        for (const auto& fo : layout)
            if (TblTypeWidth(fo.second.type) == 8) { uint64_t v = gu64(rows, bo + fo.first); if (v >= poolStart) su64(rows, bo + fo.first, v + delta); }
    }
    // 逐 clone:复制源行(用原始字节)→ 引用 +delta → 套 set
    for (const json& op : cloneOps) {
        if (!op.is_object()) { err = "clone: op 非对象"; return false; }
        int fromIdx = op.value("from_index", 0);
        if (fromIdx < 0 || (uint32_t)fromIdx >= count) { err = "clone: from_index 越界 " + std::to_string(fromIdx); return false; }
        std::vector<uint8_t> row(orig.begin() + start + (size_t)fromIdx * stride,
                                 orig.begin() + start + (size_t)fromIdx * stride + stride);
        for (const auto& fo : layout)
            if (TblTypeWidth(fo.second.type) == 8) { uint64_t v = gu64(row, fo.first); if (v >= poolStart) su64(row, fo.first, v + delta); }
        json set = op.value("set", json::object());
        if (set.is_object())
            for (auto it = set.begin(); it != set.end(); ++it) {
                std::string realKey = aliasField(name, it.key());
                for (const auto& fo : layout) {
                    if (fo.second.name != realKey) continue;
                    uint32_t w = TblTypeWidth(fo.second.type);
                    if (it.value().is_string() && w == 8) {
                        // 引用字段设为字符串 → 追加进池尾,字段指过去(自定义对话 tag/资源,如 "MyNpc.TK_xxx")
                        std::string s = it.value().get<std::string>();
                        uint64_t off = newPoolStart + origPoolSize + appendBuf.size();
                        appendBuf.insert(appendBuf.end(), s.begin(), s.end());
                        appendBuf.push_back(0);
                        su64(row, fo.first, off);
                    } else {
                        writeScalar(row, fo.first, fo.second.type, w, it.value());
                    }
                    break;
                }
            }
        rows.insert(rows.end(), row.begin(), row.end());
    }
    // 拼新文件:头部(count+added)+ 行 + 池 + 追加串
    out.assign(orig.begin(), orig.begin() + start);
    uint32_t newCount = count + (uint32_t)added;
    std::memcpy(&out[8 + 64 + 12], &newCount, 4);     // 更新 count
    out.insert(out.end(), rows.begin(), rows.end());
    out.insert(out.end(), pool.begin(), pool.end());
    out.insert(out.end(), appendBuf.begin(), appendBuf.end());
    return true;
}

} // namespace tbl_merge
} // namespace modkit
} // namespace ed9loader
