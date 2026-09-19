#include "ed9loader_api.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static const Ed9Api* g_api = nullptr;

static unsigned g_rva_global   = 0xadd798;
static unsigned g_off_list     = 0x1488;
static unsigned g_off_count    = 0x1490;
static unsigned g_entry_stride = 0x20;
static unsigned g_off_name     = 0x10;
static unsigned g_off_pos      = 0xc8;
static unsigned g_off_mcount   = 0x218;

template <typename T>
static bool RD(uintptr_t addr, T* out) {
    if (g_api == nullptr || g_api->safe_read == nullptr) return false;
    return g_api->safe_read(reinterpret_cast<const void*>(addr), out, sizeof(T)) != 0;
}

template <typename Sink>
static long long BuildAreaReport(Sink sink) {
    char buf[256] = {};
    uintptr_t base = reinterpret_cast<uintptr_t>(g_api->get_module_base());
    uintptr_t manager = 0;
    if (!RD<uintptr_t>(base + g_rva_global, &manager) || manager == 0) {
        sink("[btl] battle manager global is null (not in a scene yet?)\n");
        return -1;
    }
    uintptr_t list = 0;
    uint64_t count = 0;
    RD<uintptr_t>(manager + g_off_list, &list);
    RD<uint64_t>(manager + g_off_count, &count);

    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[btl] manager=0x%llx  list=0x%llx  count=%llu\n",
                (unsigned long long)manager, (unsigned long long)list, (unsigned long long)count);
    sink(buf);

    {
        unsigned char gate = 0xEE;
        RD<unsigned char>(manager + 0x2d24, &gate);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[btl] gate +0x2d24 = %u  (FUN_14024f530门控: 0=区子系统活跃, !=0=禁用)\n", (unsigned)gate);
        sink(buf);
        auto hexdump = [&](uintptr_t off, int n, const char* tag) {
            char line[300]; int p = 0;
            p += _snprintf_s(line + p, sizeof(line) - p, _TRUNCATE, "[btl] %s +0x%llx: ", tag, (unsigned long long)off);
            for (int k = 0; k < n && p < 270; ++k) {
                unsigned char b = 0; RD<unsigned char>(manager + off + k, &b);
                p += _snprintf_s(line + p, sizeof(line) - p, _TRUNCATE, "%02x ", b);
            }
            _snprintf_s(line + p, sizeof(line) - p, _TRUNCATE, "\n");
            sink(line);
        };
        hexdump(0x1480, 32, "list区");
        hexdump(0x2d10, 32, "gate区");
    }

    if (list == 0 || count == 0) {
        sink("[btl] area list is EMPTY in this context.\n");
        return 0;
    }
    if (count > 512) count = 512;

    for (uint64_t i = 0; i < count; ++i) {
        uintptr_t entry = list + i * g_entry_stride;
        uintptr_t obj = 0;
        if (!RD<uintptr_t>(entry, &obj) || obj == 0) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  [%llu] <null obj>\n", (unsigned long long)i);
            sink(buf);
            continue;
        }
        char name[64] = {};
        g_api->safe_read(reinterpret_cast<const void*>(obj + g_off_name), name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        for (int k = 0; k < (int)sizeof(name); ++k) {
            unsigned char c = (unsigned char)name[k];
            if (c == 0) break;
            if (c < 0x20 || c > 0x7e) { name[k] = '\0'; break; }
        }
        float pos[3] = {0, 0, 0};
        RD<float[3]>(obj + g_off_pos, &pos);
        uint32_t mcount = 0;
        RD<uint32_t>(obj + g_off_mcount, &mcount);

        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "  [%llu] obj=0x%llx  name=\"%s\"  pos=(%.2f, %.2f, %.2f)  monsters=%u\n",
                    (unsigned long long)i, (unsigned long long)obj, name,
                    pos[0], pos[1], pos[2], mcount);
        sink(buf);

        if (mcount > 0 && mcount <= 64) {
            uintptr_t mlist = 0;
            RD<uintptr_t>(obj + 0x210, &mlist);
            for (uint32_t m = 0; m < mcount && mlist != 0; ++m) {
                uintptr_t ent = 0;
                if (!RD<uintptr_t>(mlist + (uintptr_t)m * 0x18, &ent) || ent == 0) continue;
                uint16_t mid = 0; int16_t cid = 0; uintptr_t mname = 0;
                RD<uint16_t>(ent + 0x28, &mid);
                RD<int16_t>(ent + 0x8, &cid);
                RD<uintptr_t>(ent + 0x0, &mname);
                char mn[40] = {};
                if (mname != 0) {
                    g_api->safe_read(reinterpret_cast<const void*>(mname), mn, sizeof(mn) - 1);
                    for (int k = 0; k < (int)sizeof(mn); ++k) {
                        unsigned char c = (unsigned char)mn[k];
                        if (c == 0) break;
                        if (c < 0x20 || c > 0x7e) { mn[k] = '\0'; break; }
                    }
                }
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "        mon[%u] id(+0x28)=%u (0x%x)  charid(+0x8)=%d  model=\"%s\"\n",
                            m, (unsigned)mid, (unsigned)mid, (int)cid, mn);
                sink(buf);
            }
        }
    }
    sink("[btl] --- end ---\n");
    return (long long)count;
}

