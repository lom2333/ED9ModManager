#pragma once
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
namespace tbl_merge {

struct EditOp {
    nlohmann::json match;
    nlohmann::json set;
    std::string source;
};

struct TblConflict {
    std::string table;
    std::string row;
    std::string field;
    std::string oldValue;
    std::string newValue;
    std::string fromMod;
    std::string byMod;
};

bool ApplyTblPatch(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                   const std::string& preferredGame, const std::string& tableName,
                   const std::vector<nlohmann::json>& addRows,
                   const std::vector<EditOp>& edits,
                   std::vector<uint8_t>& out, std::string& err,
                   std::vector<TblConflict>* conflicts = nullptr);

bool TblHasUnmodeledPool(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                         const std::string& preferredGame);

bool CloneRowsPoolTable(const std::vector<uint8_t>& orig, const std::wstring& schemasDir,
                        const std::string& preferredGame, const std::string& tableName,
                        const std::vector<nlohmann::json>& cloneOps,
                        std::vector<uint8_t>& out, std::string& err);

std::string AliasNpcParamField(const std::string& key);

}
}
}
