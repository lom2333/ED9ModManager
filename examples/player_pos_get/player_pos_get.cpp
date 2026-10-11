#include "ed9loader_api.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const Ed9Api* g_api = nullptr;
static volatile bool g_running = false;
static unsigned g_poll_ms = 1000;
static unsigned g_mgr_to_player = 0;
static unsigned g_pos_off = 0;
static bool     g_pos_verified = false;
static bool     g_pos_from_code = false;
static unsigned g_hint_mgr = 0;
static bool     g_persisted = false;
static void* g_player_vt = nullptr;

static const unsigned kMgrScanMax   = 0x4000;
static const unsigned kPosScanMin   = 0x20;
static const unsigned kPosScanMax   = 0x800;
static const unsigned kAnchorPollMs = 200;

static const int   kMinRounds   = 15;
static const float kMinMoved    = 3.0f;
static const float kMinSpread   = 2.0f;
static const float kMaxStepCap  = 100.0f;
static const float kChangeEps   = 0.002f;
static const float kMaxSmooth   = 0.60f;
static const int   kMinCandN    = 24;
static const int   kWindowRounds = 30;
static const float kDeadRatio   = 0.5f;

struct Cand {
    uint16_t off;
    float    last[3];
    float    mn[3], mx[3];
    float    moved;
    float    maxStep;
    float    prevStep;
    float    sumAcc;
    uint16_t changed;
};
static const int kMaxCand = 384;
static Cand g_cand[kMaxCand];
static int  g_candN = 0;
static int  g_rounds = 0;
static int  g_buildTries = 0;
static int  g_badReads = 0;
static void* g_track_player = nullptr;
static float g_curLast[3] = {};
static float g_curMoved = 0.0f;
static bool  g_curHasLast = false;

static bool ReadU64(const void* a, uintptr_t* out) {
    return g_api->safe_read(a, out, sizeof(*out)) != 0;
}

static void* Manager() {
    if (g_api == nullptr || g_api->find_vtable == nullptr || g_api->find_instance == nullptr) return nullptr;
    void* vt = g_api->find_vtable("Manager@fieldmap@sora");
    return (vt != nullptr) ? g_api->find_instance(vt) : nullptr;
}

static bool IsPlayerSlot(void* manager, unsigned off) {
    uintptr_t p = 0;
    if (!ReadU64(reinterpret_cast<char*>(manager) + off, &p) || p < 0x10000) return false;
    uintptr_t vt = 0;
    if (!ReadU64(reinterpret_cast<void*>(p), &vt)) return false;
    return g_player_vt != nullptr && vt == reinterpret_cast<uintptr_t>(g_player_vt);
}

static unsigned FindPlayerOffset(void* manager) {
    if (g_mgr_to_player != 0 && IsPlayerSlot(manager, g_mgr_to_player)) return g_mgr_to_player;
    if (g_hint_mgr != 0 && IsPlayerSlot(manager, g_hint_mgr)) return g_hint_mgr;
    for (unsigned o = 8; o <= kMgrScanMax; o += 8)
        if (IsPlayerSlot(manager, o)) return o;
    return 0;
}

static void* PlayerPtr() {
    void* mgr = Manager();
    if (mgr == nullptr) return nullptr;
    const unsigned off = FindPlayerOffset(mgr);
    if (off == 0) return nullptr;
    if (g_mgr_to_player != off) {
        g_mgr_to_player = off;
        g_persisted = false;
        if (g_api->log != nullptr) {
            char b[96];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "[PlayerPosGet] 自锚定 manager_to_player_offset = 0x%X", off);
            g_api->log(b);
        }
    }
    uintptr_t p = 0;
    ReadU64(reinterpret_cast<char*>(mgr) + off, &p);
    return reinterpret_cast<void*>(p);
}

static unsigned ReadPlayerBlock(void* player, unsigned char* buf, unsigned cap) {
    for (unsigned n = cap; n >= 0x100; n >>= 1) {
        memset(buf, 0, cap);
        if (g_api->safe_read(player, buf, n) != 0) return n;
    }
    return 0;
}

