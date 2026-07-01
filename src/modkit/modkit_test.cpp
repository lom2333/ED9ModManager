// modkit 离线测试 harness:不依赖游戏,字节对照 golden 验证各阶段。
// 用法: modkit_test.exe   (跑全部已实现阶段)
#include "modkit/fpac_reader.h"
#include "modkit/bjson_decoder.h"
#include "modkit/bjson_patcher.h"
#include "modkit/tbl_codec.h"
#include "modkit/tbl_schema.h"
#include "modkit/generic_tbl.h"
#include "modkit/patch_config.h"
#include "modkit/scene_merge.h"
#include "modkit/mod_merge_orchestrator.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ed9loader::modkit;

static bool read_file(const std::wstring& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    f.seekg(0);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(f);
}

static bool read_json(const std::wstring& p, nlohmann::json& j) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    try { f >> j; } catch (...) { return false; }
    return true;
}

static int first_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return static_cast<int>(i);
    if (a.size() != b.size()) return static_cast<int>(n);
    return -1;
}

static const wchar_t* GAME = L"D:\\SteamLibrary\\steamapps\\common\\Sora No Kiseki the 1st";
static const wchar_t* SCHEMAS = L"D:\\destop\\ce修改器\\Sora1\\tbl_schemas";  // 通用 tbl schema(离线测试用项目目录)

static int test_p0_fpac() {
    printf("=== P0 FpacReader ===\n");
    int fail = 0;
    struct Case { std::wstring pac; std::string name; std::wstring golden; };
    Case cases[] = {
        { std::wstring(GAME) + L"\\pac\\steam\\scene.pac", "scene/mp4000_sys.json",
          L"D:\\destop\\ce修改器\\Sora1\\scene\\scene\\mp4000_sys.json" },
        { std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac", "table_sc/t_lookpoint.tbl",
          L"D:\\destop\\ce修改器\\Sora1\\table_sc\\t_lookpoint.tbl" },
    };
    for (auto& c : cases) {
        FpacReader r;
        if (!r.Open(c.pac)) { printf("  [FAIL] open pac for %s\n", c.name.c_str()); ++fail; continue; }
        std::vector<uint8_t> got, gold;
        if (!r.ReadEntry(c.name, got)) { printf("  [FAIL] ReadEntry %s (entries=%zu)\n", c.name.c_str(), r.Count()); ++fail; continue; }
        if (!read_file(c.golden, gold)) { printf("  [WARN] no golden for %s (got=%zu)\n", c.name.c_str(), got.size()); continue; }
        const int d = first_diff(got, gold);
        const bool eq = (d < 0);
        printf("  [%s] %s  got=%zu golden=%zu entries=%zu%s\n",
               eq ? "OK" : "FAIL", c.name.c_str(), got.size(), gold.size(), r.Count(),
               eq ? "" : ("  first_diff=" + std::to_string(d)).c_str());
        if (!eq) ++fail;
    }
    return fail;
}

static int test_p1_decode() {
    printf("\n=== P1 BjsonDecoder (语义对照) ===\n");
    int fail = 0;
    FpacReader r;
    if (!r.Open(std::wstring(GAME) + L"\\pac\\steam\\scene.pac")) { printf("  [FAIL] open scene.pac\n"); return 1; }
    std::vector<uint8_t> bytes;
    if (!r.ReadEntry("scene/mp4000_sys.json", bytes)) { printf("  [FAIL] read entry\n"); return 1; }
    BjsonDecoder d;
    if (!d.Parse(bytes)) { printf("  [FAIL] parse: %s\n", d.Error().c_str()); return 1; }

    // root keys
    printf("  names=%zu  root_children=%zu  keys=[", d.NameCount(), d.Root().children.size());
    for (uint32_t off : d.Root().children) { const BjNode& c = d.ParseNode(off); printf("%s ", c.name.c_str()); }
    printf("]\n");

    // Actor count
    uint32_t actorArrOff = 0;
    if (!d.FindRootChild("Actor", actorArrOff)) { printf("  [FAIL] no Actor root\n"); return 1; }
    const BjNode& actorArr = d.ParseNode(actorArrOff);
    size_t actorCount = actorArr.children.size();
    // count LookPoint by type field
    size_t lpCount = 0; std::string firstLp;
    for (uint32_t aoff : actorArr.children) {
        const BjNode& a = d.ParseNode(aoff);
        uint32_t tOff;
        if (d.FindNamedChild(a, "type", tOff)) {
            const BjNode& t = d.ParseNode(tOff);
            if (t.strValue == "LookPoint") {
                ++lpCount;
                if (firstLp.empty()) { uint32_t nOff; if (d.FindNamedChild(a, "name", nOff)) firstLp = d.ParseNode(nOff).strValue; }
            }
        }
    }
    printf("  Actor count=%zu (expect 400)  LookPoint=%zu (expect 25)  first LP name=%s\n",
           actorCount, lpCount, firstLp.c_str());
    if (actorCount != 400) ++fail;
    if (lpCount != 25) ++fail;

    // ActorIDs.LookPoint
    uint32_t aidOff = 0; int lpNext = -1;
    if (d.FindRootChild("ActorIDs", aidOff)) {
        const BjNode& aid = d.ParseNode(aidOff);
        for (uint32_t eoff : aid.children) {
            const BjNode& e = d.ParseNode(eoff);
            uint32_t nmOff, idOff;
            if (d.FindNamedChild(e, "name", nmOff) && d.ParseNode(nmOff).strValue == "LookPoint") {
                if (d.FindNamedChild(e, "id", idOff)) lpNext = (int)d.ParseNode(idOff).numValue;
            }
        }
    }
    printf("  ActorIDs.LookPoint(next)=%d (expect 45)\n", lpNext);
    if (lpNext != 45) ++fail;

    printf("  %s\n", fail == 0 ? "[OK] 语义全部符合" : "[FAIL] 语义不符");
    return fail;
}

static int test_p2_patch() {
    printf("\n=== P2 BjsonPatcher (字节对照 golden) ===\n");
    FpacReader r;
    if (!r.Open(std::wstring(GAME) + L"\\pac\\steam\\scene.pac")) { printf("  [FAIL] open scene.pac\n"); return 1; }
    std::vector<uint8_t> bytes;
    if (!r.ReadEntry("scene/mp4000_sys.json", bytes)) { printf("  [FAIL] read entry\n"); return 1; }
    BjsonDecoder d;
    if (!d.Parse(bytes)) { printf("  [FAIL] parse: %s\n", d.Error().c_str()); return 1; }
    BjsonPatcher p(d);

    // ---- 构造 edited Actor:克隆首个 LookPoint + 覆盖字段(预演 scene_merge)----
    uint32_t actorOff = 0; d.FindRootChild("Actor", actorOff);
    nlohmann::json editedActor = p.RenderValue(actorOff);   // 400 元素数组
    nlohmann::json tmpl;
    for (auto& a : editedActor) if (a.value("type", std::string()) == "LookPoint") { tmpl = a; break; }
    tmpl["name"] = "LP_test_00";
    tmpl["id"] = 671088685.0;                                // 0x2800002d
    tmpl["translation"] = { {"x", 23.02}, {"y", 0.0}, {"z", -144.36} };
    tmpl["outline_order"] = 31.0;
    tmpl["lp_radius"] = 1.2;
    tmpl["lp_height"] = 0.5;
    editedActor.push_back(tmpl);

    // ---- 构造 edited ActorIDs:LookPoint 45→46 ----
    uint32_t aidOff = 0; d.FindRootChild("ActorIDs", aidOff);
    nlohmann::json editedAid = p.RenderValue(aidOff);
    for (auto& e : editedAid) if (e.value("name", std::string()) == "LookPoint") e["id"] = 46.0;

    // ---- patch(顺序:Actor 先,ActorIDs 后,与 Python 一致)----
    if (!p.PatchRoot("Actor", editedActor)) { printf("  [FAIL] PatchRoot Actor: %s\n", p.Error().c_str()); return 1; }
    if (!p.PatchRoot("ActorIDs", editedAid)) { printf("  [FAIL] PatchRoot ActorIDs: %s\n", p.Error().c_str()); return 1; }

    const std::vector<uint8_t>& got = p.Bytes();
    std::vector<uint8_t> gold;
    if (!read_file(L"D:\\destop\\ce修改器\\Sora1\\mod_output\\scene_lptest\\mp4000_sys.json", gold)) {
        printf("  [WARN] no golden (got=%zu)\n", got.size()); return 0;
    }
    const int diff = first_diff(got, gold);
    const bool eq = (diff < 0);
    printf("  [%s] got=%zu golden=%zu%s\n", eq ? "OK" : "FAIL", got.size(), gold.size(),
           eq ? "" : ("  first_diff=" + std::to_string(diff)).c_str());
    return eq ? 0 : 1;
}

