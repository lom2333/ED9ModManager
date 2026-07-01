#include "modkit/tbl_schema.h"
#include "json.hpp"
#include "modkit/sora1_tbl_schemas_embedded.h"   // 编译期内嵌 schema(单一可信源)

using ojson = nlohmann::ordered_json;  // ⚠ 保序:字段顺序 = 二进制布局

namespace ed9loader {
namespace modkit {

static bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
static bool endsWith(const std::string& s, const char* suf) {
    size_t n = std::char_traits<char>::length(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

bool TblTypeIsToffset(const std::string& t) { return startsWith(t, "toffset"); }
bool TblTypeIsArray(const std::string& t) { return startsWith(t, "u") && endsWith(t, "array"); }
bool TblTypeIsFloat(const std::string& t) { return t == "float"; }
bool TblTypeIsScalar(const std::string& t) {
    return t == "byte" || t == "ubyte" || t == "short" || t == "ushort" ||
           t == "int" || t == "uint" || t == "long" || t == "ulong";
}
bool TblTypeIsSignedScalar(const std::string& t) {
    return t == "byte" || t == "short" || t == "int" || t == "long";
}
bool TblTypeIsData(const std::string& t) { return startsWith(t, "data") && t.size() > 4; }
uint32_t TblArrayElemWidth(const std::string& t) {  // "u16array"→2
    if (!TblTypeIsArray(t)) return 0;
    try { return (uint32_t)std::stoul(t.substr(1, t.size() - 6)) / 8; } catch (...) { return 0; }
}

uint32_t TblTypeWidth(const std::string& t) {
    if (t == "byte" || t == "ubyte") return 1;
    if (t == "short" || t == "ushort") return 2;
    if (t == "int" || t == "uint") return 4;
    if (t == "long" || t == "ulong") return 8;
    if (t == "float") return 4;
    if (startsWith(t, "toffset")) return 8;           // 行内 u64 指针
    if (startsWith(t, "u") && endsWith(t, "array")) return 12;  // u64 offset + u32 count
    if (startsWith(t, "data")) {                       // dataNN(裸 NN 字节)
        std::string num = t.substr(4);
        if (!num.empty()) { try { return (uint32_t)std::stoul(num); } catch (...) {} }
        return 0;                                      // 裸 "data"=变长,不支持
    }
    return 0;
}

uint32_t TblSchemaSize(const TblSchemaDef& s) {
    uint32_t sz = 0;
    for (const auto& f : s.fields) {
        uint32_t w = TblTypeWidth(f.type);
        if (w == 0) return 0;                          // 含未知/变长 → 尺寸无效
        sz += w;
    }
    return sz;
}

// 1st 专用合并 schema,结构: { "<表名>": { "<行长>": {字段:类型(有序)} } }。
// 整理脚本对真实 table_sc 严格验证产出,已剔除黎/Kuro 跨游戏 variant。
// schema 直接编译进二进制(kSora1TblSchemasJson,见 sora1_tbl_schemas_embedded.h),
// 不再读外部文件——外部即使被删改也不影响合并正确性。要更新 schema:
// 改 sora1_tbl_schemas.json → 重跑 tools/embed_schema.py → 重新编译。
static const ojson& loadCurated(const std::wstring& /*schemasDir,已弃用*/) {
    static const ojson cached = [] {
        try {
            return ojson::parse(
                reinterpret_cast<const char*>(kSora1TblSchemasJson),
                reinterpret_cast<const char*>(kSora1TblSchemasJson) + kSora1TblSchemasJsonLen);
        } catch (...) {
            return ojson::object();
        }
    }();
    return cached;
}

bool ResolveTblSchema(const std::wstring& schemasDir, const std::string& tableName,
                      uint32_t rowLength, const std::string& /*preferredGame*/,
                      TblSchemaDef& out, std::string& err) {
    const ojson& j = loadCurated(schemasDir);
    if (!j.is_object() || !j.contains(tableName)) { err = "no Sora1 schema for table '" + tableName + "'"; return false; }
    const ojson& byLen = j[tableName];
    if (!byLen.is_object() || byLen.empty()) { err = "empty schema set for '" + tableName + "'"; return false; }

    const ojson* fields = nullptr;
    std::string lenKey = std::to_string(rowLength);
    if (rowLength != 0 && byLen.contains(lenKey)) fields = &byLen[lenKey];
    else if (byLen.size() == 1) fields = &byLen.begin().value();  // 唯一行长,直接用
    if (fields == nullptr || !fields->is_object()) {
        err = "table '" + tableName + "' has no Sora1 schema for length=" + std::to_string(rowLength);
        return false;
    }

    out.game = "Sora1";
    out.variant = lenKey;
    out.fields.clear();
    for (auto it = fields->begin(); it != fields->end(); ++it) {
        if (!it.value().is_string()) { err = "bad field type in '" + tableName + "'"; return false; }
        out.fields.push_back(TblField{ it.key(), it.value().get<std::string>() });
    }
    return out.valid();
}

} // namespace modkit
} // namespace ed9loader
