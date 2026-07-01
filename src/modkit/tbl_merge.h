// modkit: 通用 tbl 补丁应用。把 Mod\<mod>\tbl\<表>.json 的 add_rows / edit_rows 套到原始 tbl → 新字节。
#pragma once
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
namespace tbl_merge {

struct EditOp {
    nlohmann::json match;   // {字段:值} 全部匹配才命中
    nlohmann::json set;     // {字段:值} 覆盖
    std::string source;     // 来源 mod 文件夹名(冲突报告用;可空)
};

// 一处「同行同字段被多个 edit 写成不同值」的冲突记录。
struct TblConflict {
    std::string table;      // 表名(由调用方填,ApplyTblPatch 留空)
    std::string row;        // 行可读标识(name/id/首文本字段/#行号)
    std::string field;      // 字段名
    std::string oldValue;   // 被覆盖的值(字符串化)
    std::string newValue;   // 覆盖后的值(若它是最后写入者则为最终生效值)
    std::string fromMod;    // 旧值来自的 mod
    std::string byMod;      // 新值来自的 mod
};

// 应用补丁到原始 tbl 字节。tableName 为空且单表头则自动取唯一表;多表头必须指定。
// addRows: 每个是 {字段:值} 对象(缺字段补默认 0/""/[])。失败填 err 返回 false。
// edits 按顺序套用,后写覆盖先写;同行同字段被不同 mod 写成不同值 → 追加一条 conflicts(table 字段留空)。
bool ApplyTblPatch(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                   const std::string& preferredGame, const std::string& tableName,
                   const std::vector<nlohmann::json>& addRows,
                   const std::vector<EditOp>& edits,
                   std::vector<uint8_t>& out, std::string& err,
                   std::vector<TblConflict>* conflicts = nullptr);

// 检测「带未建模字符串池」的表(单表头、schema 无 toffset/array、行区之后还有数据,如 t_npc)。
// 这类表不能走 ApplyTblPatch(会丢池→损坏),只能 clone_rows 或整文件替换。
bool TblHasUnmodeledPool(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                         const std::string& preferredGame);

// 池感知「克隆行」:复制现有行(连同它指向的池串)到表尾,保池 + 偏移整体平移(+新增行数×行长),
// 对每个克隆行套 set 覆盖(改坐标等)。仅单表头。cloneOps 每个 = {"from_index":N(默认0), "set":{字段:值}}。
bool CloneRowsPoolTable(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                        const std::string& preferredGame, const std::string& tableName,
                        const std::vector<nlohmann::json>& cloneOps,
                        std::vector<uint8_t>& out, std::string& err);

// NPCParam 字段友好别名(中文/英文)→ 真实 schema 字段名;无匹配则原样返回(真实字段名仍可直接用)。
// add_npc / clone_rows 的 set 与 orchestrator 的「已提供」追踪共用此唯一来源,避免两处不一致。
std::string AliasNpcParamField(const std::string& key);

} // namespace tbl_merge
} // namespace modkit
} // namespace ed9loader
