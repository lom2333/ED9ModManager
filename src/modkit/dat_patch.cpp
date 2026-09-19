#include "modkit/dat_patch.h"

#include "ed9_dat.hpp"

#include <fstream>

namespace ed9loader {
namespace modkit {
uint32_t FalcomFuncCrc(const std::string& name) {
    static uint32_t lut[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            lut[i] = c;
        }
        init = true;
    }
    if (name.empty()) return 0;
    uint32_t crc = lut[(~(uint8_t)name[0]) & 0xFF] ^ 0x00FFFFFFu;
    for (size_t i = 1; i < name.size(); ++i)
        crc = (crc >> 8) ^ lut[(crc & 0xFF) ^ (uint8_t)name[i]];
    return crc;
}

namespace {

ed9::Slot RawSlot(uint32_t v) { ed9::Slot s; s.isStr = false; s.raw = v; return s; }
ed9::Slot StrSlot(const std::string& v) { ed9::Slot s; s.isStr = true; s.str = v; return s; }

ed9::Slot ArgSlot(const nlohmann::json& a, uint32_t& typeCode) {
    if (a.is_string()) { typeCode = 2; return StrSlot(a.get<std::string>()); }
    typeCode = 1;
    return RawSlot(0x40000000u | (uint32_t)a.get<int64_t>());
}

ed9::Func MakeCmdWrapper(const std::string& name, uint8_t cmdStruct, uint8_t cmdOp,
                         const std::vector<uint32_t>& argTypes, uint32_t& synth) {
    const uint8_t n = (uint8_t)argTypes.size();
    const int32_t frame = -(int32_t)(4 * n);
    ed9::Func f;
    f.name = name;
    f.crc = FalcomFuncCrc(name);
    f.nin = n; f.b0 = 1; f.b1 = 0; f.nout = 0;
    for (uint32_t t : argTypes) f.varin.push_back(RawSlot(t));
    f.start = 0xF0000000u;

    auto add = [&](ed9::Instr in) { in.codeOff = synth++; f.code.push_back(in); };
    for (uint8_t i = 0; i < n; ++i) { ed9::Instr in; in.op = 0x02; in.i32 = frame; add(in); }
    { ed9::Instr in; in.op = 0x24; in.cmdStruct = cmdStruct; in.cmdOp = cmdOp; in.cmdNArgs = n; add(in); }
    { ed9::Instr in; in.op = 0x01; in.u8 = (uint8_t)(4 * n); add(in); }
    { ed9::Instr in; in.op = 0x00; in.pushSize = 4; in.push = RawSlot(0); add(in); }
    { ed9::Instr in; in.op = 0x0A; add(in); }
    if (n) { ed9::Instr in; in.op = 0x01; in.u8 = (uint8_t)(4 * n); add(in); }
    { ed9::Instr in; in.op = 0x0D; add(in); }
    return f;
}

}

bool LoadDatPatchConfig(const std::wstring& path, DatPatchConfig& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "打不开 ListExtraLoad json"; return false; }
    nlohmann::json j;
    try { f >> j; } catch (const std::exception& e) { err = std::string("json 解析失败: ") + e.what(); return false; }

    out = DatPatchConfig{};
    out.target = j.value("target", std::string());
    if (!j.contains("insert_calls") || !j["insert_calls"].is_array()) { err = "缺少 insert_calls 数组"; return false; }
    for (const auto& e : j["insert_calls"]) {
        DatInsertCall c;
        c.func      = e.value("func", std::string());
        c.callee    = e.value("callee", std::string());
        c.cmdStruct = e.value("cmd_struct", -1);
        c.cmdOp     = e.value("cmd_op", -1);
        if (e.contains("args") && e["args"].is_array())
            for (const auto& a : e["args"]) c.args.push_back(a);
        if (c.func.empty() || c.callee.empty()) { err = "insert_calls 条目缺 func / callee"; return false; }
        out.inserts.push_back(std::move(c));
    }
    return true;
}

bool ApplyDatPatch(const std::vector<uint8_t>& orig,
                   const std::vector<DatInsertCall>& inserts,
                   std::vector<uint8_t>& out, std::string& err) {
    ed9::Script s;
    try { s = ed9::parse(orig); }
    catch (const std::exception& e) { err = std::string("dat 解析失败: ") + e.what(); return false; }

    uint32_t synth = 0;
    for (const auto& f : s.funcs) for (const auto& in : f.code) if (in.codeOff >= synth) synth = in.codeOff + 1;
    if (synth < 0x01000000u) synth = 0x01000000u;

    for (const auto& ins : inserts) {
        int target = -1;
        for (size_t i = 0; i < s.funcs.size(); ++i) if (s.funcs[i].name == ins.func) { target = (int)i; break; }
        if (target < 0) { err = "原版 dat 里没有函数 " + ins.func; return false; }

        std::vector<ed9::Slot> argSlots;
        std::vector<uint32_t> argTypes;
        for (const auto& a : ins.args) {
            uint32_t t = 2;
            argSlots.push_back(ArgSlot(a, t));
            argTypes.push_back(t);
        }

        int callee = -1;
        for (size_t i = 0; i < s.funcs.size(); ++i) if (s.funcs[i].name == ins.callee) { callee = (int)i; break; }
        if (callee < 0) {
            if (ins.cmdStruct < 0 || ins.cmdOp < 0) {
                err = "dat 里没有 " + ins.callee + ",且未给 cmd_struct/cmd_op 以便合成";
                return false;
            }
            s.funcs.push_back(MakeCmdWrapper(ins.callee, (uint8_t)ins.cmdStruct, (uint8_t)ins.cmdOp, argTypes, synth));
            callee = (int)s.funcs.size() - 1;
        }

        std::vector<ed9::Instr> blk;
        auto add = [&](ed9::Instr in) { in.codeOff = synth++; blk.push_back(in); };
        { ed9::Instr in; in.op = 0x26; in.u16 = 1; add(in); }
        { ed9::Instr in; in.op = 0x00; in.pushSize = 4; in.push = RawSlot((uint32_t)target); add(in); }
        { ed9::Instr in; in.op = 0x00; in.pushSize = 4; in.push = RawSlot(0); in.isRetAddr = true; add(in); }
        for (const auto& a : argSlots) { ed9::Instr in; in.op = 0x00; in.pushSize = 4; in.push = a; add(in); }
        { ed9::Instr in; in.op = 0x0C; in.u16 = (uint16_t)callee; add(in); }
        blk[2].retTarget = blk.back().codeOff;

        auto& tf = s.funcs[(size_t)target];
        tf.code.insert(tf.code.begin(), blk.begin(), blk.end());

        ed9::StructDef sd;
        sd.id = callee; sd.nb_sth1 = 0;
        for (const auto& a : argSlots) { sd.array2.push_back(a); sd.array2.push_back(RawSlot(0)); }
        tf.structs.insert(tf.structs.begin(), std::move(sd));
    }

    try { out = ed9::assemble(s); }
    catch (const std::exception& e) { err = std::string("dat 组装失败: ") + e.what(); return false; }
    return true;
}

}
}