static bool PlausibleTriple(const float* v) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i])) return false;
        if (std::fabs(v[i]) > 1.0e5f) return false;
    }
    return !(std::fabs(v[0]) < 1e-4f && std::fabs(v[1]) < 1e-4f && std::fabs(v[2]) < 1e-4f);
}

static float SpreadOf(const Cand& c) {
    const float dx = c.mx[0] - c.mn[0], dy = c.mx[1] - c.mn[1], dz = c.mx[2] - c.mn[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static void ResetWindow() {
    g_candN = 0; g_rounds = 0;
    g_curMoved = 0.0f; g_curHasLast = false;
}

static void ResetTracking() {
    ResetWindow();
    g_buildTries = 0; g_badReads = 0;
    g_pos_off = 0; g_pos_verified = false; g_pos_from_code = false; g_persisted = false;
}

static void TrackCandidates(void* player) {
    if (player != g_track_player) { g_track_player = player; g_buildTries = 0; ResetWindow(); }
    else if (g_rounds >= kWindowRounds) ResetWindow();
    unsigned char buf[kPosScanMax + 16] = {};
    const unsigned n = ReadPlayerBlock(player, buf, sizeof(buf));
    if (n == 0) return;

    if (g_pos_off != 0 && g_pos_off + 12 <= n) {
        const float* v = reinterpret_cast<const float*>(buf + g_pos_off);
        if (g_curHasLast) {
            const float dx = v[0] - g_curLast[0], dy = v[1] - g_curLast[1], dz = v[2] - g_curLast[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(d)) g_curMoved += d;
        }
        memcpy(g_curLast, v, sizeof(g_curLast));
        g_curHasLast = true;
    }

    if (g_candN == 0) {
        for (unsigned o = kPosScanMin; o + 16 <= n && g_candN < kMaxCand; o += 4) {
            const float* v = reinterpret_cast<const float*>(buf + o);
            if (!PlausibleTriple(v)) continue;
            Cand& c = g_cand[g_candN++];
            c.off = static_cast<uint16_t>(o);
            memcpy(c.last, v, sizeof(c.last));
            memcpy(c.mn, v, sizeof(c.mn));
            memcpy(c.mx, v, sizeof(c.mx));
            c.moved = 0.0f; c.maxStep = 0.0f; c.changed = 0;
            c.prevStep = -1.0f; c.sumAcc = 0.0f;
        }
        if (g_candN < kMinCandN && g_buildTries < 60) { ++g_buildTries; g_candN = 0; g_rounds = 0; }
        return;
    }
    for (int i = 0; i < g_candN; ++i) {
        Cand& c = g_cand[i];
        const float* v = reinterpret_cast<const float*>(buf + c.off);
        if (!PlausibleTriple(v)) { c.maxStep = 1.0e9f; continue; }
        const float dx = v[0] - c.last[0], dy = v[1] - c.last[1], dz = v[2] - c.last[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > kChangeEps) ++c.changed;
        if (c.prevStep >= 0.0f) c.sumAcc += std::fabs(d - c.prevStep);
        c.prevStep = d;
        c.moved += d;
        if (d > c.maxStep) c.maxStep = d;
        for (int k = 0; k < 3; ++k) {
            if (v[k] < c.mn[k]) c.mn[k] = v[k];
            if (v[k] > c.mx[k]) c.mx[k] = v[k];
        }
        memcpy(c.last, v, sizeof(c.last));
    }
    ++g_rounds;
}

static float SmoothOf(const Cand& c) {
    if (g_rounds < 4 || c.moved <= 1.0e-6f) return 1.0e9f;
    const float meanStep = c.moved  / static_cast<float>(g_rounds);
    const float meanAcc  = c.sumAcc / static_cast<float>(g_rounds - 1);
    return meanAcc / (meanStep + 1.0e-9f);
}

static bool Qualifies(const Cand& c) {
    if (g_rounds < kMinRounds) return false;
    if (c.maxStep > kMaxStepCap) return false;
    if (c.moved < kMinMoved) return false;
    if (SpreadOf(c) < kMinSpread) return false;
    if (SmoothOf(c) > kMaxSmooth) return false;
    return c.changed * 2 >= g_rounds;
}

static float RankOf(const Cand& c) {
    return SpreadOf(c) / (0.1f + SmoothOf(c));
}

static int BestCandidate() {
    int best = -1; float bestR = 0.0f;
    for (int i = 0; i < g_candN; ++i) {
        if (!Qualifies(g_cand[i])) continue;
        const float r = RankOf(g_cand[i]);
        if (r > bestR) { bestR = r; best = i; }
    }
    return best;
}

static void DecidePosOffset() {
    if (g_pos_verified || g_rounds < kMinRounds) return;
    if (g_pos_off != 0)
        for (int i = 0; i < g_candN; ++i)
            if (g_cand[i].off == g_pos_off && Qualifies(g_cand[i])) { g_pos_verified = true; return; }
    const int best = BestCandidate();
    if (best < 0) return;
    if (g_pos_off != 0 && g_curMoved >= kDeadRatio * g_cand[best].moved) return;
    g_pos_off = g_cand[best].off;
    g_pos_verified = true;
    g_pos_from_code = false;
    g_persisted = false;
    if (g_api->log != nullptr) {
        char b[160];
        _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "[PlayerPosGet] 自锚定 pos_offset = 0x%X (跨度%.1f 平滑度%.2f 位移%.1f 变化%d/%d,候选%d个)",
                    g_pos_off, SpreadOf(g_cand[best]), SmoothOf(g_cand[best]), g_cand[best].moved,
                    g_cand[best].changed, g_rounds, g_candN);
        g_api->log(b);
    }
}

static void PersistOnce() {
    if (g_persisted || !g_pos_verified || g_api->cfg_set_int == nullptr) return;
    if (g_mgr_to_player == 0 || g_pos_off == 0) return;
    g_api->cfg_set_int("PlayerPosGet", "manager_to_player_offset", static_cast<int>(g_mgr_to_player));
    g_api->cfg_set_int("PlayerPosGet", "pos_offset", static_cast<int>(g_pos_off));
    g_api->cfg_set_int("PlayerPosGet", "pos_verified", 1);
    g_persisted = true;
}

static bool ReadPlayerPos(float* out, bool track) {
    if (g_api == nullptr || g_api->safe_read == nullptr) return false;
    void* player = PlayerPtr();
    if (player == nullptr) return false;
    if (track && !g_pos_verified) { TrackCandidates(player); DecidePosOffset(); }
    if (g_pos_off == 0) return false;
    if (g_api->safe_read(reinterpret_cast<char*>(player) + g_pos_off, out, sizeof(float) * 3) == 0) return false;
    bool bad = false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(out[i]) || std::fabs(out[i]) > 1.0e5f) bad = true;
    if (bad) {
        if (++g_badReads >= 30) {
            g_api->log("[PlayerPosGet] 存档的 pos_offset 读出无效值,重新自锚定");
            ResetTracking();
        }
        return false;
    }
    g_badReads = 0;
    PersistOnce();
    return true;
}