static unsigned g_rva_encmgr = 0xae0d38;
static unsigned g_off_encsub = 0x738;
static unsigned g_off_encmode = 0x1f4540;
static unsigned g_off_encmon  = 0x1f4544;
static unsigned g_off_encflag = 0x1f454c;

template <typename Sink>
static void BuildEncounterReport(Sink sink) {
    char buf[256] = {};
    uintptr_t base = reinterpret_cast<uintptr_t>(g_api->get_module_base());
    uintptr_t mgr = 0;
    if (!RD<uintptr_t>(base + g_rva_encmgr, &mgr) || mgr == 0) {
        sink("[enc] encounter manager null\n");
        return;
    }
    uintptr_t sub = 0;
    RD<uintptr_t>(mgr + g_off_encsub, &sub);
    if (sub == 0) { sink("[enc] sub(+0x738) null\n"); return; }
    uint32_t mode = 0xDEAD; uint16_t mon = 0xDEAD; uint8_t flag = 0xFF;
    RD<uint32_t>(sub + g_off_encmode, &mode);
    RD<uint16_t>(sub + g_off_encmon, &mon);
    RD<uint8_t>(sub + g_off_encflag, &flag);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[enc] mode(+0x1f4540)=%u  current_monster(+0x1f4544)=%u (0x%x)  flag(+0x1f454c)=%u\n",
                mode, (unsigned)mon, (unsigned)mon, (unsigned)flag);
    sink(buf);

    int32_t f1ba0 = 0; uint8_t f1ba4 = 0;
    RD<int32_t>(mgr + 0x1ba0, &f1ba0);
    RD<uint8_t>(mgr + 0x1ba4, &f1ba4);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[fld] mgr+0x1ba0(int)=%d  mgr+0x1ba4(byte,field-pause)=%u\n", f1ba0, (unsigned)f1ba4);
    sink(buf);
    unsigned char hx[40] = {};
    g_api->safe_read(reinterpret_cast<const void*>(mgr + 0x1b98), hx, sizeof(hx));
    char hexline[200] = "[fld] mgr+0x1b98: ";
    for (int k = 0; k < 32; ++k) {
        char t[6]; _snprintf_s(t, sizeof(t), _TRUNCATE, "%02x ", hx[k]);
        strcat_s(hexline, sizeof(hexline), t);
    }
    strcat_s(hexline, sizeof(hexline), "\n");
    sink(hexline);
    uint8_t sub2[24] = {};
    g_api->safe_read(reinterpret_cast<const void*>(sub + 0x1f4540), sub2, sizeof(sub2));
    char subline[200] = "[fld] sub+0x1f4540: ";
    for (int k = 0; k < 24; ++k) {
        char t[6]; _snprintf_s(t, sizeof(t), _TRUNCATE, "%02x ", sub2[k]);
        strcat_s(subline, sizeof(subline), t);
    }
    strcat_s(subline, sizeof(subline), "\n");
    sink(subline);
}