static int test_p3_tbl() {
    printf("\n=== P3 TblCodec (字节对照 golden) ===\n");
    FpacReader r;
    if (!r.Open(std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac")) { printf("  [FAIL] open table_sc.pac\n"); return 1; }
    std::vector<uint8_t> bytes;
    if (!r.ReadEntry("table_sc/t_lookpoint.tbl", bytes)) { printf("  [FAIL] read entry\n"); return 1; }
    std::vector<LpRow> rows; std::string err;
    if (!TblCodec::DecodeLookPoint(bytes, rows, err)) { printf("  [FAIL] decode: %s\n", err.c_str()); return 1; }
    printf("  decoded rows=%zu (expect 301)\n", rows.size());
    // 加 LP_test_00 钩子(与 Python 产 golden 一致)
    LpRow hook; hook.text1 = "mp4000"; hook.text2 = "LP_test_00"; hook.text3 = "\xe2\x97\x86\xe6\xb5\x8b\xe8\xaf\x95\xe8\xa7\x86\xe7\x82\xb9"; // ◆测试视点(UTF-8)
    rows.push_back(hook);
    std::vector<uint8_t> got = TblCodec::EncodeLookPoint(rows);
    std::vector<uint8_t> gold;
    if (!read_file(L"C:\\Users\\a1354\\AppData\\Local\\Temp\\claude\\D--destop-ce----Sora1\\b43fdd1d-c098-43ff-ab16-12905d902e9b\\scratchpad\\t_lookpoint.tbl", gold)) {
        printf("  [WARN] no golden (got=%zu)\n", got.size()); return 0;
    }
    const int diff = first_diff(got, gold);
    const bool eq = (diff < 0);
    printf("  [%s] got=%zu golden=%zu%s\n", eq ? "OK" : "FAIL", got.size(), gold.size(),
           eq ? "" : ("  first_diff=" + std::to_string(diff)).c_str());
    return eq ? 0 : 1;
}

static int test_p4_merge() {
    printf("\n=== P4 scene_merge (精简json驱动,字节对照两个 golden) ===\n");
    PatchConfig cfg; std::string err;
    if (!LoadPatchConfig(L"D:\\destop\\ce修改器\\Sora1\\mod_output\\scene_lptest\\scene_patch.json", cfg, err)) {
        printf("  [FAIL] load patch: %s\n", err.c_str()); return 1;
    }
    printf("  cfg: mod=%s target=%s map=%s actors=%zu\n", cfg.mod.c_str(), cfg.target.c_str(), cfg.MapName().c_str(), cfg.addActors.size());
    int fail = 0;
    FpacReader rs, rt;
    if (!rs.Open(std::wstring(GAME) + L"\\pac\\steam\\scene.pac") || !rt.Open(std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac")) { printf("  [FAIL] open pac\n"); return 1; }
    std::vector<uint8_t> origScene, origTbl;
    rs.ReadEntry("scene/mp4000_sys.json", origScene);
    rt.ReadEntry("table_sc/t_lookpoint.tbl", origTbl);

    SceneMergeResult mr = MergeScene(origScene, cfg);
    if (!mr.ok) { printf("  [FAIL] merge: %s\n", mr.err.c_str()); return 1; }

    // scene 对照
    std::vector<uint8_t> sceneGold;
    if (read_file(L"D:\\destop\\ce修改器\\Sora1\\mod_output\\scene_lptest\\mp4000_sys.json", sceneGold)) {
        int diff = first_diff(mr.sceneBytes, sceneGold); bool eq = diff < 0;
        printf("  [%s] scene got=%zu golden=%zu%s\n", eq ? "OK" : "FAIL", mr.sceneBytes.size(), sceneGold.size(),
               eq ? "" : ("  first_diff=" + std::to_string(diff)).c_str());
        if (!eq) ++fail;
    } else printf("  [WARN] no scene golden\n");

    // tbl:原表 decode + 派生钩子 + encode,对照 golden
    std::vector<LpRow> rows; std::string e2;
    if (!TblCodec::DecodeLookPoint(origTbl, rows, e2)) { printf("  [FAIL] tbl decode: %s\n", e2.c_str()); return 1; }
    for (const LpRow& h : mr.tblHooks) rows.push_back(h);
    std::vector<uint8_t> tblOut = TblCodec::EncodeLookPoint(rows);
    std::vector<uint8_t> tblGold;
    if (read_file(L"C:\\Users\\a1354\\AppData\\Local\\Temp\\claude\\D--destop-ce----Sora1\\b43fdd1d-c098-43ff-ab16-12905d902e9b\\scratchpad\\t_lookpoint.tbl", tblGold)) {
        int diff = first_diff(tblOut, tblGold); bool eq = diff < 0;
        printf("  [%s] tbl got=%zu golden=%zu%s\n", eq ? "OK" : "FAIL", tblOut.size(), tblGold.size(),
               eq ? "" : ("  first_diff=" + std::to_string(diff)).c_str());
        if (!eq) ++fail;
    } else printf("  [WARN] no tbl golden\n");
    return fail;
}

static const wchar_t* SCRATCH = L"C:\\Users\\a1354\\AppData\\Local\\Temp\\claude\\D--destop-ce----Sora1\\b43fdd1d-c098-43ff-ab16-12905d902e9b\\scratchpad";

static int test_p5_orchestrator() {
    printf("\n=== P5 orchestrator (扫plugins→cache,字节对照+指纹跳过) ===\n");
    int fail = 0;
    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = std::wstring(SCRATCH) + L"\\p5_plugins";
    paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = std::wstring(SCRATCH) + L"\\p5_cache";
    paths.schemasDir = SCHEMAS;
    { std::error_code ec; std::filesystem::remove_all(paths.cacheDir, ec); }  // 清缓存,模拟首次

    auto r1 = ed9loader::modkit::orchestrator::Run(paths, /*force*/false);
    printf("  run1: mods=%d merged=%d failed=%d\n", r1.mods, r1.merged, r1.failed);
    if (r1.merged < 1 || r1.failed > 0) { printf("  [FAIL] run1\n  log:\n%s\n", r1.log.c_str()); return 1; }

    std::vector<uint8_t> got, gold;
    if (read_file(paths.cacheDir + L"\\scene\\mp4000_sys.json", got) &&
        read_file(L"D:\\destop\\ce修改器\\Sora1\\mod_output\\scene_lptest\\mp4000_sys.json", gold)) {
        int diff = first_diff(got, gold); bool eq = diff < 0;
        printf("  [%s] cache scene got=%zu golden=%zu%s\n", eq ? "OK" : "FAIL", got.size(), gold.size(),
               eq ? "" : ("  first_diff=" + std::to_string(diff)).c_str());
        if (!eq) ++fail;
    } else { printf("  [FAIL] no cache scene / golden\n"); ++fail; }

    if (read_file(paths.cacheDir + L"\\table_sc\\t_lookpoint.tbl", got) &&
        read_file(std::wstring(SCRATCH) + L"\\t_lookpoint.tbl", gold)) {
        int diff = first_diff(got, gold); bool eq = diff < 0;
        printf("  [%s] cache tbl got=%zu golden=%zu%s\n", eq ? "OK" : "FAIL", got.size(), gold.size(),
               eq ? "" : ("  first_diff=" + std::to_string(diff)).c_str());
        if (!eq) ++fail;
    } else { printf("  [FAIL] no cache tbl / golden\n"); ++fail; }

    auto r2 = ed9loader::modkit::orchestrator::Run(paths, /*force*/false);
    printf("  run2(指纹): skipped=%d merged=%d\n", r2.skipped, r2.merged);
    if (r2.skipped < 1) { printf("  [FAIL] expected fingerprint skip\n"); ++fail; }
    else printf("  [OK] 指纹缓存生效(未改→跳过)\n");
    return fail;
}

static int test_p7_tblpatch() {
    printf("\n=== P7 tbl_patch.json 已弃用(编排器只读 tbl\\*.json,应被忽略) ===\n");
    int fail = 0;
    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = std::wstring(SCRATCH) + L"\\p7_plugins";
    paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = std::wstring(SCRATCH) + L"\\p7_cache";
    paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  mods=%d merged=%d failed=%d\n", r.mods, r.merged, r.failed);
    if (r.failed > 0) { printf("  [FAIL]\n  log:\n%s\n", r.log.c_str()); return 1; }

    std::vector<uint8_t> tbl;
    if (!read_file(paths.cacheDir + L"\\table_sc\\t_lookpoint.tbl", tbl)) { printf("  [FAIL] no cache tbl\n"); return 1; }
    std::vector<LpRow> rows; std::string err;
    if (!TblCodec::DecodeLookPoint(tbl, rows, err)) { printf("  [FAIL] decode: %s\n", err.c_str()); return 1; }
    bool hasScene = false, hasTbl = false;
    for (const auto& row : rows) { if (row.text2 == "LP_test_00") hasScene = true; if (row.text2 == "LP_tblpatch_test") hasTbl = true; }
    printf("  rows=%zu (expect 302)  scene派生 LP_test_00=%d  tbl_patch LP_tblpatch_test=%d(应0)\n",
           rows.size(), (int)hasScene, (int)hasTbl);
    if (rows.size() != 302) ++fail;     // 仅 scene 派生 +1,tbl_patch 不再注入
    if (!hasScene) ++fail;              // scene 派生行仍应在
    if (hasTbl) ++fail;                 // tbl_patch.json 已弃用,绝不应出现
    printf("  %s\n", fail == 0 ? "[OK] tbl_patch.json 被忽略,仅 scene 派生生效" : "[FAIL]");
    return fail;
}

static int test_p8_scriptinject() {
    printf("\n=== P8 add_dat_ini(自包含:根 dat→中性 script, dat\\sc\\→语言 script_sc, 注入表)===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p8_plugins";
    std::wstring cache = std::wstring(SCRATCH) + L"\\p8_cache\\merged";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(std::wstring(SCRATCH) + L"\\p8_cache", ec);
      std::filesystem::create_directories(root + L"\\InjMod\\dat\\sc", ec); }
    // fixture:add_dat_ini.json 声明两条注入;MyTalk.dat 放根(中性),ScTalk.dat 放 dat\sc\(简中)
    { std::ofstream f(root + L"\\InjMod\\add_dat_ini.json", std::ios::binary);
      f << "{ \"inject\": [ {\"map\":\"mp4000\",\"script\":\"MyTalk\"}, {\"map\":\"mp4000\",\"script\":\"ScTalk\"} ] }\n"; }
    { std::ofstream f(root + L"\\InjMod\\MyTalk.dat", std::ios::binary); f << "NEUTRAL_DAT_BODY"; }
    { std::ofstream f(root + L"\\InjMod\\dat\\sc\\ScTalk.dat", std::ios::binary); f << "SC_DAT_BODY_xyz"; }

    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root;
    paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = cache;
    paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  mods=%d injected=%d failed=%d\n", r.mods, r.injected, r.failed);

    // 1) 根 MyTalk.dat → 中性 script\MyTalk.dat(无 scena 层)
    std::vector<uint8_t> got;
    bool n = read_file(cache + L"\\script\\MyTalk.dat", got) && std::string(got.begin(), got.end()) == "NEUTRAL_DAT_BODY";
    printf("  [%s] 中性 script\\MyTalk.dat\n", n ? "OK" : "FAIL"); if (!n) ++fail;

    // 2) dat\sc\ScTalk.dat → 语言 script_sc\ScTalk.dat(无 scena 层)
    bool sc = read_file(cache + L"\\script_sc\\ScTalk.dat", got) && std::string(got.begin(), got.end()) == "SC_DAT_BODY_xyz";
    printf("  [%s] 简中 script_sc\\ScTalk.dat\n", sc ? "OK" : "FAIL"); if (!sc) ++fail;

    // 3) 注入表含两条(.list 语言中性,只存 map\tscript)
    std::vector<uint8_t> tbl;
    if (read_file(std::wstring(SCRATCH) + L"\\p8_cache\\script_inject.list", tbl)) {
        std::string s(tbl.begin(), tbl.end());
        bool ok = s.find("mp4000\tMyTalk") != std::string::npos && s.find("mp4000\tScTalk") != std::string::npos;
        printf("  [%s] script_inject.list 含 MyTalk + ScTalk\n", ok ? "OK" : "FAIL");
        if (!ok) { printf("    table:\n%s\n", s.c_str()); ++fail; }
    } else { printf("  [FAIL] no script_inject.list\n"); ++fail; }

    printf("  %s\n", fail == 0 ? "[OK] add_dat_ini 注入(中性 + 语言)" : "[FAIL]");
    return fail;
}

// 构造一行 LookPoint 通用行(字段名同 schema)
static TblRowG mkLookPointRow(const std::string& map, const std::string& name, const std::string& label) {
    auto S = [](std::string v) { TblValue x; x.kind = TblValue::K::Str; x.s = std::move(v); return x; };
    auto I = [](int64_t v) { TblValue x; x.kind = TblValue::K::Int; x.i = v; return x; };
    auto A = []() { TblValue x; x.kind = TblValue::K::Arr; return x; };
    TblRowG r;
    r.fields = { {"text1", S(map)}, {"text2", S(name)}, {"text3", S(label)}, {"empty", S("")},
                 {"arr1", A()}, {"uint1", I(0)}, {"arr2", A()}, {"uint2", I(0)} };
    return r;
}

// 把通用 TblFileG 序列化成可比字符串(语义对照,池布局无关)
static std::string serializeG(const TblFileG& t) {
    std::string s;
    for (const auto& tb : t.tables) {
        s += "T:" + tb.name + ":" + std::to_string(tb.rows.size()) + "\n";
        for (const auto& row : tb.rows) {
            for (const auto& kv : row.fields) {
                const TblValue& v = kv.second;
                s += kv.first + "=";
                switch (v.kind) {
                    case TblValue::K::Int: s += "i" + std::to_string(v.i); break;
                    case TblValue::K::Flt: s += "f" + std::to_string(v.f); break;
                    case TblValue::K::Str: s += "s" + v.s; break;
                    case TblValue::K::Arr: { s += "a["; for (auto e : v.arr) s += std::to_string(e) + ","; s += "]"; } break;
                    case TblValue::K::Raw: { s += "r" + std::to_string(v.raw.size()); } break;
                }
                s += ";";
            }
            s += "\n";
        }
    }
    return s;
}

static int test_tblg_lookpoint() {
    printf("\n=== TBLG1 通用引擎 ≡ LookPoint 专用编码器(字节对照) ===\n");
    std::vector<uint8_t> orig;
    if (!read_file(std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac", orig)) {} // 占位,改从 pac 读
    FpacReader r;
    if (!r.Open(std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac") || !r.ReadEntry("table_sc/t_lookpoint.tbl", orig)) {
        printf("  [FAIL] read t_lookpoint from pac\n"); return 1;
    }
    const std::string label = "\xe2\x97\x86\xe6\xb5\x8b\xe8\xaf\x95\xe8\xa7\x86\xe7\x82\xb9"; // ◆测试视点
    int fail = 0;

    // 专用路径
    std::vector<LpRow> rows; std::string e1;
    if (!TblCodec::DecodeLookPoint(orig, rows, e1)) { printf("  [FAIL] DecodeLookPoint: %s\n", e1.c_str()); return 1; }
    LpRow hook; hook.text1 = "mp4000"; hook.text2 = "LP_test_00"; hook.text3 = label;
    rows.push_back(hook);
    std::vector<uint8_t> bytesA = TblCodec::EncodeLookPoint(rows);

    // 通用路径
    TblFileG g; std::string e2;
    if (!DecodeTblG(orig, SCHEMAS, "Sora1", g, e2)) { printf("  [FAIL] DecodeTblG: %s\n", e2.c_str()); return 1; }
    printf("  通用解码: tables=%zu  rows[0]=%zu  schema=%s/%s\n", g.tables.size(),
           g.tables.empty() ? 0 : g.tables[0].rows.size(),
           g.tables.empty() ? "?" : g.tables[0].schema.game.c_str(),
           g.tables.empty() ? "?" : g.tables[0].schema.variant.c_str());
    if (g.tables.size() != 1 || g.tables[0].name != "LookPointTableData") { printf("  [FAIL] table\n"); return 1; }
    g.tables[0].rows.push_back(mkLookPointRow("mp4000", "LP_test_00", label));
    std::vector<uint8_t> bytesB = EncodeTblG(g);

    int d = first_diff(bytesA, bytesB); bool eq = d < 0;
    printf("  [%s] 专用=%zu 通用=%zu%s\n", eq ? "OK" : "FAIL", bytesA.size(), bytesB.size(),
           eq ? "" : ("  first_diff=" + std::to_string(d)).c_str());
    if (!eq) ++fail;
    return fail;
}

static int test_tblg_roundtrip() {
    printf("\n=== TBLG2 通用引擎语义往返(decode→encode→decode 稳定) ===\n");
    const char* tables[] = { "t_chapter", "t_eventbox", "t_mapjump", "t_tbox",
                             "t_item", "t_skill", "t_condition_info", "t_shop", "t_minigame_fishing", "t_notemenu" };
    int fail = 0;
    FpacReader r;
    if (!r.Open(std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac")) { printf("  [FAIL] open table_sc.pac\n"); return 1; }
    for (const char* tn : tables) {
        std::vector<uint8_t> orig;
        std::string entry = std::string("table_sc/") + tn + ".tbl";
        if (!r.ReadEntry(entry, orig)) { printf("  [WARN] no pac entry %s\n", entry.c_str()); continue; }
        TblFileG g1; std::string e1;
        if (!DecodeTblG(orig, SCHEMAS, "Sora1", g1, e1)) { printf("  [FAIL] %s decode1: %s\n", tn, e1.c_str()); ++fail; continue; }
        std::vector<uint8_t> reb = EncodeTblG(g1);
        TblFileG g2; std::string e2;
        if (!DecodeTblG(reb, SCHEMAS, "Sora1", g2, e2)) { printf("  [FAIL] %s decode2: %s\n", tn, e2.c_str()); ++fail; continue; }
        bool eq = serializeG(g1) == serializeG(g2);
        size_t rc = g1.tables.empty() ? 0 : g1.tables[0].rows.size();
        printf("  [%s] %-12s tables=%zu rows=%zu  orig=%zu reb=%zu  schema=%s\n",
               eq ? "OK" : "FAIL", tn, g1.tables.size(), rc, orig.size(), reb.size(),
               g1.tables.empty() ? "?" : g1.tables[0].schema.variant.c_str());
        if (!eq) ++fail;
    }
    return fail;
}

// —— TBL⇄JSON 双向往返(验证 mod_manager 的 tbl→json→tbl;算法与 mod_manager.cpp 保持一致)——
namespace {
using tjson = nlohmann::ordered_json;
static tjson tvToJson(const TblValue& v) {
    using K = TblValue::K;
    switch (v.kind) {
        case K::Int: return v.i;
        case K::Flt: return v.f;
        case K::Str: return v.s;
        case K::Arr: { tjson a = tjson::array(); for (auto x : v.arr) a.push_back(x); return a; }
        default:     { tjson a = tjson::array(); for (auto b : v.raw) a.push_back((int)b); return a; }
    }
}
static tjson gToJson(const TblFileG& g) {
    tjson j; j["file"] = "x"; j["tables"] = tjson::array();
    for (const auto& t : g.tables) {
        tjson tj; tj["table"] = t.name; tj["rows"] = tjson::array();
        for (const auto& r : t.rows) { tjson rj = tjson::object(); for (const auto& kv : r.fields) rj[kv.first] = tvToJson(kv.second); tj["rows"].push_back(std::move(rj)); }
        j["tables"].push_back(std::move(tj));
    }
    return j;
}
static TblValue jvToTv(const std::string& type, const tjson& jv) {
    using K = TblValue::K; TblValue v;
    if (TblTypeIsToffset(type)) { v.kind = K::Str; if (jv.is_string()) v.s = jv.get<std::string>(); }
    else if (TblTypeIsArray(type)) { v.kind = K::Arr; if (jv.is_array()) for (const auto& e : jv) if (e.is_number()) v.arr.push_back((uint64_t)e.get<int64_t>()); }
    else if (TblTypeIsData(type)) { v.kind = K::Raw; if (jv.is_array()) for (const auto& e : jv) if (e.is_number()) v.raw.push_back((uint8_t)(e.get<int>() & 0xff)); }
    else if (TblTypeIsFloat(type)) { v.kind = K::Flt; if (jv.is_number()) v.f = jv.get<double>(); }
    else { v.kind = K::Int; if (jv.is_number()) v.i = jv.get<int64_t>(); }
    return v;
}
static bool jsonToG(const tjson& j, TblFileG& out, std::string& err) {
    for (const auto& tj : j["tables"]) {
        std::string name = tj["table"].get<std::string>();
        TblSchemaDef schema; if (!ResolveTblSchema(SCHEMAS, name, 0, "Sora1", schema, err)) return false;
        TblTableG t; t.name = name; t.schema = schema;
        static const tjson kNull;
        for (const auto& rj : tj["rows"]) {
            TblRowG row;
            for (const auto& f : schema.fields) { auto it = rj.find(f.name); row.fields.emplace_back(f.name, jvToTv(f.type, it != rj.end() ? it.value() : kNull)); }
            t.rows.push_back(std::move(row));
        }
        out.tables.push_back(std::move(t));
    }
    return true;
}
} // namespace
static int test_tblg_json_roundtrip() {
    printf("\n=== TBL⇄JSON 双向往返(decode→json→文本→json→encode→decode 稳定) ===\n");
    const char* tables[] = { "t_chapter", "t_item", "t_skill", "t_shop", "t_tbox", "t_mapjump", "t_eventbox" };
    int fail = 0; FpacReader r;
    if (!r.Open(std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac")) { printf("  [FAIL] open table_sc.pac\n"); return 1; }
    for (const char* tn : tables) {
        std::vector<uint8_t> orig;
        if (!r.ReadEntry(std::string("table_sc/") + tn + ".tbl", orig)) { printf("  [WARN] no pac entry %s\n", tn); continue; }
        TblFileG g1; std::string e;
        if (!DecodeTblG(orig, SCHEMAS, "Sora1", g1, e)) { printf("  [FAIL] %s decode: %s\n", tn, e.c_str()); ++fail; continue; }
        std::string js = gToJson(g1).dump(2);                                  // TblFileG → JSON 文本
        tjson jp; try { jp = tjson::parse(js); } catch (...) { printf("  [FAIL] %s json parse\n", tn); ++fail; continue; }
        TblFileG g2; if (!jsonToG(jp, g2, e)) { printf("  [FAIL] %s json->g: %s\n", tn, e.c_str()); ++fail; continue; }
        std::vector<uint8_t> reb = EncodeTblG(g2);                             // JSON→TblFileG → tbl
        TblFileG g3; if (!DecodeTblG(reb, SCHEMAS, "Sora1", g3, e)) { printf("  [FAIL] %s decode2: %s\n", tn, e.c_str()); ++fail; continue; }
        bool eq = serializeG(g1) == serializeG(g3);                           // 功能等价?
        printf("  [%s] %-12s rows=%zu  orig=%zu reb=%zu\n", eq ? "OK" : "FAIL", tn,
               g1.tables.empty() ? 0 : g1.tables[0].rows.size(), orig.size(), reb.size());
        if (!eq) ++fail;
    }
    return fail;
}

static int test_p9_tbl_generic() {
    printf("\n=== P9 通用 tbl\\*.json(add_rows + edit_rows 端到端) ===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p9_plugins";
    std::wstring tblDir = root + L"\\MyMod\\tbl";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(std::wstring(SCRATCH) + L"\\p9_cache", ec); std::filesystem::create_directories(tblDir, ec); }
    {   // fixture: 给 t_chapter 加一行 + 改 id=0 行的 text4
        std::ofstream f(tblDir + L"\\t_chapter.json", std::ios::binary);
        f << "{\n"
             "  \"add_rows\": [ {\"id\": 99, \"int1\": 12345, \"name\": \"测试章节\"} ],\n"
             "  \"edit_rows\": [ {\"match\": {\"id\": 0}, \"set\": {\"name3\": \"改后END\"}} ]\n"
             "}\n";
    }
    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root;
    paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = std::wstring(SCRATCH) + L"\\p9_cache\\merged";
    paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  mods=%d tbls=%d failed=%d\n", r.mods, r.tbls, r.failed);
    if (r.tbls < 1 || r.failed > 0) { printf("  [FAIL]\n  log:\n%s\n", r.log.c_str()); return 1; }

    std::vector<uint8_t> out;
    if (!read_file(paths.cacheDir + L"\\table_sc\\t_chapter.tbl", out)) { printf("  [FAIL] no cache t_chapter.tbl\n"); return 1; }
    TblFileG g; std::string e;
    if (!DecodeTblG(out, SCHEMAS, "Sora1", g, e)) { printf("  [FAIL] decode: %s\n", e.c_str()); return 1; }
    auto& rows = g.tables[0].rows;
    printf("  rows=%zu (expect 7)\n", rows.size());
    if (rows.size() != 7) ++fail;

    const TblValue* id = rows.back().find("id"); const TblValue* t2 = rows.back().find("name");
    bool addOk = id && id->i == 99 && t2 && t2->s == "测试章节";
    printf("  [%s] add_row: id=%lld name=\"%s\"\n", addOk ? "OK" : "FAIL", id ? (long long)id->i : -1, t2 ? t2->s.c_str() : "?");
    if (!addOk) ++fail;

    bool editOk = false;
    for (auto& row : rows) { const TblValue* i = row.find("id"); const TblValue* t4 = row.find("name3"); if (i && i->i == 0 && t4 && t4->s == "改后END") editOk = true; }
    printf("  [%s] edit_row: id=0 → name3=\"改后END\"\n", editOk ? "OK" : "FAIL");
    if (!editOk) ++fail;
    return fail;
}

// P10:多 mod 改同表同行同字段 → 冲突检测 + mods.json 加载顺序/启停决定赢家。
static int test_p10_conflict() {
    printf("\n=== P10 多 mod 冲突检测 + mods.json 加载顺序 ===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p10_plugins";
    std::wstring cacheRoot = std::wstring(SCRATCH) + L"\\p10_cache";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(cacheRoot, ec);
      std::filesystem::create_directories(root + L"\\AAA\\tbl", ec);
      std::filesystem::create_directories(root + L"\\BBB\\tbl", ec); }
    { std::ofstream f(root + L"\\AAA\\tbl\\t_chapter.json", std::ios::binary);
      f << "{ \"edit_rows\": [ {\"match\":{\"id\":0}, \"set\":{\"name3\":\"FROM_A\"}} ] }\n"; }
    { std::ofstream f(root + L"\\BBB\\tbl\\t_chapter.json", std::ios::binary);
      f << "{ \"edit_rows\": [ {\"match\":{\"id\":0}, \"set\":{\"name3\":\"FROM_B\"}} ] }\n"; }

    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root;
    paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = cacheRoot + L"\\merged";
    paths.schemasDir = SCHEMAS;

    auto winner = [&]() -> std::string {
        std::vector<uint8_t> out; if (!read_file(paths.cacheDir + L"\\table_sc\\t_chapter.tbl", out)) return "<no tbl>";
        TblFileG g; std::string e; if (!DecodeTblG(out, SCHEMAS, "Sora1", g, e)) return "<decode fail>";
        for (auto& row : g.tables[0].rows) { const TblValue* i = row.find("id"); const TblValue* n = row.find("name3"); if (i && i->i == 0 && n) return n->s; }
        return "<no row id=0>";
    };

    // run1:无 mods.json → 默认字母序 AAA<BBB,BBB 后套用→胜
    auto r1 = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  run1: mods=%d tbls=%d conflicts=%d failed=%d\n", r1.mods, r1.tbls, r1.conflicts, r1.failed);
    if (r1.failed > 0 || r1.tbls < 1) { printf("  [FAIL]\n  log:\n%s\n", r1.log.c_str()); return 1; }
    if (r1.conflicts != 1) { printf("  [FAIL] expect 1 conflict got %d\n", r1.conflicts); ++fail; }
    std::string w1 = winner();
    printf("  [%s] run1 winner name3=\"%s\" (字母序后者 BBB 胜→ expect FROM_B)\n", w1 == "FROM_B" ? "OK" : "FAIL", w1.c_str());
    if (w1 != "FROM_B") ++fail;

    nlohmann::json rep;
    if (read_json(cacheRoot + L"\\merge_report.json", rep)) {
        bool found = false;
        for (auto& t : rep.value("tables", nlohmann::json::array()))
            if (t.value("table", std::string()) == "t_chapter")
                for (auto& c : t.value("conflicts", nlohmann::json::array()))
                    if (c.value("field", std::string()) == "name3" && c.value("byMod", std::string()) == "BBB" && c.value("fromMod", std::string()) == "AAA") found = true;
        printf("  [%s] merge_report.json 含 t_chapter.name3 冲突(AAA->BBB)\n", found ? "OK" : "FAIL");
        if (!found) ++fail;
    } else { printf("  [FAIL] no merge_report.json\n"); ++fail; }

    // run2:写 mods.json 把 AAA 排到 BBB 之后 → AAA 后套用→胜
    { std::ofstream f(root + L"\\mods.json", std::ios::binary);
      f << "{ \"version\":1, \"mods\":[ {\"name\":\"BBB\",\"enabled\":true}, {\"name\":\"AAA\",\"enabled\":true} ] }\n"; }
    auto r2 = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    std::string w2 = winner();
    printf("  [%s] run2 winner name3=\"%s\" (mods.json 把 AAA 置后→ expect FROM_A) conflicts=%d\n", w2 == "FROM_A" ? "OK" : "FAIL", w2.c_str(), r2.conflicts);
    if (w2 != "FROM_A") ++fail;

    // run3:禁用 BBB → 无冲突,AAA 生效
    { std::ofstream f(root + L"\\mods.json", std::ios::binary);
      f << "{ \"version\":1, \"mods\":[ {\"name\":\"AAA\",\"enabled\":true}, {\"name\":\"BBB\",\"enabled\":false} ] }\n"; }
    auto r3 = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    std::string w3 = winner();
    printf("  [%s] run3 禁用 BBB: conflicts=%d winner=\"%s\" (expect 0 冲突, FROM_A)\n",
           (r3.conflicts == 0 && w3 == "FROM_A") ? "OK" : "FAIL", r3.conflicts, w3.c_str());
    if (r3.conflicts != 0 || w3 != "FROM_A") ++fail;

    printf("  %s\n", fail == 0 ? "[OK] 冲突检测 + mods.json 顺序/启停" : "[FAIL]");
    return fail;
}

// P11:Mod\<mod>\dat\*.dat 整文件替换原版脚本 + 同名多 mod 冲突(纯文件操作,不需 pac)。
static int test_p11_dat_replace() {
    printf("\n=== P11 dat 替换(Mod\\<mod>\\dat\\*.dat 整文件 + 冲突) ===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p11_plugins";
    std::wstring cacheRoot = std::wstring(SCRATCH) + L"\\p11_cache";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(cacheRoot, ec);
      std::filesystem::create_directories(root + L"\\AAA\\dat", ec);
      std::filesystem::create_directories(root + L"\\BBB\\dat", ec); }
    auto writeBytes = [](const std::wstring& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; };
    writeBytes(root + L"\\AAA\\dat\\mp0001.dat", "AAA_mp0001");  // 共享名:与 BBB 冲突
    writeBytes(root + L"\\AAA\\dat\\onlyA.dat",  "AAA_only");
    writeBytes(root + L"\\BBB\\dat\\mp0001.dat", "BBB_mp0001");

    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root;
    paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";  // 本测试只有 dat\,不会用到 pac
    paths.cacheDir = cacheRoot + L"\\merged";
    paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  mods=%d conflicts=%d failed=%d\n", r.mods, r.conflicts, r.failed);

    auto readStr = [](const std::wstring& p) -> std::string { std::vector<uint8_t> b; if (!read_file(p, b)) return "<none>"; return std::string(b.begin(), b.end()); };
    std::string shared = readStr(paths.cacheDir + L"\\script\\mp0001.dat");
    std::string only   = readStr(paths.cacheDir + L"\\script\\onlyA.dat");
    printf("  [%s] mp0001.dat = \"%s\" (BBB 后加载应胜→BBB_mp0001)\n", shared == "BBB_mp0001" ? "OK" : "FAIL", shared.c_str());
    if (shared != "BBB_mp0001") ++fail;
    printf("  [%s] onlyA.dat = \"%s\"\n", only == "AAA_only" ? "OK" : "FAIL", only.c_str());
    if (only != "AAA_only") ++fail;
    if (r.conflicts < 1) { printf("  [FAIL] expect >=1 dat conflict\n"); ++fail; }

    nlohmann::json rep;
    if (read_json(cacheRoot + L"\\merge_report.json", rep)) {
        bool found = false;
        for (auto& dc : rep.value("datConflicts", nlohmann::json::array()))
            if (dc.value("name", std::string()) == "mp0001" && dc.value("byMod", std::string()) == "BBB" && dc.value("fromMod", std::string()) == "AAA") found = true;
        printf("  [%s] datConflicts 含 mp0001 (AAA->BBB)\n", found ? "OK" : "FAIL"); if (!found) ++fail;
    } else { printf("  [FAIL] no merge_report.json\n"); ++fail; }

    printf("  %s\n", fail == 0 ? "[OK] dat 替换 + 冲突" : "[FAIL]");
    return fail;
}

// P12:clone_rows 池感知克隆(对真实 t_npc_mp4000)+ 池表护栏(add_rows 应被拒)。
static int test_p12_npc_clone() {
    printf("\n=== P12 NPC clone_rows(池感知)+ 池表护栏 ===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p12_plugins";
    std::wstring tblDir = root + L"\\NpcMod\\tbl";
    std::wstring cacheRoot = std::wstring(SCRATCH) + L"\\p12_cache";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(cacheRoot, ec); std::filesystem::create_directories(tblDir, ec); }
    { std::ofstream f(tblDir + L"\\t_npc_mp4000.json", std::ios::binary);   // 用语义别名 model/talk
      f << "{ \"table\":\"NPCParam\", \"clone_rows\":[ {\"from_index\":0, \"set\":{\"model\":12000,\"pos_x\":15.88,\"pos_y\":0.0,\"pos_z\":-144.21,\"talk\":\"MyNpc.TK_TEST\"}} ] }\n"; }
    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root; paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = cacheRoot + L"\\merged"; paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  clone: mods=%d tbls=%d failed=%d\n", r.mods, r.tbls, r.failed);
    if (r.failed > 0 || r.tbls < 1) { printf("  [FAIL]\n  log:\n%s\n", r.log.c_str()); return 1; }
    std::vector<uint8_t> out;
    if (!read_file(paths.cacheDir + L"\\table_sc\\t_npc_mp4000.tbl", out)) { printf("  [FAIL] no cache t_npc\n"); return 1; }
    uint32_t cnt; memcpy(&cnt, &out[84], 4);
    size_t start = 88, RL = 160;
    size_t crow = start + (size_t)(cnt - 1) * RL;
    float px; memcpy(&px, &out[crow + 0x2C], 4);
    printf("  [%s] count=%u(expect 1202)  克隆行 pos_x=%.2f(expect 15.88)  size=%zu\n",
           (cnt == 1202 && px > 15.87f && px < 15.89f) ? "OK" : "FAIL", cnt, px, out.size());
    if (cnt != 1202 || px < 15.87f || px > 15.89f) ++fail;
    // 字符串追加:克隆行 resource_ref_78 偏移应指向新追加的 "MyNpc.TK_TEST"
    uint64_t talkOff; memcpy(&talkOff, &out[crow + 0x78], 8);
    std::string talk = (talkOff < out.size()) ? std::string((const char*)&out[talkOff]) : "<bad>";
    printf("  [%s] 别名 talk -> resource_ref_78 -> \"%s\"(expect MyNpc.TK_TEST)\n", talk == "MyNpc.TK_TEST" ? "OK" : "FAIL", talk.c_str());
    if (talk != "MyNpc.TK_TEST") ++fail;
    uint32_t pid; memcpy(&pid, &out[crow + 0x00], 4);   // 别名 model -> packed_id
    printf("  [%s] 别名 model -> packed_id = %u(expect 12000)\n", pid == 12000 ? "OK" : "FAIL", pid);
    if (pid != 12000) ++fail;

    // 护栏:对池表用 add_rows 应被拒(failed>0)
    { std::ofstream f(tblDir + L"\\t_npc_mp4000.json", std::ios::binary);
      f << "{ \"table\":\"NPCParam\", \"add_rows\":[ {\"packed_id\":99} ] }\n"; }
    auto r2 = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  [%s] 护栏: add_rows 池表 failed=%d(expect >0)\n", r2.failed > 0 ? "OK" : "FAIL", r2.failed);
    if (r2.failed == 0) ++fail;

    printf("  %s\n", fail == 0 ? "[OK] clone_rows + 护栏" : "[FAIL]");
    return fail;
}

// P13:给 t_name(NameTableData,toffset 表)add_rows 一个自定义角色 → 可读回(自定义名+模型+头像)。
static int test_p13_name_add() {
    printf("\n=== P13 t_name add_rows 自定义角色(名字/模型/头像)===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p13_plugins";
    std::wstring tblDir = root + L"\\NameMod\\tbl";
    std::wstring cacheRoot = std::wstring(SCRATCH) + L"\\p13_cache";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(cacheRoot, ec); std::filesystem::create_directories(tblDir, ec); }
    { std::ofstream f(tblDir + L"\\t_name.json", std::ios::binary);
      f << "{ \"table\":\"NameTableData\", \"add_rows\":[ {\"character_id\":9990001, \"name\":\"测试NPC\", \"model\":\"chr5501_c01\", \"face\":\"chrf502_face\", \"script\":\"chrx001\"} ] }\n"; }
    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root; paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = cacheRoot + L"\\merged"; paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  add: mods=%d tbls=%d failed=%d\n", r.mods, r.tbls, r.failed);
    if (r.failed > 0 || r.tbls < 1) { printf("  [FAIL]\n  log:\n%s\n", r.log.c_str()); return 1; }
    std::vector<uint8_t> out; TblFileG g; std::string e;
    if (!read_file(paths.cacheDir + L"\\table_sc\\t_name.tbl", out) || !DecodeTblG(out, SCHEMAS, "Sora1", g, e)) { printf("  [FAIL] decode: %s\n", e.c_str()); return 1; }
    bool found = false;
    for (auto& row : g.tables[0].rows) {
        const TblValue* id = row.find("character_id"); const TblValue* nm = row.find("name"); const TblValue* md = row.find("model");
        if (id && id->i == 9990001 && nm && nm->s == "测试NPC" && md && md->s == "chr5501_c01") found = true;
    }
    printf("  [%s] 读回自定义角色 id=9990001 name=测试NPC model=chr5501_c01\n", found ? "OK" : "FAIL");
    if (!found) ++fail;
    printf("  %s\n", fail == 0 ? "[OK] t_name add_rows 可用" : "[FAIL]");
    return fail;
}

// P14:add_npc 从零构造 NPC(借 behavior_from 的 flag-tag,资源 ref 按配置/清空)。
static int test_p14_add_npc() {
    printf("\n=== P14 add_npc 从零构造 NPC ===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p14_plugins";
    std::wstring tblDir = root + L"\\NpcMod\\tbl";
    std::wstring cacheRoot = std::wstring(SCRATCH) + L"\\p14_cache";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(cacheRoot, ec); std::filesystem::create_directories(tblDir, ec); }
    { std::ofstream f(tblDir + L"\\t_npc_mp4000.json", std::ios::binary);
      f << "{ \"table\":\"NPCParam\", \"add_npc\":[ {\"behavior_from\":0, \"model\":12300, \"pos_x\":20.0, \"pos_y\":0.0, \"pos_z\":-144.0, \"yaw_deg\":90.0, \"talk\":\"NpcTalk.TK_X\", \"anim\":\"npc_setting.AniEvKaiwa\"} ] }\n"; }
    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root; paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = cacheRoot + L"\\merged"; paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  add_npc: mods=%d tbls=%d failed=%d\n", r.mods, r.tbls, r.failed);
    if (r.failed > 0 || r.tbls < 1) { printf("  [FAIL]\n  log:\n%s\n", r.log.c_str()); return 1; }
    std::vector<uint8_t> out;
    if (!read_file(paths.cacheDir + L"\\table_sc\\t_npc_mp4000.tbl", out)) { printf("  [FAIL] no cache\n"); return 1; }
    uint32_t cnt; memcpy(&cnt, &out[84], 4);
    size_t crow = 88 + (size_t)(cnt - 1) * 160;
    auto poolstr = [&](size_t off) { uint64_t o; memcpy(&o, &out[crow + off], 8); return (o < out.size()) ? std::string((const char*)&out[o]) : std::string("<bad>"); };
    uint32_t pid; memcpy(&pid, &out[crow + 0x00], 4);
    uint32_t f18; memcpy(&f18, &out[crow + 0x18], 4);
    uint32_t t28; memcpy(&t28, &out[crow + 0x28], 4);
    float px; memcpy(&px, &out[crow + 0x2C], 4);
    std::string talk = poolstr(0x78), anim = poolstr(0x68), look = poolstr(0x60), tag = poolstr(0x58);
    uint32_t p98; memcpy(&p98, &out[crow + 0x98], 4);   // 商人/商店标志,应被清 0(不继承模板诺娜的 1)
    bool ok = (cnt == 1202) && pid == 12300 && f18 == 1 && t28 == 1 && px > 19.9f && px < 20.1f
              && talk == "NpcTalk.TK_X" && anim == "npc_setting.AniEvKaiwa" && look.empty() && tag == "DGPTSQ" && p98 == 0;
    printf("  [%s] count=%u model=%u flags18=%u type28=%u pos_x=%.1f param_98=%u(应0,无商店)\n", ok ? "OK" : "FAIL", cnt, pid, f18, t28, px, p98);
    printf("       talk=\"%s\" anim=\"%s\" lookdist=\"%s\"(应空) flag-tag=\"%s\"(借自row0=DGPTSQ)\n", talk.c_str(), anim.c_str(), look.c_str(), tag.c_str());
    if (!ok) ++fail;
    printf("  %s\n", fail == 0 ? "[OK] add_npc 从零构造" : "[FAIL]");
    return fail;
}

// P15:add_npc 用中文别名键(含「对话」追加自定义字符串)。复现/验证中文别名端到端。
static int test_p15_add_npc_zh() {
    printf("\n=== P15 add_npc 中文别名键 ===\n");
    int fail = 0;
    std::wstring root = std::wstring(SCRATCH) + L"\\p15_plugins";
    std::wstring tblDir = root + L"\\NpcModZh\\tbl";
    std::wstring cacheRoot = std::wstring(SCRATCH) + L"\\p15_cache";
    { std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::remove_all(cacheRoot, ec); std::filesystem::create_directories(tblDir, ec); }
    { std::ofstream f(tblDir + L"\\t_npc_mp4000.json", std::ios::binary);
      f << "{ \"table\":\"NPCParam\", \"add_npc\":[ {\"behavior_from\":0, \"模型\":12300, \"位置X\":17.0, \"位置Y\":0.0, \"位置Z\":-144.21, \"朝向\":0, \"对话\":\"NpcTalk.TK_LEVI\", \"动作\":\"npc_setting.AniEvKaiwa\"} ] }\n"; }
    ed9loader::modkit::orchestrator::Paths paths;
    paths.modsDir = root; paths.pacSteamDir = std::wstring(GAME) + L"\\pac\\steam";
    paths.cacheDir = cacheRoot + L"\\merged"; paths.schemasDir = SCHEMAS;
    auto r = ed9loader::modkit::orchestrator::Run(paths, /*force*/true);
    printf("  add_npc(zh): mods=%d tbls=%d failed=%d\n", r.mods, r.tbls, r.failed);
    if (r.failed > 0 || r.tbls < 1) { printf("  [FAIL]\n  log:\n%s\n", r.log.c_str()); return 1; }
    std::vector<uint8_t> out;
    if (!read_file(paths.cacheDir + L"\\table_sc\\t_npc_mp4000.tbl", out)) { printf("  [FAIL] no cache\n"); return 1; }
    uint32_t cnt; memcpy(&cnt, &out[84], 4);
    size_t crow = 88 + (size_t)(cnt - 1) * 160;
    auto poolstr = [&](size_t off) { uint64_t o; memcpy(&o, &out[crow + off], 8); return (o < out.size()) ? std::string((const char*)&out[o]) : std::string("<bad>"); };
    uint32_t pid; memcpy(&pid, &out[crow + 0x00], 4);
    float px; memcpy(&px, &out[crow + 0x2C], 4);
    std::string talk = poolstr(0x78), anim = poolstr(0x68);
    bool ok = (cnt == 1202) && pid == 12300 && px > 16.9f && px < 17.1f
              && talk == "NpcTalk.TK_LEVI" && anim == "npc_setting.AniEvKaiwa";
    printf("  [%s] count=%u 模型=%u 位置X=%.2f 对话=\"%s\" 动作=\"%s\"\n", ok ? "OK" : "FAIL", cnt, pid, px, talk.c_str(), anim.c_str());
    if (!ok) ++fail;
    printf("  %s\n", fail == 0 ? "[OK] add_npc 中文别名" : "[FAIL]");
    return fail;
}

// P16: 室内/无怪图从普通 scene_patch 注入 MonsterArea(无现成模板,需扩名表)。
// golden = python inject_monsterarea_v2.py 产出(已实机验证战斗区注册成功)。
static int test_p16_scene_inject() {
    printf("=== P16 scene_inject (室内图无模板 MonsterArea) ===\n");
    std::vector<uint8_t> orig, gold;
    if (!read_file(L"D:\\destop\\ce修改器\\Sora1\\scene\\scene\\mp4010_02_sys.json", orig)) { printf("  [FAIL] no orig scene\n"); return 1; }
    if (!read_file(L"D:\\destop\\ce修改器\\Sora1\\scene\\scene\\mp4010_02_sys.v2full.golden", gold)) { printf("  [WARN] no golden, skip\n"); return 0; }
    ed9loader::modkit::PatchConfig cfg; cfg.target = "mp4010_02_sys";
    ed9loader::modkit::AddActor a;
    a.type = "MonsterArea"; a.name = "BT_AREA_EV_TEST_00"; a.group = "Battle_Section";
    a.hasTranslation = true; a.tx = 5.88; a.ty = 0.20; a.tz = 2.75;
    a.fields = nlohmann::json::object();
    a.fields["scale"] = nlohmann::json{ {"x", 15.0}, {"y", 15.0}, {"z", 15.0} };
    a.fields["rotation"] = nlohmann::json{ {"x", 0.0}, {"y", 0.0}, {"z", 0.0} };
    a.fields["btl_height"] = "L"; a.fields["btl_width"] = "L"; a.fields["btl_radius"] = "L";
    a.fields["stroll_margin"] = "EVENT"; a.fields["navi_name"] = "navi0";
    cfg.addActors.push_back(a);
    ed9loader::modkit::SceneMergeResult r = ed9loader::modkit::MergeScene(orig, cfg);
    if (!r.ok) { printf("  [FAIL] merge: %s\n", r.err.c_str()); return 1; }
    const int d = first_diff(r.sceneBytes, gold);
    const bool eq = (d < 0);
    printf("  [%s] got=%zu golden=%zu%s\n", eq ? "OK" : "FAIL", r.sceneBytes.size(), gold.size(),
           eq ? "" : ("  first_diff=" + std::to_string(d)).c_str());
    return eq ? 0 : 1;
}

// P17:DecodeTblG 池表 ref 可读化(NPCParam 的 ref 字段解析成 <name>__s 可读串);
//      非池表(t_lookpoint,有 toffset)不应产生 __s 字段(零回归)。
static int test_p17_pool_ref_readable() {
    printf("\n=== P17 DecodeTblG 池表 ref 可读化 ===\n");
    int fail = 0;
    FpacReader r;
    if (!r.Open(std::wstring(GAME) + L"\\pac\\steam\\table_sc.pac")) { printf("  [FAIL] open table_sc.pac\n"); return 1; }
    std::vector<uint8_t> npc;
    if (!r.ReadEntry("table_sc/t_npc_mp4000.tbl", npc)) { printf("  [FAIL] read t_npc_mp4000\n"); return 1; }
    TblFileG g; std::string e;
    if (!DecodeTblG(npc, SCHEMAS, "Sora1", g, e)) { printf("  [FAIL] decode: %s\n", e.c_str()); return 1; }
    if (g.tables.empty() || g.tables[0].rows.empty()) { printf("  [FAIL] no rows\n"); return 1; }
    const TblRowG& row0 = g.tables[0].rows[0];
    struct Exp { const char* field; const char* val; };
    Exp exp[] = {
        { "resource_ref_78__s", "mp4000.TK_NONNA" },          // 对话
        { "resource_ref_68__s", "npc_setting.AniEvKaiwa" },   // 动作
        { "resource_ref_60__s", "npc_setting.LookDistance_7" },// 注视距离
        { "resource_ref_80__s", "map.YobikomiV_NONNA" },      // 语音
        { "tagged_ref_58__s",   "DGPTSQ" },                   // 行为字母码(干净 tag)
    };
    for (const Exp& x : exp) {
        const TblValue* v = row0.find(x.field);
        bool ok = v && v->kind == TblValue::K::Str && v->s == x.val;
        printf("  [%s] %s = \"%s\"(expect \"%s\")\n", ok ? "OK" : "FAIL", x.field,
               v ? v->s.c_str() : "<none>", x.val);
        if (!ok) ++fail;
    }
    // tagged_ref_08 指向 "\0\0"(空)/二进制 → 不应有 __s companion(只留裸偏移)
    bool no08 = (row0.find("tagged_ref_08__s") == nullptr);
    printf("  [%s] tagged_ref_08__s 不存在(空/二进制前缀,只留裸偏移)\n", no08 ? "OK" : "FAIL");
    if (!no08) ++fail;
    // 原始 ulong 字段仍在(裸偏移保留,EncodeTblG/写路径不受影响)
    const TblValue* raw78 = row0.find("resource_ref_78");
    bool rawok = raw78 && raw78->kind == TblValue::K::Int && raw78->i > 0;
    printf("  [%s] resource_ref_78 裸偏移仍保留 = %lld\n", rawok ? "OK" : "FAIL", rawok ? (long long)raw78->i : -1);
    if (!rawok) ++fail;

    // 零回归:非池表 t_lookpoint(有 toffset)不产生任何 __s 字段
    std::vector<uint8_t> lp; TblFileG gl; std::string el;
    if (r.ReadEntry("table_sc/t_lookpoint.tbl", lp) && DecodeTblG(lp, SCHEMAS, "Sora1", gl, el)) {
        int sCount = 0;
        for (const auto& t : gl.tables) for (const auto& rw : t.rows) for (const auto& kv : rw.fields)
            if (kv.first.size() > 3 && kv.first.compare(kv.first.size() - 3, 3, "__s") == 0) ++sCount;
        printf("  [%s] t_lookpoint(非池表)__s 字段数 = %d(expect 0)\n", sCount == 0 ? "OK" : "FAIL", sCount);
        if (sCount != 0) ++fail;
    } else {
        printf("  [WARN] 跳过 t_lookpoint 零回归检查\n");
    }
    printf("  %s\n", fail == 0 ? "[OK] 池表 ref 可读化" : "[FAIL]");
    return fail;
}

int main() {
    int fail = 0;
    fail += test_p0_fpac();
    fail += test_p1_decode();
    fail += test_p2_patch();
    fail += test_p3_tbl();
    fail += test_p4_merge();
    fail += test_p5_orchestrator();
    fail += test_p7_tblpatch();
    fail += test_p8_scriptinject();
    fail += test_tblg_lookpoint();
    fail += test_tblg_roundtrip();
    fail += test_tblg_json_roundtrip();
    fail += test_p9_tbl_generic();
    fail += test_p10_conflict();
    fail += test_p11_dat_replace();
    fail += test_p12_npc_clone();
    fail += test_p13_name_add();
    fail += test_p14_add_npc();
    fail += test_p15_add_npc_zh();
    fail += test_p16_scene_inject();
    fail += test_p17_pool_ref_readable();
    printf("\n%s  total_fail=%d\n", fail == 0 ? "ALL PASS" : "SOME FAIL", fail);
    return fail;
}