static void GetOutPath(char* out, size_t n) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash != nullptr) *slash = '\0';
    _snprintf_s(out, n, _TRUNCATE, "%s\\ED9Loader\\PlayerPosGet.pos.txt", exe);
}

static DWORD WINAPI ThreadMain(LPVOID) {
    char outpath[MAX_PATH] = {};
    GetOutPath(outpath, sizeof(outpath));
    while (g_running) {
        Sleep(g_pos_verified ? g_poll_ms : kAnchorPollMs);
        float pos[3] = {0.0f, 0.0f, 0.0f};
        const bool ok = ReadPlayerPos(pos, true);
        char line[176] = {};
        if (ok) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "X=%.2f  Y=%.2f  Z=%.2f   [mgr+0x%X, pos+0x%X]",
                        pos[0], pos[1], pos[2], g_mgr_to_player, g_pos_off);
        } else if (g_mgr_to_player != 0) {
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "(anchoring: walk around; %d candidates, %d rounds)", g_candN, g_rounds);
        } else {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "(waiting for field scene)");
        }
        FILE* file = nullptr;
        if (fopen_s(&file, outpath, "w") == 0 && file != nullptr) {
            fputs(line, file);
            fputc('\n', file);
            fclose(file);
        }
    }
    return 0;
}

