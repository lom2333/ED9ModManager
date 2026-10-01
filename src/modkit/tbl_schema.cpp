#include "modkit/tbl_schema.h"
#include "json.hpp"
#include "modkit/sora1_tbl_schemas_embedded.h"

#include <algorithm>
#include <climits>

using ojson = nlohmann::ordered_json;

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
uint32_t TblArrayElemWidth(const std::string& t) {
    if (!TblTypeIsArray(t)) return 0;
    try { return (uint32_t)std::stoul(t.substr(1, t.size() - 6)) / 8; } catch (...) { return 0; }
}

uint32_t TblTypeWidth(const std::string& t) {
    if (t == "byte" || t == "ubyte") return 1;
    if (t == "short" || t == "ushort") return 2;
    if (t == "int" || t == "uint") return 4;
    if (t == "long" || t == "ulong") return 8;
    if (t == "float") return 4;
    if (startsWith(t, "toffset")) return 8;
    if (startsWith(t, "u") && endsWith(t, "array")) return 12;
    if (startsWith(t, "data")) {
        std::string num = t.substr(4);
        if (!num.empty()) { try { return (uint32_t)std::stoul(num); } catch (...) {} }
        return 0;
    }
    return 0;
}

uint32_t TblSchemaSize(const TblSchemaDef& s) {
    uint32_t sz = 0;
    for (const auto& f : s.fields) {
        uint32_t w = TblTypeWidth(f.type);
        if (w == 0) return 0;
        sz += w;
    }
    return sz;
}

