#pragma once
#include "modkit/tbl_schema.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
struct TblValue {
    enum class K { Int, Flt, Str, Arr, Raw } kind = K::Int;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<uint64_t> arr;
    std::vector<uint8_t> raw;
};

struct TblRowG {
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
    std::string name;
    TblSchemaDef schema;
    std::vector<TblRowG> rows;
};

struct TblFileG {
    std::vector<TblTableG> tables;
};

bool DecodeTblG(const std::vector<uint8_t>& bytes, const std::wstring& schemasDir,
                const std::string& preferredGame, TblFileG& out, std::string& err);

std::vector<uint8_t> EncodeTblG(const TblFileG& tbl);

}
}
