// modkit: 通用 #TBL 解码/编码(schema 驱动)。decode 把容器+各表行解成结构化值;encode 整表重建。
// 蓝本 sora_tbl_tool.py / json2tbl.py。round-trip 非字节一致(池重排)但功能等价——游戏照常加载。
#pragma once
#include "modkit/tbl_schema.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

// 行内单字段的值。kind 决定用哪个成员。
struct TblValue {
    enum class K { Int, Flt, Str, Arr, Raw } kind = K::Int;
    int64_t i = 0;                 // Int(标量)
    double f = 0;                  // Flt(float)
    std::string s;                 // Str(toffset 文本)
    std::vector<uint64_t> arr;     // Arr(uNNarray 元素)
    std::vector<uint8_t> raw;      // Raw(dataNN 裸字节)
};

struct TblRowG {
    // 有序字段(与 schema 同序),按名查找。
    std::vector<std::pair<std::string, TblValue>> fields;
    const TblValue* find(const std::string& k) const {
        for (auto& kv : fields) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    TblValue* find(const std::string& k) {
        for (auto& kv : fields) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct TblTableG {
    std::string name;              // header 名(= schema 表名)
    TblSchemaDef schema;           // 解析到的行 schema
    std::vector<TblRowG> rows;
};

struct TblFileG {
    std::vector<TblTableG> tables;
};

// 解码整个 #TBL。schemasDir 供逐 header 解析 schema;preferredGame 如 "Sora1"。任一 header schema 解析失败 → 整体失败。
bool DecodeTblG(const std::vector<uint8_t>& bytes, const std::wstring& schemasDir,
                const std::string& preferredGame, TblFileG& out, std::string& err);

// 整表重建为字节(全量重写,所有 start/length/count/crc32/池 重算)。
std::vector<uint8_t> EncodeTblG(const TblFileG& tbl);

} // namespace modkit
} // namespace ed9loader
