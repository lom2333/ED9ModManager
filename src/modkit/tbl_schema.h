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
    std::string game;
    std::string variant;
    std::vector<TblField> fields;
    bool valid() const { return !fields.empty(); }
};

uint32_t TblTypeWidth(const std::string& type);

bool TblTypeIsToffset(const std::string& t);
bool TblTypeIsArray(const std::string& t);
bool TblTypeIsFloat(const std::string& t);
bool TblTypeIsScalar(const std::string& t);
bool TblTypeIsSignedScalar(const std::string& t);
bool TblTypeIsData(const std::string& t);
uint32_t TblArrayElemWidth(const std::string& t);

uint32_t TblSchemaSize(const TblSchemaDef& s);

bool ResolveTblSchema(const std::wstring& schemasDir, const std::string& tableName,
                      uint32_t rowLength, const std::string& preferredGame,
                      TblSchemaDef& out, std::string& err);

bool ResolveTblSchemaByFieldNames(const std::wstring& schemasDir, const std::string& tableName,
                                  const std::vector<std::string>& jsonFieldNames,
                                  TblSchemaDef& out, std::string& err);

std::string TblVariantGame(const std::string& tableName, uint32_t rowLength);

std::string TblGameByFieldNames(const std::string& tableName, const std::vector<std::string>& fieldNames);

}
}