static unsigned g_rva_chara_root = 0xadd7a8;
template <typename Sink>
static void BuildCharaReport(Sink sink) {
    char buf[512] = {};
    uintptr_t base = reinterpret_cast<uintptr_t>(g_api->get_module_base());
    uintptr_t root = 0, obj = 0, cm = 0, table_base = 0, data_base = 0;
    if (!RD<uintptr_t>(base + g_rva_chara_root, &root) || root == 0) { sink("[chara] root null\n"); return; }
    if (!RD<uintptr_t>(root + 0xa0, &obj) || obj == 0) { sink("[chara] obj null\n"); return; }
    if (!RD<uintptr_t>(obj + 8, &cm) || cm == 0) { sink("[chara] chara_mgr null\n"); return; }
    RD<uintptr_t>(cm + 0x20, &table_base);
    uint32_t page = 0; RD<uint32_t>(cm + 0x2c, &page);
    RD<uintptr_t>(cm + 0x10, &data_base);
    if (table_base == 0 || data_base == 0) { sink("[chara] table/data null\n"); return; }
    uintptr_t pe = table_base + 0x44 + (uintptr_t)page * 0x50;
    uint32_t offset = 0, count = 0; int32_t stride = 0;
    RD<uint32_t>(pe, &offset);
    RD<int32_t>(pe + 4, &stride);
    RD<uint32_t>(pe + 8, &count);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[chara] page=%u count=%u stride=%d\n", page, count, stride);
    sink(buf);
    if (count > 4000) count = 4000;
    int found550 = 0, found999 = 0;
    char ids[400] = ""; size_t idlen = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t ca = data_base + offset + (uintptr_t)stride * i;
        uint16_t id = 0;
        if (!RD<uint16_t>(ca, &id)) continue;
        if (id == 61550) found550 = 1;
        if (id == 61999) found999 = 1;
        if (id >= 61000 && id < 63000) {
            char t[12]; int tn = _snprintf_s(t, sizeof(t), _TRUNCATE, "%u ", id);
            if (tn > 0 && idlen + (size_t)tn < sizeof(ids) - 1) { strcat_s(ids, sizeof(ids), t); idlen += tn; }
        }
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[chara] 61550(orig-event)_present=%d  61999(ours)_present=%d\n[chara] event-range ids: %s\n",
                found550, found999, ids);
    sink(buf);

    uintptr_t emgr = 0;
    if (!RD<uintptr_t>(base + g_rva_encmgr, &emgr) || emgr == 0) { sink("[chara2] enc-mgr null\n"); return; }
    uintptr_t elist = 0; uint64_t ecount = 0;
    RD<uintptr_t>(emgr + 0x200, &elist);
    RD<uint64_t>(emgr + 0x208, &ecount);
    if (elist == 0) { sink("[chara2] list null\n"); return; }
    if (ecount > 4000) ecount = 4000;
    int e550 = 0, e999 = 0; char eids[400] = ""; size_t elen = 0;
    for (uint64_t i = 0; i < ecount; ++i) {
        uintptr_t cp = 0;
        if (!RD<uintptr_t>(elist + i * 8, &cp) || cp == 0) continue;
        uint32_t id = 0;
        if (!RD<uint32_t>(cp + 0x190, &id)) continue;
        if (id == 61550) e550 = 1;
        if (id == 61999) e999 = 1;
        if (id >= 61000 && id < 63000) {
            char t[12]; int tn = _snprintf_s(t, sizeof(t), _TRUNCATE, "%u ", id);
            if (tn > 0 && elen + (size_t)tn < sizeof(eids) - 1) { strcat_s(eids, sizeof(eids), t); elen += tn; }
        }
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[chara2] DAT_140ad4e98+0x200 active-list count=%llu  61550=%d  61999=%d  event-ids: %s\n",
                (unsigned long long)ecount, e550, e999, eids);
    sink(buf);
}