static void Cmd_Scan() {
    char b[224];
    void* mgr = Manager();
    _snprintf_s(b, sizeof(b), _TRUNCATE, "Manager=%p  Player vtable=%p\n", mgr, g_player_vt);
    g_api->console_print(b);
    if (mgr == nullptr) { g_api->console_print("[ERR] Manager 单例没找到(先进场景)\n"); return; }
    void* player = PlayerPtr();
    _snprintf_s(b, sizeof(b), _TRUNCATE, "manager_to_player_offset = 0x%X  ->  Player=%p\n", g_mgr_to_player, player);
    g_api->console_print(b);
    if (player == nullptr) { g_api->console_print("[ERR] Player 槽位没找到(先进场景)\n"); return; }
    unsigned char buf[kPosScanMax + 16] = {};
    const unsigned n = ReadPlayerBlock(player, buf, sizeof(buf));
    if (n == 0) { g_api->console_print("[ERR] Player 读不动\n"); return; }
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                "可读 0x%X 字节,候选 %d 个,采样 %d 轮(合格线: 轮数>=%d 位移>=%.0f 跨度>=%.0f 变化帧>=半数)\n",
                n, g_candN, g_rounds, kMinRounds, kMinMoved, kMinSpread);
    g_api->console_print(b);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "pos_offset = 0x%X  %s%s(本窗口这格挪动 %.1f)\n",
                g_pos_off, g_pos_verified ? "已核实" : "未核实", g_pos_from_code ? "、游戏代码里读的" : "", g_curMoved);
    g_api->console_print(b);

    int idx[14]; int m = 0;
    for (int i = 0; i < g_candN; ++i) {
        const float s = SpreadOf(g_cand[i]);
        if (s <= 0.01f) continue;
        int j = m;
        while (j > 0 && SpreadOf(g_cand[idx[j - 1]]) < s) { if (j < 14) idx[j] = idx[j - 1]; --j; }
        if (j < 14) { idx[j] = i; if (m < 14) ++m; }
    }
    if (m == 0) { g_api->console_print("  (还没有候选变化过 —— 边走边再敲一次 -get scan)\n"); return; }
    for (int k = 0; k < m; ++k) {
        const Cand& c = g_cand[idx[k]];
        const bool ok = Qualifies(c);
        _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "  %s +0x%03X  X=%.2f Y=%.2f Z=%.2f  跨度=%.1f 平滑=%.2f 位移=%.1f 变化=%d/%d 最大步=%.1f%s\n",
                    ok ? "√" : "×", c.off, c.last[0], c.last[1], c.last[2],
                    SpreadOf(c), SmoothOf(c), c.moved, c.changed, g_rounds, c.maxStep,
                    (c.off == g_pos_off) ? "  <== 采用" : "");
        g_api->console_print(b);
    }
    g_api->console_print("  挑错了就用 -get use <十六进制偏移> 手动指定,-get reset 重新锚定\n");
}

static const unsigned kDumpCap    = 0x1000;
static const unsigned kDumpEveryMs = 100;
static volatile bool g_dumping = false;
static unsigned g_dumpSec = 20;