static const ojson& loadCurated(const std::wstring&) {
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
                      uint32_t rowLength, const std::string&,
                      TblSchemaDef& out, std::string& err) {
    const ojson& j = loadCurated(schemasDir);
    if (!j.is_object() || !j.contains(tableName)) { err = "no Sora1 schema for table '" + tableName + "'"; return false; }
    const ojson& byLen = j[tableName];
    if (!byLen.is_object() || byLen.empty()) { err = "empty schema set for '" + tableName + "'"; return false; }

    const ojson* fields = nullptr;
    std::string lenKey = std::to_string(rowLength);
    if (rowLength != 0 && byLen.contains(lenKey)) fields = &byLen[lenKey];
    else if (byLen.size() == 1) fields = &byLen.begin().value();
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

std::string TblVariantGame(const std::string& tableName, uint32_t rowLength) {
    const ojson& j = loadCurated(std::wstring());
    if (!j.is_object() || !j.contains("__variants")) return {};
    const ojson& v = j["__variants"];
    if (!v.is_object() || !v.contains(tableName)) return {};
    const ojson& byLen = v[tableName];
    const std::string key = std::to_string(rowLength);
    if (!byLen.is_object() || !byLen.contains(key)) return {};
    const ojson& g = byLen[key];
    return g.is_string() ? g.get<std::string>() : std::string();
}

static bool legacyFits(const std::string& tableName, uint32_t len, const std::vector<std::string>& names) {
    std::vector<TblSchemaDef> legs;
    if (!ResolveLegacyTblSchemas(tableName, len, legs)) return false;
    for (const TblSchemaDef& L : legs) {
        bool all = true;
        for (const auto& n : names) {
            bool found = false;
            for (const TblField& f : L.fields) if (f.name == n) { found = true; break; }
            if (!found) { all = false; break; }
        }
        if (all) return true;
    }
    return false;
}

std::string TblGameByFieldNames(const std::string& tableName, const std::vector<std::string>& fieldNames) {
    if (fieldNames.empty()) return {};
    const ojson& j = loadCurated(std::wstring());
    if (!j.is_object() || !j.contains(tableName)) return {};
    const ojson& byLen = j[tableName];
    if (!byLen.is_object() || byLen.size() < 2) return {};
    std::string game;
    for (auto it = byLen.begin(); it != byLen.end(); ++it) {
        if (!it.value().is_object()) continue;
        uint32_t len = 0;
        try { len = (uint32_t)std::stoul(it.key()); } catch (...) { continue; }
        bool fits = true;
        for (const auto& n : fieldNames) if (!it.value().contains(n)) { fits = false; break; }
        if (!fits) fits = legacyFits(tableName, len, fieldNames);
        if (!fits) continue;
        const std::string g = TblVariantGame(tableName, len);
        if (g.empty()) return {};
        if (game.empty()) game = g;
        else if (game != g) return {};
    }
    return game;
}

bool ResolveTblSchemaByFieldNames(const std::wstring& schemasDir, const std::string& tableName,
                                  const std::vector<std::string>& jsonFieldNames,
                                  TblSchemaDef& out, std::string& err) {
    const ojson& j = loadCurated(schemasDir);
    if (!j.is_object() || !j.contains(tableName)) { err = "no Sora1 schema for table '" + tableName + "'"; return false; }
    const ojson& byLen = j[tableName];
    if (!byLen.is_object() || byLen.empty()) { err = "empty schema set for '" + tableName + "'"; return false; }
    if (byLen.size() == 1) return ResolveTblSchema(schemasDir, tableName, 0, "", out, err);

    auto scoreOf = [&jsonFieldNames](const std::vector<std::string>& schemaNames) {
        long hit = 0, extra = 0;
        for (const auto& s : schemaNames) {
            bool found = false;
            for (const auto& n : jsonFieldNames) if (n == s) { found = true; break; }
            if (found) ++hit; else ++extra;
        }
        return hit * 2 - extra;
    };
    std::string bestKey; long bestScore = LONG_MIN;
    for (auto it = byLen.begin(); it != byLen.end(); ++it) {
        if (!it.value().is_object()) continue;
        std::vector<std::string> names;
        for (auto f = it.value().begin(); f != it.value().end(); ++f) names.push_back(f.key());
        long score = scoreOf(names);
        uint32_t len = 0;
        try { len = (uint32_t)std::stoul(it.key()); } catch (...) {}
        std::vector<TblSchemaDef> legs;
        if (len != 0 && ResolveLegacyTblSchemas(tableName, len, legs))
            for (const TblSchemaDef& L : legs) {
                std::vector<std::string> ln;
                for (const TblField& f : L.fields) ln.push_back(f.name);
                score = (std::max)(score, scoreOf(ln));
            }
        if (score > bestScore) { bestScore = score; bestKey = it.key(); }
    }
    if (bestKey.empty()) { err = "no matching variant for '" + tableName + "'"; return false; }
    uint32_t len = 0;
    try { len = (uint32_t)std::stoul(bestKey); } catch (...) {}
    return ResolveTblSchema(schemasDir, tableName, len, "", out, err);
}

bool ResolveLegacyTblSchemas(const std::string& tableName, uint32_t rowLength, std::vector<TblSchemaDef>& out) {
    out.clear();
    const ojson& j = loadCurated(std::wstring());
    if (!j.is_object() || !j.contains("__legacy")) return false;
    const ojson& L = j["__legacy"];
    if (!L.is_object() || !L.contains(tableName)) return false;
    const ojson& byLen = L[tableName];
    const std::string key = std::to_string(rowLength);
    if (!byLen.is_object() || !byLen.contains(key) || !byLen[key].is_array()) return false;
    for (const auto& v : byLen[key]) {
        if (!v.is_object()) continue;
        TblSchemaDef d;
        d.game = "Sora1";
        d.variant = key;
        bool ok = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!it.value().is_string()) { ok = false; break; }
            d.fields.push_back(TblField{ it.key(), it.value().get<std::string>() });
        }
        if (ok && d.valid() && TblSchemaSize(d) == rowLength) out.push_back(std::move(d));
    }
    return !out.empty();
}

uint64_t TblSchemaFingerprint() {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < kSora1TblSchemasJsonLen; ++i) { h ^= kSora1TblSchemasJson[i]; h *= 1099511628211ull; }
    return h;
}

}
}
