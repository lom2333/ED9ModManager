// ed9_dat.hpp — ED9 scena (#scp) 纯 C++ 解析/组装引擎
// 目标:dat <-> 结构化内存模型 双向无损(字节完美),不依赖 Python。
// 字节码层(低级):每条指令定长编解码,无栈分析 —— 风险最低的地基。
//
// 已逆向的关键事实(对照 kurotools ED9Assembler/ED9InstructionsSet):
//  - #scp header 0x18 字节;函数头每 0x20 字节;
//  - 区顺序: funcHdr -> varOut -> varIn -> structs -> structParams -> scriptVars -> code -> strings
//  - 字符串指针 = addr | 0xC0000000 (2bit 类型标签: 00=UNDEF 01=INT 10=FLOAT 11=STR)
//  - 字符串区收集顺序: code串(code顺序) -> 函数名 -> varOut -> varIn -> structParams -> scriptVar
//    原始编译器对字符串"全局去重(首次出现)";复刻它即可字节完美还原原始 dat。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>

namespace ed9 {

// ---- 一个 u32 槽:要么原始值(带2bit标签),要么字符串引用 ----
struct Slot {
    bool isStr = false;
    uint32_t raw = 0;        // 非字符串时的原始 u32(保留标签位)
    std::string str;         // isStr 时的字符串内容
};

// ---- 字节码指令(低级) ----
struct Instr {
    uint8_t  op = 0;
    uint32_t codeOff = 0;    // 解析时记录:该指令在 code 区的偏移(用于跳转重定位)

    // PUSH(0x00): pushSize + value;value 可能是字符串指针
    uint8_t  pushSize = 4;
    Slot     push;           // op==0: 值或字符串

    // 栈/索引类(0x02-0x08): 单个 i32/u32 原始操作数
    int32_t  i32 = 0;

    // 单字节操作数(0x01 POP / 0x09 LOADRESULT / 0x0A SAVERESULT / 0x27 POP2)
    uint8_t  u8 = 0;

    // 跳转(0x0B JUMP / 0x0E JUMPIFTRUE / 0x0F JUMPIFFALSE / 0x25 / 0x28): 目标 code 偏移
    uint32_t jumpTargetOff = 0;

    // PUSHUNDEFINED 若是 CallFunction 的返回地址(value==CALL之后绝对地址),符号化以便编辑后重定位
    bool     isRetAddr = false;
    uint32_t retTarget = 0;  // 指向的 code 偏移(= CALL.codeOff + 3)

    // CALL(0x0C): 函数索引
    uint16_t u16 = 0;

    // CALLFROMANOTHERSCRIPT(0x22/0x23): str1,str2,var
    std::string sA, sB;
    uint8_t  cfsVar = 0;

    // RUNCMD(0x24): struct,opcode,nargs
    uint8_t  cmdStruct = 0, cmdOp = 0, cmdNArgs = 0;
};

struct StructDef {
    int32_t  id = 0;
    uint16_t nb_sth1 = 0;
    std::vector<Slot> array2;  // count*2 个 u32 槽(字符串或包装值)
};

struct Func {
    std::string name;
    uint32_t crc = 0;
    uint8_t  nin = 0, b0 = 0, b1 = 0, nout = 0;
    std::vector<Slot> varin, varout;
    std::vector<StructDef> structs;
    uint32_t start = 0;        // code 区内偏移(解析自函数头的 code_addr - start_code)
    std::vector<Instr> code;   // 该函数的指令(按 code 顺序)
};

struct Script {
    std::string name;          // 取自首个被引用?实际 dat 无脚本名字段;用文件名
    uint32_t nScriptVarIn = 0, nScriptVarOut = 0;
    std::vector<Slot> scriptVars;  // (nScriptVarIn + nScriptVarOut) * 2 个 u32 槽
    std::vector<Func> funcs;       // 按函数 id 顺序(= dat 函数头顺序)
};

// 解析 dat 字节 -> Script
Script parse(const std::vector<uint8_t>& buf);
// 组装 Script -> dat 字节(字符串去重,字节完美还原)
std::vector<uint8_t> assemble(const Script& s);

// 诊断:布局计算
uint32_t instrLen(const Instr& in);
struct Layout { uint32_t startCode=0, codeLen=0, startStrings=0; };
Layout computeLayout(const Script& s);

} // namespace ed9