static DWORD WINAPI DumpThread(LPVOID) {
    char path[MAX_PATH] = {}, exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash != nullptr) *slash = '\0';
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\ED9Loader\\console_logs", exe);
    CreateDirectoryA(path, nullptr);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\ED9Loader\\console_logs\\pos_trace.bin", exe);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) { g_dumping = false; return 0; }
    const unsigned hdr[6] = { 0x54475050u, 1u, kDumpCap, kDumpEveryMs, g_mgr_to_player, 0u };
    fwrite(hdr, sizeof(hdr), 1, f);

    static unsigned char blk[kDumpCap];
    const DWORD t0 = GetTickCount();
    unsigned n = 0;
    while (g_dumping && (GetTickCount() - t0) < g_dumpSec * 1000u) {
        void* player = PlayerPtr();
        if (player != nullptr) {
            memset(blk, 0, sizeof(blk));
            if (g_api->safe_read(player, blk, kDumpCap) != 0) {
                const unsigned tick = static_cast<unsigned>(GetTickCount() - t0);
                const uint64_t va = reinterpret_cast<uint64_t>(player);
                fwrite(&tick, 4, 1, f);
                fwrite(&va, 8, 1, f);
                fwrite(blk, kDumpCap, 1, f);
                ++n;
            }
        }
        Sleep(kDumpEveryMs);
    }
    fclose(f);
    g_dumping = false;
    if (g_api->console_print != nullptr) {
        char b[192];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "[dump] 完成:%u 帧 -> ED9Loader\\console_logs\\pos_trace.bin\n", n);
        g_api->console_print(b);
    }
    return 0;
}

static void Cmd_Near(float tx, float tz, float tol) {
    char b[224];
    void* player = PlayerPtr();
    if (player == nullptr) { g_api->console_print("[ERR] Player 没找到(先进场景)\n"); return; }
    static unsigned char buf[kDumpCap];
    const unsigned n = ReadPlayerBlock(player, buf, kDumpCap);
    if (n == 0) { g_api->console_print("[ERR] Player 读不动\n"); return; }
    _snprintf_s(b, sizeof(b), _TRUNCATE, "找 x≈%.2f z≈%.2f (容差 %.2f) 的槽位:\n", tx, tz, tol);
    g_api->console_print(b);
    int hit = 0;
    for (unsigned o = 0; o + 12 <= n; o += 4) {
        const float* v = reinterpret_cast<const float*>(buf + o);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
        if (std::fabs(v[0] - tx) > tol || std::fabs(v[2] - tz) > tol) continue;
        float spread = -1.0f; int changed = -1;
        for (int i = 0; i < g_candN; ++i)
            if (g_cand[i].off == o) { spread = SpreadOf(g_cand[i]); changed = g_cand[i].changed; break; }
        _snprintf_s(b, sizeof(b), _TRUNCATE, "  +0x%03X  X=%.3f Y=%.3f Z=%.3f   spread=%.1f changed=%d/%d%s\n",
                    o, v[0], v[1], v[2], spread, changed, g_rounds, (o == g_pos_off) ? "  <== 当前采用" : "");
        g_api->console_print(b);
        ++hit;
    }
    if (hit == 0) g_api->console_print("  (没有槽位匹配 —— 确认站对地方了?或把容差放大: -get near x z 10)\n");
    else g_api->console_print("  选定后:-get use <偏移>\n");
}

