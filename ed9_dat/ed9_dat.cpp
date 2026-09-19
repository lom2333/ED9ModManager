#include "ed9_dat.hpp"
#include <cstring>
#include <algorithm>

namespace ed9 {
static uint8_t  rdU8 (const std::vector<uint8_t>& b, size_t o){ return b.at(o); }
static uint16_t rdU16(const std::vector<uint8_t>& b, size_t o){ return (uint16_t)(b.at(o) | (b.at(o+1)<<8)); }
static uint32_t rdU32(const std::vector<uint8_t>& b, size_t o){
    return (uint32_t)b.at(o) | ((uint32_t)b.at(o+1)<<8) | ((uint32_t)b.at(o+2)<<16) | ((uint32_t)b.at(o+3)<<24);
}
static int32_t  rdI32(const std::vector<uint8_t>& b, size_t o){ return (int32_t)rdU32(b,o); }

static std::string rdCStr(const std::vector<uint8_t>& b, size_t o){
    std::string s;
    while (o < b.size() && b[o] != 0) { s.push_back((char)b[o]); ++o; }
    return s;
}

static const uint32_t TAG_MASK = 0xC0000000u;
static const uint32_t TAG_STR  = 0xC0000000u;
static const uint32_t ADDR_MASK= 0x3FFFFFFFu;

static Slot slotFromU32(const std::vector<uint8_t>& b, uint32_t v){
    Slot s;
    if ((v & TAG_MASK) == TAG_STR) { s.isStr = true; s.str = rdCStr(b, v & ADDR_MASK); }
    else { s.raw = v; }
    return s;
}

static void wrU8 (std::vector<uint8_t>& b, uint8_t v){ b.push_back(v); }
static void wrU16(std::vector<uint8_t>& b, uint16_t v){ b.push_back((uint8_t)(v&0xFF)); b.push_back((uint8_t)((v>>8)&0xFF)); }
static void wrU32(std::vector<uint8_t>& b, uint32_t v){ for(int i=0;i<4;i++) b.push_back((uint8_t)((v>>(8*i))&0xFF)); }
static void setU32(std::vector<uint8_t>& b, size_t o, uint32_t v){ for(int i=0;i<4;i++) b[o+i]=(uint8_t)((v>>(8*i))&0xFF); }

uint32_t instrLen(const Instr& in){
    switch(in.op){
        case 0x00: return 2 + in.pushSize;
        case 0x01: return 2;
        case 0x02: case 0x03: case 0x04: case 0x05:
        case 0x06: case 0x07: case 0x08: return 5;
        case 0x09: case 0x0A: return 2;
        case 0x0B: return 5;
        case 0x0C: return 3;
        case 0x0D: return 1;
        case 0x0E: case 0x0F: return 5;
        case 0x22: case 0x23: return 10;
        case 0x24: return 4;
        case 0x25: return 5;
        case 0x26: return 3;
        case 0x27: return 2;
        case 0x28: return 5;
        default:
            if (in.op >= 0x10 && in.op <= 0x21) return 1;
            throw std::runtime_error("instrLen: 未知 opcode " + std::to_string(in.op));
    }
}

Script parse(const std::vector<uint8_t>& buf){
    if (buf.size() < 0x18 || std::memcmp(buf.data(), "#scp", 4) != 0)
        throw std::runtime_error("不是 #scp 文件");

    Script sc;
    uint32_t startFuncHdr   = rdU32(buf, 0x04);
    uint32_t nfunc          = rdU32(buf, 0x08);
    uint32_t startScriptVars= rdU32(buf, 0x0C);
    sc.nScriptVarIn  = rdU32(buf, 0x10);
    sc.nScriptVarOut = rdU32(buf, 0x14);

    sc.funcs.resize(nfunc);
    std::vector<uint32_t> codeAddr(nfunc);

    for (uint32_t i=0;i<nfunc;i++){
        size_t base = startFuncHdr + 0x20*i;
        Func& f = sc.funcs[i];
        codeAddr[i]      = rdU32(buf, base+0x00);
        uint32_t vars    = rdU32(buf, base+0x04);
        f.nin  = (uint8_t)( vars        & 0xFF);
        f.b0   = (uint8_t)((vars >> 8)  & 0xFF);
        f.b1   = (uint8_t)((vars >> 16) & 0xFF);
        f.nout = (uint8_t)((vars >> 24) & 0xFF);
        uint32_t addrVarOut = rdU32(buf, base+0x08);
        uint32_t addrVarIn  = rdU32(buf, base+0x0C);
        uint32_t nstructs   = rdU32(buf, base+0x10);
        uint32_t addrStructs= rdU32(buf, base+0x14);
        f.crc               = rdU32(buf, base+0x18);
        uint32_t namePtr    = rdU32(buf, base+0x1C);
        f.name = rdCStr(buf, namePtr & ADDR_MASK);

        for (uint32_t k=0;k<f.nout;k++) f.varout.push_back(slotFromU32(buf, rdU32(buf, addrVarOut+4*k)));
        for (uint32_t k=0;k<f.nin ;k++) f.varin .push_back(slotFromU32(buf, rdU32(buf, addrVarIn +4*k)));
        for (uint32_t k=0;k<nstructs;k++){
            size_t sb = addrStructs + 0xC*k;
            StructDef sd;
            sd.id      = rdI32(buf, sb+0x00);
            sd.nb_sth1 = rdU16(buf, sb+0x04);
            uint16_t cnt = rdU16(buf, sb+0x06);
            uint32_t ap  = rdU32(buf, sb+0x08);
            for (uint32_t j=0;j<(uint32_t)cnt*2;j++)
                sd.array2.push_back(slotFromU32(buf, rdU32(buf, ap+4*j)));
            f.structs.push_back(std::move(sd));
        }
    }

    uint32_t nSV = (sc.nScriptVarIn + sc.nScriptVarOut) * 2;
    for (uint32_t k=0;k<nSV;k++)
        sc.scriptVars.push_back(slotFromU32(buf, rdU32(buf, startScriptVars + 4*k)));

    uint32_t startCode = startScriptVars + nSV*4;
    for (uint32_t i=0;i<nfunc;i++) sc.funcs[i].start = codeAddr[i] - startCode;

    uint32_t curMin = (uint32_t)buf.size();
    auto consider = [&](uint32_t addr){ if (addr < curMin) curMin = addr; };
    for (uint32_t i=0;i<nfunc;i++){
        size_t base = startFuncHdr + 0x20*i;
        uint32_t namePtr = rdU32(buf, base+0x1C);
        if ((namePtr & TAG_MASK)==TAG_STR) consider(namePtr & ADDR_MASK);
        Func& f = sc.funcs[i];
        for (auto& s: f.varout) if (s.isStr) {}
    }
    auto scanSlotAddr = [&](uint32_t rawAddrField){
        uint32_t v = rawAddrField;
        if ((v & TAG_MASK)==TAG_STR) consider(v & ADDR_MASK);
    };
    for (uint32_t i=0;i<nfunc;i++){
        size_t base = startFuncHdr + 0x20*i;
        uint32_t addrVarOut = rdU32(buf, base+0x08);
        uint32_t addrVarIn  = rdU32(buf, base+0x0C);
        uint32_t nstructs   = rdU32(buf, base+0x10);
        uint32_t addrStructs= rdU32(buf, base+0x14);
        Func& f = sc.funcs[i];
        for (uint32_t k=0;k<f.nout;k++) scanSlotAddr(rdU32(buf, addrVarOut+4*k));
        for (uint32_t k=0;k<f.nin ;k++) scanSlotAddr(rdU32(buf, addrVarIn +4*k));
        for (uint32_t k=0;k<nstructs;k++){
            size_t sb = addrStructs + 0xC*k;
            uint16_t cnt = rdU16(buf, sb+0x06);
            uint32_t ap  = rdU32(buf, sb+0x08);
            for (uint32_t j=0;j<(uint32_t)cnt*2;j++) scanSlotAddr(rdU32(buf, ap+4*j));
        }
    }
    for (uint32_t k=0;k<nSV;k++) scanSlotAddr(rdU32(buf, startScriptVars + 4*k));

    std::vector<Instr> flat;
    size_t cur = startCode;
    while (cur < curMin){
        Instr in; in.op = rdU8(buf, cur); in.codeOff = (uint32_t)(cur - startCode); cur += 1;
        switch (in.op){
            case 0x00: {
                in.pushSize = rdU8(buf, cur); cur += 1;
                uint32_t v = 0;
                for (uint32_t i=0;i<in.pushSize;i++) v |= (uint32_t)rdU8(buf, cur+i) << (8*i);
                cur += in.pushSize;
                if (in.pushSize==4 && (v & TAG_MASK)==TAG_STR){
                    in.push.isStr = true; in.push.str = rdCStr(buf, v & ADDR_MASK);
                    consider(v & ADDR_MASK); if ((v & ADDR_MASK) < curMin) curMin = v & ADDR_MASK;
                } else in.push.raw = v;
                break;
            }
            case 0x01: in.u8 = rdU8(buf, cur); cur+=1; break;
            case 0x02: case 0x03: case 0x04: case 0x05:
            case 0x06: case 0x07: case 0x08: in.i32 = rdI32(buf, cur); cur+=4; break;
            case 0x09: case 0x0A: in.u8 = rdU8(buf, cur); cur+=1; break;
            case 0x0B: in.jumpTargetOff = rdU32(buf, cur) - startCode; cur+=4; break;
            case 0x0C: in.u16 = rdU16(buf, cur); cur+=2; break;
            case 0x0D: break;
            case 0x0E: case 0x0F: in.jumpTargetOff = rdU32(buf, cur) - startCode; cur+=4; break;
            case 0x22: case 0x23: {
                uint32_t p1 = rdU32(buf, cur); cur+=4;
                uint32_t p2 = rdU32(buf, cur); cur+=4;
                in.cfsVar = rdU8(buf, cur); cur+=1;
                in.sA = rdCStr(buf, p1 & ADDR_MASK);
                in.sB = rdCStr(buf, p2 & ADDR_MASK);
                consider(p1 & ADDR_MASK); if ((p1&ADDR_MASK)<curMin) curMin=p1&ADDR_MASK;
                consider(p2 & ADDR_MASK); if ((p2&ADDR_MASK)<curMin) curMin=p2&ADDR_MASK;
                break;
            }
            case 0x24: in.cmdStruct=rdU8(buf,cur); in.cmdOp=rdU8(buf,cur+1); in.cmdNArgs=rdU8(buf,cur+2); cur+=3; break;
            case 0x25: in.jumpTargetOff = rdU32(buf, cur) - startCode; cur+=4; break;
            case 0x26: in.u16 = rdU16(buf, cur); cur+=2; break;
            case 0x27: in.u8 = rdU8(buf, cur); cur+=1; break;
            case 0x28: in.i32 = rdI32(buf, cur); cur+=4; break;
            default:
                if (in.op >= 0x10 && in.op <= 0x21) break;
                throw std::runtime_error("parse: 未知 opcode 0x" + std::to_string(in.op) +
                                         " @code_off " + std::to_string(in.codeOff));
        }
        flat.push_back(std::move(in));
    }

    std::vector<uint32_t> order;
    for (uint32_t i=0;i<nfunc;i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){ return sc.funcs[a].start < sc.funcs[b].start; });

