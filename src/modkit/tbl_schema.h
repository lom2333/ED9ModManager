// modkit: 通用 .tbl 行 schema(从 tbl_schemas/headers/<表名>.json + sora1_local_schemas.json 解析)。
// schema = 有序字段列表 {名, 类型}。类型: ubyte/ushort/uint/ulong + 有符号版 + float + toffset + uNNarray + dataNN。
// ⚠ 字段顺序 = 二进制布局顺序,解析必须保序(nlohmann::ordered_json)。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

struct TblField {
    std::string name;
    std::string type;
};

struct TblSchemaDef {
    std::string game;                 // 该 schema 的 game(如 "Sora1"/"Kai")
    std::string variant;              // variant 名(如 "FALCOM_PC")
    std::vector<TblField> fields;     // 有序
    bool valid() const { return !fields.empty(); }
};

// 字段类型字节宽度(行内固定区占用)。toffset=8(指针),uNNarray=12(u64偏移+u32计数),dataNN=NN,标量按类型。返回0=未知。
uint32_t TblTypeWidth(const std::string& type);

// 字段类型分类(供编解码与补丁应用共用)
bool TblTypeIsToffset(const std::string& t);       // 文本(行内 u64 指针)
bool TblTypeIsArray(const std::string& t);         // uNNarray(行内 u64 偏移+u32 计数)
bool TblTypeIsFloat(const std::string& t);         // == "float"
bool TblTypeIsScalar(const std::string& t);        // 整型标量 byte/short/int/long(含 u 前缀)
bool TblTypeIsSignedScalar(const std::string& t);  // 有符号整型标量
bool TblTypeIsData(const std::string& t);          // dataNN(裸字节)
uint32_t TblArrayElemWidth(const std::string& t);  // uNNarray 元素字节宽(u16array→2)

// 整行字节数 = 各字段 TblTypeWidth 之和。
uint32_t TblSchemaSize(const TblSchemaDef& s);

// 解析某表的行 schema。schemasDir = 含 headers\ 和 sora1_local_schemas.json 的目录。
// rowLength = #TBL header 里的 length(用于在多 variant 间按尺寸过滤)。preferredGame 优先(如 "Sora1")。
// 找到填 out 返回 true;否则填 err 返回 false。
bool ResolveTblSchema(const std::wstring& schemasDir, const std::string& tableName,
                      uint32_t rowLength, const std::string& preferredGame,
                      TblSchemaDef& out, std::string& err);

} // namespace modkit
} // namespace ed9loader
