#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

namespace ed9loader {
namespace modkit {

struct DatInsertCall {
    std::string func;
    std::string callee;
    int cmdStruct = -1, cmdOp = -1;
    std::vector<nlohmann::json> args;
};

struct DatPatchConfig {
    std::string target;
    std::vector<DatInsertCall> inserts;
};

bool LoadDatPatchConfig(const std::wstring& path, DatPatchConfig& out, std::string& err);

bool ApplyDatPatch(const std::vector<uint8_t>& orig,
                   const std::vector<DatInsertCall>& inserts,
                   std::vector<uint8_t>& out, std::string& err);

uint32_t FalcomFuncCrc(const std::string& name);

}
}