    for (size_t oi=0; oi<order.size(); ++oi){
        uint32_t fi = order[oi];
        uint32_t lo = sc.funcs[fi].start;
        uint32_t hi = (oi+1<order.size()) ? sc.funcs[order[oi+1]].start : (uint32_t)(cur - startCode);
        for (auto& in : flat) if (in.codeOff >= lo && in.codeOff < hi) sc.funcs[fi].code.push_back(in);
    }

    for (auto& f : sc.funcs){
        for (size_t ci=0; ci<f.code.size(); ci++){
            if (f.code[ci].op != 0x0C) continue;
            uint32_t retVal = (startCode + f.code[ci].codeOff + 3) & ADDR_MASK;
            for (size_t k=ci; k-- > 0; ){
                Instr& p = f.code[k];
                if (p.op==0x00 && !p.push.isStr && !p.isRetAddr && p.push.raw == retVal){
                    p.isRetAddr = true; p.retTarget = f.code[ci].codeOff; break;
                }
            }
        }
    }
    return sc;
}

std::vector<uint8_t> assemble(const Script& sc){
    uint32_t nfunc = (uint32_t)sc.funcs.size();
    uint32_t total_out=0, total_in=0, total_structs=0, size_params=0;
    for (auto& f: sc.funcs){
        total_out += (uint32_t)f.varout.size();
        total_in  += (uint32_t)f.varin.size();
        total_structs += (uint32_t)f.structs.size();
        for (auto& st: f.structs) size_params += (uint32_t)st.array2.size()*4;
    }
    uint32_t startFuncHdr = 0x18;
    uint32_t startVarOut  = startFuncHdr + 0x20*nfunc;
    uint32_t startVarIn   = startVarOut + total_out*4;
    uint32_t startStructs = startVarIn + total_in*4;
    uint32_t startStructParams = startStructs + 0xC*total_structs;
    uint32_t startScriptVars   = startStructParams + size_params;
    uint32_t startCode = startScriptVars + (uint32_t)sc.scriptVars.size()*4;

    std::vector<uint32_t> order;
    for (uint32_t i=0;i<nfunc;i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){ return sc.funcs[a].start < sc.funcs[b].start; });

    std::map<uint32_t,uint32_t> oldToNew;
    std::vector<uint32_t> funcNewStart(nfunc, 0);
    uint32_t codeLen = 0;
    for (uint32_t oi=0; oi<order.size(); ++oi){
        uint32_t fi = order[oi];
        funcNewStart[fi] = codeLen;
        for (auto& in : sc.funcs[fi].code){ oldToNew[in.codeOff] = codeLen; codeLen += instrLen(in); }
    }
    oldToNew[ (uint32_t)0 ] = oldToNew.count(0)? oldToNew[0] : 0;
    uint32_t startStrings = startCode + codeLen;

    std::vector<uint8_t> strSec;
    uint32_t sa = startStrings;
    auto appendStr = [&](const std::string& s)->uint32_t {
        uint32_t off = sa;
        for (char c: s) strSec.push_back((uint8_t)c);
        strSec.push_back(0); sa += (uint32_t)s.size()+1; return off;
    };
    std::map<const Slot*,uint32_t> slotOff;
    std::vector<uint32_t> nameOff(nfunc, 0);

    std::vector<uint8_t> codeSec;
    auto emitJump = [&](uint32_t oldOff){ uint32_t n = oldToNew.count(oldOff)? oldToNew[oldOff] : oldOff; wrU32(codeSec, startCode + n); };
    for (uint32_t oi=0; oi<order.size(); ++oi) for (auto& in : sc.funcs[order[oi]].code){
        wrU8(codeSec, in.op);
        switch (in.op){
            case 0x00: {
                wrU8(codeSec, in.pushSize);
                uint32_t v;
                if (in.isRetAddr){
                    uint32_t n = oldToNew.count(in.retTarget) ? oldToNew[in.retTarget] : in.retTarget;
                    v = (startCode + n + 3) & ADDR_MASK;
                } else {
                    v = in.push.isStr ? ((appendStr(in.push.str) & ADDR_MASK) | TAG_STR) : in.push.raw;
                }
                for (uint32_t i=0;i<in.pushSize;i++) codeSec.push_back((uint8_t)((v>>(8*i))&0xFF));
                break;
            }
            case 0x01: wrU8(codeSec, in.u8); break;
            case 0x02: case 0x03: case 0x04: case 0x05:
            case 0x06: case 0x07: case 0x08: wrU32(codeSec, (uint32_t)in.i32); break;
            case 0x09: case 0x0A: wrU8(codeSec, in.u8); break;
            case 0x0B: emitJump(in.jumpTargetOff); break;
            case 0x0C: wrU16(codeSec, in.u16); break;
            case 0x0D: break;
            case 0x0E: case 0x0F: emitJump(in.jumpTargetOff); break;
            case 0x22: case 0x23: {
                uint32_t p1=(appendStr(in.sA)&ADDR_MASK)|TAG_STR;
                uint32_t p2=(appendStr(in.sB)&ADDR_MASK)|TAG_STR;
                wrU32(codeSec, p1); wrU32(codeSec, p2); wrU8(codeSec, in.cfsVar); break;
            }
            case 0x24: wrU8(codeSec, in.cmdStruct); wrU8(codeSec, in.cmdOp); wrU8(codeSec, in.cmdNArgs); break;
            case 0x25: emitJump(in.jumpTargetOff); break;
            case 0x26: wrU16(codeSec, in.u16); break;
            case 0x27: wrU8(codeSec, in.u8); break;
            case 0x28: wrU32(codeSec, (uint32_t)in.i32); break;
            default:
                if (in.op >= 0x10 && in.op <= 0x21) break;
                throw std::runtime_error("assemble: 未知 opcode");
        }
    }

    for (uint32_t i=0;i<nfunc;i++) nameOff[i] = appendStr(sc.funcs[i].name);
    for (auto& f: sc.funcs) for (auto& s: f.varout) if (s.isStr) slotOff[&s]=appendStr(s.str);
    for (auto& f: sc.funcs) for (auto& s: f.varin)  if (s.isStr) slotOff[&s]=appendStr(s.str);
    for (auto& f: sc.funcs) for (auto& st: f.structs) for (auto& s: st.array2) if (s.isStr) slotOff[&s]=appendStr(s.str);
    for (auto& s: sc.scriptVars) if (s.isStr) slotOff[&s]=appendStr(s.str);

    auto slotU32 = [&](const Slot& sl)->uint32_t { return sl.isStr ? ((slotOff[&sl] & ADDR_MASK) | TAG_STR) : sl.raw; };

    std::vector<uint8_t> funcHdr, varOutSec, varInSec, structsSec, paramsSec, scriptVarSec;
    uint32_t curVarOut=startVarOut, curVarIn=startVarIn, curStructs=startStructs, curParams=startStructParams;
    for (uint32_t i=0;i<nfunc;i++){
        const Func& f = sc.funcs[i];
        wrU32(funcHdr, startCode + funcNewStart[i]);
        uint32_t vars = (uint32_t)f.nin | ((uint32_t)f.b0<<8) | ((uint32_t)f.b1<<16) | ((uint32_t)f.nout<<24);
        wrU32(funcHdr, vars);
        wrU32(funcHdr, curVarOut);
        wrU32(funcHdr, curVarIn);
        wrU32(funcHdr, (uint32_t)f.structs.size());
        wrU32(funcHdr, curStructs);
        wrU32(funcHdr, f.crc);
        wrU32(funcHdr, (nameOff[i] & ADDR_MASK) | TAG_STR);
        for (auto& s: f.varout){ wrU32(varOutSec, slotU32(s)); curVarOut+=4; }
        for (auto& s: f.varin ){ wrU32(varInSec , slotU32(s)); curVarIn +=4; }
        for (auto& st: f.structs){
            wrU32(structsSec, (uint32_t)st.id);
            wrU16(structsSec, st.nb_sth1);
            wrU16(structsSec, (uint16_t)(st.array2.size()/2));
            wrU32(structsSec, curParams);
            curStructs += 0xC;
            for (auto& s: st.array2){ wrU32(paramsSec, slotU32(s)); curParams+=4; }
        }
    }
    for (auto& s: sc.scriptVars) wrU32(scriptVarSec, slotU32(s));

    std::vector<uint8_t> out;
    out.insert(out.end(), {'#','s','c','p'});
    wrU32(out, startFuncHdr);
    wrU32(out, nfunc);
    wrU32(out, startScriptVars);
    wrU32(out, sc.nScriptVarIn);
    wrU32(out, sc.nScriptVarOut);
    auto app = [&](std::vector<uint8_t>& v){ out.insert(out.end(), v.begin(), v.end()); };
    app(funcHdr); app(varOutSec); app(varInSec); app(structsSec); app(paramsSec); app(scriptVarSec); app(codeSec); app(strSec);
    return out;
}

Layout computeLayout(const Script& sc){
    uint32_t nfunc=(uint32_t)sc.funcs.size();
    uint32_t total_out=0,total_in=0,total_structs=0,size_params=0;
    for (auto& f: sc.funcs){ total_out+=(uint32_t)f.varout.size(); total_in+=(uint32_t)f.varin.size();
        total_structs+=(uint32_t)f.structs.size(); for(auto& st:f.structs) size_params+=(uint32_t)st.array2.size()*4; }
    uint32_t startVarOut=0x18+0x20*nfunc, startVarIn=startVarOut+total_out*4, startStructs=startVarIn+total_in*4;
    uint32_t startStructParams=startStructs+0xC*total_structs, startScriptVars=startStructParams+size_params;
    Layout L; L.startCode=startScriptVars+(uint32_t)sc.scriptVars.size()*4;
    uint32_t codeLen=0; for (auto& f: sc.funcs) for (auto& in: f.code) codeLen+=instrLen(in);
    L.codeLen=codeLen; L.startStrings=L.startCode+codeLen; return L;
}

}
