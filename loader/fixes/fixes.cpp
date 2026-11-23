// Built-in game fixes.
//
//  * FastPhysics: the exact constraint solver of the game's physics
//    (Bullet's btDantzigSolver::solveMLCP) is replaced by a faster build of the
//    same algorithm. Based on FastPhysics 1.1.0 (zlib licence,
//    https://www.nexusmods.com/scrapmechanic/mods/63). This is an ALTERED
//    version: the hook below was rewritten for SMLoader. See
//    THIRD_PARTY_NOTICES.md.
//    Its two other patches (reflection probe sweep, navmesh duplicate search)
//    are not included: the game fixed both itself in 1.0.7.
//
//  * NetworkFix: a client no longer postpones packets. Written for SMLoader
//    from the game's own code; the idea comes from Scrap-Mods' networking-fix
//    (https://github.com/Scrap-Mods/networking-fix). No code is shared with it.
//
// Nothing here touches files, saves or the Lua side.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "fastlcp.h"
#include "fixes.h"

namespace {

void (*Log)(const char* fmt, ...) = nullptr;

struct Range {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
    bool Contains(const void* p) const { return p >= begin && p < end; }
};
uint8_t* g_base = nullptr;
Range g_text, g_rdata, g_data;

bool InitSections() {
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        memcpy(name, sec->Name, IMAGE_SIZEOF_SHORT_NAME);
        Range r{g_base + sec->VirtualAddress, g_base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (!strcmp(name, ".text")) g_text = r;
        else if (!strcmp(name, ".rdata")) g_rdata = r;
        else if (!strcmp(name, ".data")) g_data = r;
    }
    return g_text.begin && g_rdata.begin && g_data.begin;
}

template <typename T>
T Read(const void* p) {
    T v;
    memcpy(&v, p, sizeof v);
    return v;
}

// ---------------------------------------------------------------------------
// NetworkFix
//
// NetworkClient remembers the last server tick it processed and what kind of
// packet that was. When a packet of a newer tick arrives and its kind differs
// (or is script data), the game does not handle it: it copies it into a queue
// and takes it out again on a later client tick. On a busy server the queue
// grows faster than it drains, and the client falls further and further behind.
//
//     mov  eax, [r14+120h]      ; last processed server tick
//     mov  edx, 402800h         ; packet kinds
//     mov  r15d, 1
//     test eax, eax
//     je   process              ; first packet               <- made unconditional
//     cmp  esi, eax
//     jbe  process              ; not newer
//     ...                       ; decide whether to postpone
//
// With the first branch always taken, every packet is processed on arrival, in
// the order it came in. The queue stays empty.
// ---------------------------------------------------------------------------
void InstallNetworkFix() {
    static const int sig[] = {
        0x41, 0x8B, 0x86, 0x20, 0x01, 0x00, 0x00, 0xBA, 0x00, 0x28, 0x40, 0x00, 0x41, 0xBF, 0x01, 0x00, 0x00, 0x00,
        0x85, 0xC0, 0x0F, 0x84, -1, -1, -1, -1, 0x3B, 0xF0, 0x0F, 0x86, -1, -1, -1, -1, 0x80, 0xFB, 0x16,
    };
    int matches = 0;
    uint8_t* at = FindCode(sig, std::size(sig), &matches);
    if (!at) {
        Log("network fix: this game version is not recognised (%d matches), left as in the game", matches);
        return;
    }
    uint8_t* je = at + 20;    // 6 bytes
    uint8_t* jbe = at + 28;   // 6 bytes
    const int32_t dJe = Read<int32_t>(je + 2), dJbe = Read<int32_t>(jbe + 2);
    if (je + 6 + dJe != jbe + 6 + dJbe) {   // both must lead to "process"
        Log("network fix: unexpected code, left as in the game");
        return;
    }
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t d = dJe + 1;              // jmp rel32 is one byte shorter than je rel32
    memcpy(jmp + 1, &d, 4);
    if (PatchCode(je, jmp, sizeof jmp)) Log("network fix: client packets are processed as they arrive");
}

// ---------------------------------------------------------------------------
// FastPhysics: settings, statistics
// ---------------------------------------------------------------------------
int g_warmStart = 1, g_verify = 0, g_logSeconds = 0;
LARGE_INTEGER g_freq;
LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double Ms(LONGLONG ticks) { return 1000.0 * (double)ticks / (double)g_freq.QuadPart; }

CRITICAL_SECTION g_statsCs;
struct Stats {
    int calls = 0, fast = 0, fallback = 0, invalid = 0, warm = 0, retries = 0, maxRows = 0, verified = 0;
    LONGLONG tFast = 0, tFastMax = 0, tGame = 0;
} g_stats;
LONGLONG g_lastReport = 0;

void Report() {   // g_statsCs held
    const Stats s = g_stats;
    g_stats = Stats();
    if (!s.calls) return;
    Log("fast physics: %d solves, avg %.3f ms, max %.3f ms, up to %d rows, %d warm starts, %d retries, %d left to the game, %d unknown layout",
        s.calls, s.fast ? Ms(s.tFast) / s.fast : 0.0, Ms(s.tFastMax), s.maxRows, s.warm, s.retries, s.fallback, s.invalid);
    if (s.verified) Log("fast physics: the game's own solver for comparison: avg %.3f ms over %d solves", Ms(s.tGame) / s.verified, s.verified);
}

// ---------------------------------------------------------------------------
// Warm start: which rows ended strictly inside their bounds the last time the
// same problem was solved. A wrong guess costs time, never correctness.
// ---------------------------------------------------------------------------
struct CacheEntry {
    uint64_t key = 0;
    int n = 0;
    LONGLONG lastUse = 0;
    std::vector<unsigned char> interior;
};
CacheEntry g_cache[32];
SRWLOCK g_cacheLock = SRWLOCK_INIT;

uint64_t HashBytes(uint64_t h, const void* p, size_t size) {
    auto b = (const uint8_t*)p;
    for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// Bound values drift every tick (motors, pistons), so only the structure counts.
uint64_t HashProblem(int n, const float* lo, const float* hi, const int* dep) {
    uint64_t h = HashBytes(14695981039346656037ull, &n, sizeof n);
    h = HashBytes(h, dep, sizeof(int) * (size_t)n);
    for (int i = 0; i < n; ++i) {
        unsigned char kind;
        if (dep[i] >= 0) kind = 4;
        else if (lo[i] <= -FLT_MAX && hi[i] >= FLT_MAX) kind = 0;
        else if (lo[i] == 0.0f) kind = 1;
        else if (hi[i] == 0.0f) kind = 3;
        else if (lo[i] < 0.0f && hi[i] > 0.0f) kind = 2;
        else kind = 5;
        h ^= kind;
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

bool CacheLookup(uint64_t key, int n, std::vector<unsigned char>& out) {
    bool found = false;
    AcquireSRWLockShared(&g_cacheLock);
    for (CacheEntry& e : g_cache)
        if (e.key == key && e.n == n) { out = e.interior; found = true; break; }
    ReleaseSRWLockShared(&g_cacheLock);
    return found && (int)out.size() == n;
}

void CacheStore(uint64_t key, int n, const std::vector<unsigned char>& interior) {
    const LONGLONG now = Now();
    AcquireSRWLockExclusive(&g_cacheLock);
    CacheEntry* target = nullptr;
    for (CacheEntry& e : g_cache)
        if (e.key == key && e.n == n) { target = &e; break; }
    if (!target) {
        target = &g_cache[0];
        for (CacheEntry& e : g_cache) if (e.lastUse < target->lastUse) target = &e;
    }
    target->key = key;
    target->n = n;
    target->lastUse = now;
    target->interior = interior;
    ReleaseSRWLockExclusive(&g_cacheLock);
}

// ---------------------------------------------------------------------------
// bool btDantzigSolver::solveMLCP(const btMatrixXu& A, const btVectorXu& b, btVectorXu& x, const btVectorXu& lo,
//     const btVectorXu& hi, const btAlignedObjectArray<int>& limitDependency, int numIterations, bool useSparsity)
// btMatrixX: rows +0, cols +4, storage size +1Ch, storage data +28h.
// btVectorX / btAlignedObjectArray: size +4, data +10h.
// ---------------------------------------------------------------------------
using SolveFn = bool (*)(void*, const uint8_t*, const uint8_t*, uint8_t*, const uint8_t*, const uint8_t*, const uint8_t*, int, uintptr_t);
SolveFn g_gameSolve = nullptr;
LONG g_verifyLeft = 0;
// Created on the first solve of a thread, not for every thread of the game.
struct Work {
    FastLcpScratch scratch;
    std::vector<unsigned char> guess, interior;
    std::vector<float> gameX;
};

bool SolveHook(void* self, const uint8_t* A, const uint8_t* b, uint8_t* x, const uint8_t* lo, const uint8_t* hi, const uint8_t* dep,
               int numIterations, uintptr_t sparsity) {
    const int n = Read<int>(A);
    auto aD = Read<const float*>(A + 0x28);
    auto bD = Read<const float*>(b + 0x10);
    auto xD = Read<float*>(x + 0x10);
    auto loD = Read<const float*>(lo + 0x10);
    auto hiD = Read<const float*>(hi + 0x10);
    auto depD = Read<const int*>(dep + 0x10);
    const bool known = n > 0 && Read<int>(A + 4) == n && Read<int>(A + 0x1C) == n * n && aD && bD && xD && loD && hiD && depD &&
                       Read<int>(b + 4) == n && Read<int>(x + 4) == n && Read<int>(lo + 4) == n && Read<int>(hi + 4) == n && Read<int>(dep + 4) == n;
    if (!known) {
        EnterCriticalSection(&g_statsCs);
        ++g_stats.calls;
        ++g_stats.invalid;
        LeaveCriticalSection(&g_statsCs);
        return g_gameSolve(self, A, b, x, lo, hi, dep, numIterations, sparsity);
    }

    static thread_local Work work;
    FastLcpScratch& t_scratch = work.scratch;
    std::vector<unsigned char>&t_guess = work.guess, &t_interior = work.interior;
    std::vector<float>& t_gameX = work.gameX;

    const unsigned char* guess = nullptr;
    uint64_t key = 0;
    if (g_warmStart) {
        key = HashProblem(n, loD, hiD, depD);
        if (CacheLookup(key, n, t_guess)) guess = t_guess.data();
    }

    // Verify mode: the game's solver first, on the untouched problem (only problems big enough to say anything).
    const bool verify = n >= 32 && g_verifyLeft > 0 && InterlockedDecrement(&g_verifyLeft) >= 0;
    LONGLONG tGame = 0;
    bool gameOk = false;
    if (verify) {
        const LONGLONG t = Now();
        gameOk = g_gameSolve(self, A, b, x, lo, hi, dep, numIterations, sparsity);
        tGame = Now() - t;
        t_gameX.assign(xD, xD + n);
    }

    // Dantzig can stall on a degenerate pivot (so can the game's). Retry with other arithmetic / pivot order.
    t_interior.resize((size_t)n);
    const LONGLONG t0 = Now();
    bool ok = FastSolveLcp(n, aD, bD, loD, hiD, depD, xD, t_scratch, guess, t_interior.data(), 0), retried = false;
    if (!ok && guess) { retried = true; ok = FastSolveLcp(n, aD, bD, loD, hiD, depD, xD, t_scratch, nullptr, t_interior.data(), 0); }
    if (!ok) { retried = true; ok = FastSolveLcp(n, aD, bD, loD, hiD, depD, xD, t_scratch, nullptr, t_interior.data(), 2); }
    if (!ok) { retried = true; ok = FastSolveLcp(n, aD, bD, loD, hiD, depD, xD, t_scratch, nullptr, t_interior.data(), 8); }
    for (int i = 0; ok && i < n; ++i) if (xD[i] != xD[i]) ok = false;   // NaN: the game rejects those too
    const LONGLONG dt = Now() - t0;
    if (ok && g_warmStart) CacheStore(key, n, t_interior);

    if (verify) {
        double maxDiff = 0, maxX = 0;
        for (int i = 0; i < n; ++i) {
            maxDiff = std::fmax(maxDiff, std::fabs((double)t_gameX[i] - xD[i]));
            maxX = std::fmax(maxX, std::fabs((double)t_gameX[i]));
        }
        Log("fast physics verify: %d rows, ours %.3f ms (%s%s), game %.3f ms (%s), relative difference %.1e, %s start", n, Ms(dt),
            ok ? "ok" : "failed", retried ? ", retried" : "", Ms(tGame), gameOk ? "ok" : "failed", maxX > 0 ? maxDiff / maxX : 0.0,
            guess ? "warm" : "cold");
    }

    EnterCriticalSection(&g_statsCs);
    ++g_stats.calls;
    if (retried) ++g_stats.retries;
    if (ok) {
        ++g_stats.fast;
        g_stats.tFast += dt;
        if (dt > g_stats.tFastMax) g_stats.tFastMax = dt;
        if (n > g_stats.maxRows) g_stats.maxRows = n;
        if (guess) ++g_stats.warm;
    } else {
        ++g_stats.fallback;
    }
    if (verify) { ++g_stats.verified; g_stats.tGame += tGame; }
    const LONGLONG now = Now();
    if (g_logSeconds > 0 && Ms(now - g_lastReport) >= 1000.0 * g_logSeconds) {
        g_lastReport = now;
        Report();
    }
    LeaveCriticalSection(&g_statsCs);
    return ok ? true : g_gameSolve(self, A, b, x, lo, hi, dep, numIterations, sparsity);
}

bool CpuHasAvx2Fma() {
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool fma = (info[2] & (1 << 12)) != 0, osxsave = (info[2] & (1 << 27)) != 0, avx = (info[2] & (1 << 28)) != 0;
    if (!fma || !osxsave || !avx || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
}

uint8_t* FindBytes(const Range& r, const void* needle, size_t size, uint8_t* from) {
    for (uint8_t* p = from; p + size <= r.end; ++p)
        if (*p == *(const uint8_t*)needle && !memcmp(p, needle, size)) return p;
    return nullptr;
}

// MSVC RTTI: type descriptor name -> complete object locator -> vtable.
void** FindVTable(const char* mangledName) {
    const size_t nameSize = strlen(mangledName) + 1;
    for (const Range* r : {&g_data, &g_rdata}) {
        for (uint8_t* name = FindBytes(*r, mangledName, nameSize, r->begin); name; name = FindBytes(*r, mangledName, nameSize, name + 1)) {
            const uint32_t td = (uint32_t)(name - 0x10 - g_base);
            for (uint8_t* col = g_rdata.begin; col + 24 <= g_rdata.end; col += 4) {
                if (Read<uint32_t>(col) != 1 || Read<uint32_t>(col + 4) != 0 || Read<uint32_t>(col + 12) != td ||
                    Read<uint32_t>(col + 20) != (uint32_t)(col - g_base)) continue;
                for (uint8_t* slot = g_rdata.begin; slot + 16 <= g_rdata.end; slot += 8)
                    if (Read<uint8_t*>(slot) == col) return (void**)(slot + 8);
            }
        }
    }
    return nullptr;
}

void InstallFastPhysics() {
    if (!CpuHasAvx2Fma()) { Log("fast physics: needs a CPU with AVX2 and FMA, the game's solver is used"); return; }
    void** vt = FindVTable(".?AVbtDantzigSolver@@");
    if (!vt || !g_rdata.Contains(vt)) { Log("fast physics: solver not found, left as in the game"); return; }
    if (!g_text.Contains(vt[1])) { Log("fast physics: another mod already replaced the solver (FastPhysics?), left alone"); return; }
    InitializeCriticalSection(&g_statsCs);
    QueryPerformanceFrequency(&g_freq);
    g_lastReport = Now();
    g_verifyLeft = g_verify;
    g_gameSolve = (SolveFn)vt[1];
    void* hook = (void*)&SolveHook;
    if (!PatchCode(&vt[1], &hook, sizeof hook)) { Log("fast physics: could not install"); return; }
    Log("fast physics: constraint solver replaced (warm start %d)", g_warmStart);
}

}  // namespace

unsigned char* FindCode(const int* sig, size_t len, int* matches) {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* found = nullptr;
    int count = 0;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) || sec->Misc.VirtualSize < len) continue;
        uint8_t* b = base + sec->VirtualAddress;
        for (uint8_t* p = b; p <= b + sec->Misc.VirtualSize - len; ++p) {
            if (p[0] != sig[0] || p[1] != sig[1]) continue;
            size_t k = 2;
            while (k < len && (sig[k] < 0 || p[k] == sig[k])) ++k;
            if (k == len) { found = p; ++count; }
        }
    }
    if (matches) *matches = count;
    return count == 1 ? found : nullptr;
}

bool PatchCode(void* at, const void* bytes, size_t len) {
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

void InstallGameFixes(const wchar_t* ini, void (*log)(const char* fmt, ...)) {
    Log = log;
    auto get = [&](const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"fixes", key, def, ini); };
    if (!InitSections()) { Log("fixes: unexpected game executable, none applied"); return; }
    if (get(L"NetworkFix", 1)) InstallNetworkFix();
    else Log("network fix: off (loader.ini)");
    if (get(L"FastPhysics", 1)) {
        g_warmStart = get(L"PhysicsWarmStart", 1);
        g_verify = get(L"PhysicsVerify", 0);
        g_logSeconds = get(L"PhysicsLogSeconds", 0);
        InstallFastPhysics();
    } else {
        Log("fast physics: off (loader.ini)");
    }
}