static void Cmd_Get(int argc, const char** argv) {
    if (g_api == nullptr || g_api->console_print == nullptr) return;
    char buf[192] = {};
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) { Cmd_Scan(); return; }
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        if (g_dumping) { g_api->console_print("[dump] 已经在录了\n"); return; }
        if (PlayerPtr() == nullptr) { g_api->console_print("[ERR] Player 没找到(先进场景)\n"); return; }
        g_dumpSec = (argc >= 3) ? static_cast<unsigned>(strtoul(argv[2], nullptr, 10)) : 20u;
        if (g_dumpSec < 3 || g_dumpSec > 300) g_dumpSec = 20;
        g_dumping = true;
        const HANDLE h = CreateThread(nullptr, 0, DumpThread, nullptr, 0, nullptr);
        if (h != nullptr) CloseHandle(h);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[dump] 开始录 %u 秒(10Hz,每帧 0x%X 字节)—— 现在开始走动\n",
                    g_dumpSec, kDumpCap);
        g_api->console_print(buf);
        return;
    }
    if (argc >= 4 && strcmp(argv[1], "near") == 0) {
        Cmd_Near(static_cast<float>(atof(argv[2])), static_cast<float>(atof(argv[3])),
                 (argc >= 5) ? static_cast<float>(atof(argv[4])) : 4.0f);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        ResetTracking();
        g_api->console_print("已清空,重新锚定中 —— 走动几秒后 -get pos\n");
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "use") == 0) {
        const unsigned o = static_cast<unsigned>(strtoul(argv[2], nullptr, 16));
        if (o < kPosScanMin || o + 12 > kPosScanMax) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "偏移要在 0x%X..0x%X 之间\n", kPosScanMin, kPosScanMax - 12);
            g_api->console_print(buf);
            return;
        }
        g_pos_off = o; g_pos_verified = true; g_pos_from_code = false; g_persisted = false;
        PersistOnce();
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "已手动指定 pos_offset = 0x%X 并写入 ini\n", o);
        g_api->console_print(buf);
        return;
    }
    if (argc < 2 || strcmp(argv[1], "pos") != 0) {
        g_api->console_print("usage: -get pos | scan | near <x> <z> [tol] | dump [sec] | use <hex> | reset\n");
        return;
    }
    float pos[3] = {0.0f, 0.0f, 0.0f};
    if (ReadPlayerPos(pos, false)) {
        const bool zero = pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f;
        const bool unsure = !g_pos_verified && !g_pos_from_code;
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "X=%.2f  Y=%.2f  Z=%.2f   [mgr+0x%X, pos+0x%X%s]%s\n",
                    pos[0], pos[1], pos[2], g_mgr_to_player, g_pos_off, unsure ? " 未核实" : "",
                    (zero && unsure) ? "  走几步再敲一次(边走边核实偏移)" : "");
    } else if (g_mgr_to_player != 0) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[..] 正在自锚定:边走边等(候选%d个,已采样%d轮/需%d轮),细节看 -get scan\n",
                    g_candN, g_rounds, kMinRounds);
    } else {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[ERR] position unavailable (enter a field scene first)\n");
    }
    g_api->console_print(buf);
}

static const unsigned kCodeChunk = 0x100000;
static const unsigned kCodeOverlap = 64;

static bool MatchSync(const unsigned char* p, size_t avail, unsigned* d_out) {
    if (avail < 3 * 5 + 3 * 8 + 4) return false;
    int sb = -1, a = 0;
    for (int k = 0; k < 3; ++k, p += 5) {
        if (p[0] != 0xF3 || p[1] != 0x0F || p[2] != 0x10) return false;
        const int m = p[3];
        if ((m >> 6) != 1 || (m & 7) == 4 || ((m >> 3) & 7) != k) return false;
        const int disp = static_cast<signed char>(p[4]);
        if (k == 0) { sb = m & 7; a = disp; }
        else if ((m & 7) != sb || disp != a + 4 * k) return false;
    }
    int db = -1;
    int32_t d = 0;
    for (int k = 0; k < 3; ++k, p += 8) {
        if (p[0] != 0xF3 || p[1] != 0x0F || p[2] != 0x11) return false;
        const int m = p[3];
        if ((m >> 6) != 2 || (m & 7) == 4 || ((m >> 3) & 7) != k) return false;
        int32_t disp = 0;
        memcpy(&disp, p + 4, 4);
        if (k == 0) { db = m & 7; d = disp; }
        else if ((m & 7) != db || disp != d + 4 * k) return false;
    }
    if (p[0] != 0x48 || p[1] != 0x8B || (p[2] >> 6) != 1 || (p[2] & 7) != db) return false;
    *d_out = static_cast<unsigned>(d);
    return true;
}

