#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>

namespace ed9 {
struct Slot {
    bool isStr = false;
    uint32_t raw = 0;
    std::string str;
};

struct Instr {
    uint8_t  op = 0;
    uint32_t codeOff = 0;

    uint8_t  pushSize = 4;
    Slot     push;

    int32_t  i32 = 0;

    uint8_t  u8 = 0;

    uint32_t jumpTargetOff = 0;

    bool     isRetAddr = false;
    uint32_t retTarget = 0;

    uint16_t u16 = 0;

    std::string sA, sB;
    uint8_t  cfsVar = 0;

    uint8_t  cmdStruct = 0, cmdOp = 0, cmdNArgs = 0;
};

struct StructDef {
    int32_t  id = 0;
    uint16_t nb_sth1 = 0;
    std::vector<Slot> array2;
};

struct Func {
    std::string name;
    uint32_t crc = 0;
    uint8_t  nin = 0, b0 = 0, b1 = 0, nout = 0;
    std::vector<Slot> varin, varout;
    std::vector<StructDef> structs;
    uint32_t start = 0;
    std::vector<Instr> code;
};

struct Script {
    std::string name;
    uint32_t nScriptVarIn = 0, nScriptVarOut = 0;
    std::vector<Slot> scriptVars;
    std::vector<Func> funcs;
};

Script parse(const std::vector<uint8_t>& buf);
std::vector<uint8_t> assemble(const Script& s);

uint32_t instrLen(const Instr& in);
struct Layout { uint32_t startCode=0, codeLen=0, startStrings=0; };
Layout computeLayout(const Script& s);

}