static void ReadCStr(uintptr_t p, char* out, size_t n) {
    out[0] = '\0';
    if (p < 0x10000) return;
    g_api->safe_read(reinterpret_cast<const void*>(p), out, n - 1);
    out[n - 1] = '\0';
    for (size_t k = 0; k < n; ++k) {
        unsigned char c = (unsigned char)out[k];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) { out[k] = '\0'; break; }
    }
}
template <typename Sink>
static void BuildFieldCharFullDump(Sink sink) {
    char buf[700] = {};
    uintptr_t base = reinterpret_cast<uintptr_t>(g_api->get_module_base());
    uintptr_t root = 0, obj = 0, cm = 0, table_base = 0, data_base = 0;
    if (!RD<uintptr_t>(base + g_rva_chara_root, &root) || root == 0) { sink("[fc] root null\n"); return; }
    if (!RD<uintptr_t>(root + 0xa0, &obj) || obj == 0) { sink("[fc] obj null\n"); return; }
    if (!RD<uintptr_t>(obj + 8, &cm) || cm == 0) { sink("[fc] cm null\n"); return; }
    RD<uintptr_t>(cm + 0x20, &table_base);
    uint32_t page = 0; RD<uint32_t>(cm + 0x2c, &page);
    RD<uintptr_t>(cm + 0x10, &data_base);
    if (table_base == 0 || data_base == 0) { sink("[fc] table/data null\n"); return; }
    uintptr_t pe = table_base + 0x44 + (uintptr_t)page * 0x50;
    uint32_t offset = 0, count = 0; int32_t stride = 0;
    RD<uint32_t>(pe, &offset);
    RD<int32_t>(pe + 4, &stride);
    RD<uint32_t>(pe + 8, &count);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[fc] full field-char table: page=%u count=%u stride=%d (id / evt@+8 / scr@+38)\n", page, count, stride);
    sink(buf);
    if (count > 2000) count = 2000;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t ca = data_base + offset + (uintptr_t)stride * i;
        uint16_t id = 0; if (!RD<uint16_t>(ca, &id)) continue;
        uintptr_t pevt = 0, pscr = 0; uint16_t f30 = 0, f42 = 0;
        RD<uintptr_t>(ca + 0x8, &pevt);
        RD<uintptr_t>(ca + 0x38, &pscr);
        RD<uint16_t>(ca + 0x30, &f30);
        RD<uint16_t>(ca + 0x42, &f42);
        char evt[48] = {}, scr[48] = {};
        ReadCStr(pevt, evt, sizeof(evt));
        ReadCStr(pscr, scr, sizeof(scr));
        if (evt[0] == '\0' && scr[0] == '\0') continue;
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "  [%u] id=%u(0x%x) f30=%u f42=%u evt=\"%s\" scr=\"%s\"\n",
                    i, (unsigned)id, (unsigned)id, (unsigned)f30, (unsigned)f42, evt, scr);
        sink(buf);
    }
    sink("[fc] --- end ---\n");
}

static void DumpAreas() {
    if (g_api == nullptr || g_api->console_print == nullptr) return;
    BuildAreaReport([](const char* s) { g_api->console_print(s); });
    BuildEncounterReport([](const char* s) { g_api->console_print(s); });
    BuildCharaReport([](const char* s) { g_api->console_print(s); });
}

static volatile bool g_running = false;
static unsigned g_poll_ms = 1500;

static void GetOutPath(char* out, size_t n) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash != nullptr) *slash = '\0';
    _snprintf_s(out, n, _TRUNCATE, "%s\\ED9Loader\\BattleProbe.areas.txt", exe);
}

static DWORD WINAPI ThreadMain(LPVOID) {
    char outpath[MAX_PATH] = {};
    GetOutPath(outpath, sizeof(outpath));
    while (g_running) {
        Sleep(g_poll_ms);
        FILE* file = nullptr;
        if (fopen_s(&file, outpath, "w") != 0 || file == nullptr) continue;
        BuildAreaReport([file](const char* s) { fputs(s, file); });
        BuildEncounterReport([file](const char* s) { fputs(s, file); });
        BuildCharaReport([file](const char* s) { fputs(s, file); });
        BuildFieldCharFullDump([file](const char* s) { fputs(s, file); });
        fclose(file);
    }
    return 0;
}

static void Cmd_Btl(int argc, const char** argv) {
    if (g_api == nullptr || g_api->console_print == nullptr) return;
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        DumpAreas();
        return;
    }
    g_api->console_print("usage: -btl list   (dump active battle-area list)\n");
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) return;
    if (api->abi_version < 6) {
        return;
    }
    g_api = api;

    if (api->cfg_get_int != nullptr) {
        g_rva_global   = (unsigned)api->cfg_get_int("BattleProbe", "rva_global",   (int)g_rva_global);
        g_off_list     = (unsigned)api->cfg_get_int("BattleProbe", "off_list",     (int)g_off_list);
        g_off_count    = (unsigned)api->cfg_get_int("BattleProbe", "off_count",    (int)g_off_count);
        g_entry_stride = (unsigned)api->cfg_get_int("BattleProbe", "entry_stride", (int)g_entry_stride);
        g_off_name     = (unsigned)api->cfg_get_int("BattleProbe", "off_name",     (int)g_off_name);
        g_off_pos      = (unsigned)api->cfg_get_int("BattleProbe", "off_pos",      (int)g_off_pos);
        g_off_mcount   = (unsigned)api->cfg_get_int("BattleProbe", "off_mcount",   (int)g_off_mcount);
    }

    if (api->cfg_get_int != nullptr) {
        g_poll_ms = (unsigned)api->cfg_get_int("BattleProbe", "poll_ms", (int)g_poll_ms);
    }

    if (api->register_command != nullptr) {
        api->register_command("-btl", "dump active battle-area list (usage: -btl list)", Cmd_Btl);
    }

    g_running = true;
    CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