static unsigned PosOffsetFromCode() {
    if (g_api->get_module_base == nullptr || g_api->safe_read == nullptr) return 0;
    const unsigned char* mod = static_cast<const unsigned char*>(g_api->get_module_base());
    if (mod == nullptr) return 0;
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS64 nt = {};
    if (g_api->safe_read(mod, &dos, sizeof(dos)) == 0 || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (g_api->safe_read(mod + dos.e_lfanew, &nt, sizeof(nt)) == 0 || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    const unsigned char* sec = mod + dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    static unsigned char buf[kCodeChunk + kCodeOverlap];
    unsigned found = 0;
    int hits = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(nt.FileHeader.NumberOfSections); ++i) {
        IMAGE_SECTION_HEADER sh = {};
        if (g_api->safe_read(sec + i * sizeof(sh), &sh, sizeof(sh)) == 0) return 0;
        if ((sh.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        const unsigned size = sh.Misc.VirtualSize;
        for (unsigned off = 0; off < size; off += kCodeChunk) {
            const unsigned n = (size - off < kCodeChunk + kCodeOverlap) ? size - off : kCodeChunk + kCodeOverlap;
            if (g_api->safe_read(mod + sh.VirtualAddress + off, buf, n) == 0) return 0;
            const unsigned lim = (n < kCodeChunk) ? n : kCodeChunk;
            for (unsigned j = 0; j < lim; ++j) {
                const void* q = memchr(buf + j, 0xF3, lim - j);
                if (q == nullptr) break;
                j = static_cast<unsigned>(static_cast<const unsigned char*>(q) - buf);
                unsigned d = 0;
                if (MatchSync(buf + j, n - j, &d)) { found = d; ++hits; }
            }
        }
    }
    if (hits != 1 || found < kPosScanMin || found + 12 > kPosScanMax || (found & 3) != 0) return 0;
    return found;
}

static int GameDefaults(unsigned* mgr, unsigned* pos) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* name = strrchr(exe, '\\');
    name = (name != nullptr) ? name + 1 : exe;
    *mgr = 0; *pos = 0;
    if (_stricmp(name, "sora_1st.exe") == 0) { *mgr = 0x640; *pos = 0x290; return 1; }
    if (_stricmp(name, "sora_2nd.exe") == 0) { *mgr = 0x660; *pos = 0x2C0; return 2; }
    return 0;
}

extern "C" __declspec(dllexport) void Plugin_Load(const Ed9Api* api) {
    if (api == nullptr || api->log == nullptr) return;
    if (api->abi_version < 5) return;
    g_api = api;

    unsigned def_mgr = 0, def_pos = 0;
    const int game = GameDefaults(&def_mgr, &def_pos);
    g_hint_mgr = def_mgr;
    g_pos_off  = def_pos;
    bool from_ini = false;
    if (api->cfg_get_int != nullptr) {
        g_poll_ms = static_cast<unsigned>(api->cfg_get_int("PlayerPosGet", "poll_ms", 1000));
        const unsigned ini_mgr = static_cast<unsigned>(api->cfg_get_int("PlayerPosGet", "manager_to_player_offset", 0));
        const unsigned ini_pos = static_cast<unsigned>(api->cfg_get_int("PlayerPosGet", "pos_offset", 0));
        if (ini_mgr != 0) g_hint_mgr = ini_mgr;
        if (ini_pos != 0 && (api->cfg_get_int("PlayerPosGet", "pos_verified", 0) == 1 || def_pos == 0)) {
            g_pos_off = ini_pos;
            from_ini = true;
        }
        if (api->cfg_set_int != nullptr) api->cfg_set_int("PlayerPosGet", "poll_ms", static_cast<int>(g_poll_ms));
    }
    const char* from = from_ini ? " ini " : "默认";
    const unsigned code_pos = PosOffsetFromCode();
    if (code_pos != 0) {
        g_pos_off = code_pos;
        g_pos_from_code = true;
        g_pos_verified = true;
        from = "游戏代码里";
    } else if (game == 2) {
        api->log("[PlayerPosGet] 这个游戏版本里没认出坐标位置,先按老位置读,走几步会自己找回");
    }
    if (g_pos_off != 0) {
        char b[96];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "[PlayerPosGet] 采用%s的 pos_offset = 0x%X", from, g_pos_off);
        api->log(b);
    }
    g_player_vt = (api->find_vtable != nullptr) ? api->find_vtable("Player@fieldmap@sora") : nullptr;
    if (g_player_vt == nullptr) api->log("[PlayerPosGet] 警告:没拿到 Player vtable,无法自锚定");

    if (api->abi_version >= 6 && api->register_command != nullptr)
        api->register_command("-get", "player coords (pos | scan | near <x> <z> | dump [sec] | use <hex> | reset)", Cmd_Get);

    g_running = true;
    CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
