// SMContent: lets any number of mods add or edit game data files in memory,
// without changing anything in the game folder.
//
// How the game loads data (reverse-engineered on 1.0.7a.903):
//   * JSON (shape sets, tool sets, recipes, descriptions, GUIs) and raw text
//     (Lua, XML) are precompiled into Cache\Bundle\*.cbo. The source files are
//     only read when the cache has no entry for them.
//   * Every lookup goes through GenericCache: LoadFromBundle / LoadFromFile take
//     the source path and return an error code (0 = hit, 2 = not found).
//   * On a miss the source is read through a FILE* (MSVCP140 _Fiopen), compiled,
//     and written back with StoreToBundle / StoreToFile.
// So, for registered paths only: both loads report "not found", the file open is
// redirected to the merged copy, and nothing from a mod ever ends up in the
// game's cache (converted meshes and textures go to a cache of our own).
//
// Older mods (SMWaystones 1.0) carry their own copy of these hooks. The loader
// installs its hooks after every mod, so it finds theirs in place and chains to
// them, and it reads the "unmodified" file through the chain: their edits are
// part of the base text our mods see.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "content.h"

namespace {

void (*Log)(const char* fmt, ...) = nullptr;

// MSVC std::string as the game lays it out (read only).
struct GameString {
    union { char buf[16]; const char* ptr; };
    size_t size;
    size_t cap;
    const char* data() const { return cap > 15 ? ptr : buf; }
};

struct Edit {
    SMContentEditFn fn = nullptr;
    void*           user = nullptr;
    std::string     text;        // ContentAdd: the whole file
};

struct VFile {
    std::string       gamePath;     // "$SURVIVAL_DATA/..." as registered
    std::wstring      diskPath;     // <game>\Survival\...  (where the game would look)
    std::wstring      servedPath;   // the merged copy
    std::vector<Edit> edits;        // in registration order
    bool              built = false, ok = false, used = false;
    uint32_t          hash = 0;     // of the merged text (reported to the game as the file checksum)
};

struct Icon {
    std::string                uuid;
    std::vector<unsigned char> pixels;   // 96x96 BGRA, top row first
    int                        x = 0, y = 0;
};

std::mutex   g_regMx;
bool         g_installed = false;
std::map<std::string, VFile*>  g_byGamePath;   // lower-case "$survival_data/..."
std::map<std::wstring, VFile*> g_byDiskPath;   // lower-case, '/' separators
std::vector<Icon>              g_icons;
std::mutex   g_buildMx;
std::wstring g_gameRoot;                       // lower-case, '/' separators, trailing '/'

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string LowerGamePath(const char* p, size_t n) {
    std::string r;
    r.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        char c = p[i] == '\\' ? '/' : p[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '/' && !r.empty() && r.back() == '/') continue;
        r += c;
    }
    return r;
}

VFile* FindByGamePath(const GameString* s) {
    if (!s || s->size == 0 || s->size > 1024 || s->cap < s->size) return nullptr;
    const char* p = s->data();
    if (p[0] != '$') return nullptr;
    auto it = g_byGamePath.find(LowerGamePath(p, s->size));
    return it == g_byGamePath.end() ? nullptr : it->second;
}

std::wstring NormalizeDisk(const wchar_t* p) {
    std::wstring r;
    for (; *p; ++p) {
        wchar_t c = *p == L'\\' ? L'/' : *p;
        if (c >= L'A' && c <= L'Z') c += 32;
        if (c == L'/' && !r.empty() && r.back() == L'/') continue;
        r += c;
    }
    return r;
}

// Runs for every file the game opens.
VFile* FindByDiskPath(const wchar_t* p) {
    if (!p || !*p) return nullptr;
    size_t n = wcslen(p);
    if (n < 5 || n > 1024) return nullptr;
    std::wstring w = NormalizeDisk(p);
    if (w.compare(0, 2, L"./") == 0) w.erase(0, 2);
    if (w.compare(0, 3, L"../") == 0) w = g_gameRoot + w.substr(3);   // the game runs in Release\ or the root
    else if (w.size() > 1 && w[1] != L':') w = g_gameRoot + w;
    auto it = g_byDiskPath.find(w);
    return it == g_byDiskPath.end() ? nullptr : it->second;
}

// --- imported file functions -------------------------------------------------
typedef FILE* (*FiopenFn)(const wchar_t*, int, int);
FiopenFn                        o_Fiopen = nullptr;
decltype(&CreateFileW)          o_CreateFileW = nullptr;
decltype(&_wfopen)              o_wfopen = nullptr;
decltype(&_wfopen_s)            o_wfopen_s = nullptr;
typedef int (*StatFn)(const wchar_t*, struct _stat64*);
StatFn                          o_wstat64 = nullptr;
decltype(&GetFileAttributesW)   o_GetFileAttributesW = nullptr;
decltype(&GetFileAttributesExW) o_GetFileAttributesExW = nullptr;

// The file without our edits, but with those of mods that hooked the game
// themselves (their hooks are behind o_*).
std::string ReadBase(const std::wstring& path) {
    std::string out;
    HANDLE h = (o_CreateFileW ? o_CreateFileW : CreateFileW)(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::vector<char> buf(65536);
    DWORD got = 0;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) out.append(buf.data(), got);
    CloseHandle(h);
    return out;
}

// A mod's mistake must not reach the game: without (say) its shape sets the game
// would load a world with the parts missing. Accepts what the game's own files
// use: comments, and nothing else beyond standard JSON.
struct JsonCheck {
    const char *p, *end;
    int depth = 0;
    JsonCheck(const char* b, const char* e) : p(b), end(e) {}
    void Skip() {
        while (p < end) {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { ++p; continue; }
            if (end - p >= 2 && p[0] == '/' && p[1] == '/') { while (p < end && *p != '\n') ++p; continue; }
            if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (end - p >= 2 && !(p[0] == '*' && p[1] == '/')) ++p;
                p = end - p >= 2 ? p + 2 : end;
                continue;
            }
            break;
        }
    }
    bool Is(char c) { Skip(); if (p < end && *p == c) { ++p; return true; } return false; }
    bool String() {
        if (!Is('"')) return false;
        while (p < end && *p != '"') p += *p == '\\' && end - p >= 2 ? 2 : 1;
        if (p >= end) return false;
        ++p;
        return true;
    }
    bool Scalar() {
        const char* s = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) ++p;
        return p > s;
    }
    bool Value() {
        if (depth > 200) return false;
        Skip();
        if (p >= end) return false;
        if (*p == '"') return String();
        const char open = *p;
        if (open != '{' && open != '[') return Scalar();
        ++p; ++depth;
        const char close = open == '{' ? '}' : ']';
        if (!Is(close)) {
            for (;;) {
                if (open == '{' && (!String() || !Is(':'))) return false;
                if (!Value()) return false;
                if (Is(close)) break;
                if (!Is(',')) return false;
            }
        }
        --depth;
        return true;
    }
    static bool Ok(const std::string& s) {
        JsonCheck c(s.data(), s.data() + s.size());
        if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) c.p += 3;
        if (!c.Value()) return false;
        c.Skip();
        return c.p == c.end;
    }
};

// .json, and the set files (.shapeset, .projectileset, .effectset, ...).
bool IsJson(const std::string& path) {
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    return ext == "json" || (ext.size() > 3 && ext.compare(ext.size() - 3, 3, "set") == 0);
}

bool Build(VFile* f) {
    std::lock_guard<std::mutex> lk(g_buildMx);
    if (f->built) return f->ok;
    f->built = true;
    std::string text = ReadBase(f->diskPath);
    bool changed = false;
    for (Edit& e : f->edits) {
        std::string result = e.text;
        if (e.fn) {
            size_t len = 0;
            const char* r = e.fn(f->gamePath.c_str(), text.data(), text.size(), &len, e.user);
            if (!r) { Log("content: a mod could not edit %s - that edit is skipped", f->gamePath.c_str()); continue; }
            result.assign(r, len);
        }
        if (IsJson(f->gamePath) && !JsonCheck::Ok(result)) {
            Log("content: ERROR: a mod's version of %s is not valid JSON - that edit is skipped", f->gamePath.c_str());
            continue;
        }
        text = std::move(result);
        changed = true;
    }
    if (!changed) return false;
    FILE* out = _wfopen(f->servedPath.c_str(), L"wb");
    if (!out) {
        Log("content: cannot write the merged copy of %s", f->gamePath.c_str());
        return false;
    }
    bool written = fwrite(text.data(), 1, text.size(), out) == text.size();
    fclose(out);
    if (!written) { Log("content: writing the merged copy of %s failed", f->gamePath.c_str()); return false; }
    f->hash = 2166136261u;
    for (unsigned char c : text) f->hash = (f->hash ^ c) * 16777619u;
    f->ok = true;
    return true;
}

// Path to open instead of p, or p itself.
const wchar_t* Redirect(const wchar_t* p) {
    VFile* f = FindByDiskPath(p);
    if (!f || !Build(f)) return p;
    if (!f->used) {
        f->used = true;
        Log("content: serving %s", f->gamePath.c_str());
    }
    return f->servedPath.c_str();
}

FILE* H_Fiopen(const wchar_t* p, int mode, int prot) { return o_Fiopen(Redirect(p), mode, prot); }
HANDLE WINAPI H_CreateFileW(LPCWSTR p, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (!(access & GENERIC_WRITE) && disp == OPEN_EXISTING) p = Redirect(p);
    return o_CreateFileW(p, access, share, sa, disp, flags, tmpl);
}
bool IsRead(const wchar_t* mode) { return mode && mode[0] == L'r' && !wcschr(mode, L'+'); }
FILE* H_wfopen(const wchar_t* p, const wchar_t* mode) { return o_wfopen(IsRead(mode) ? Redirect(p) : p, mode); }
errno_t H_wfopen_s(FILE** f, const wchar_t* p, const wchar_t* mode) { return o_wfopen_s(f, IsRead(mode) ? Redirect(p) : p, mode); }
int H_wstat64(const wchar_t* p, struct _stat64* st) { return o_wstat64(Redirect(p), st); }   // the size of a mesh file
DWORD WINAPI H_GetFileAttributesW(LPCWSTR p) { return o_GetFileAttributesW(Redirect(p)); }
BOOL WINAPI H_GetFileAttributesExW(LPCWSTR p, GET_FILEEX_INFO_LEVELS lvl, LPVOID info) {
    return o_GetFileAttributesExW(Redirect(p), lvl, info);
}

// --- GenericCache ------------------------------------------------------------
const int kCacheMiss = 2;   // the game's own "not found" (loads return an error code)
const int kStored = 1;      // stores return a bool

typedef int (*CacheFn)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
CacheFn o_LoadBundle = nullptr, o_LoadFile = nullptr, o_StoreBundle = nullptr, o_StoreFile = nullptr;

VFile* Ours(const void* srcPath) {
    VFile* f = FindByGamePath((const GameString*)srcPath);
    return f && Build(f) ? f : nullptr;
}

// Converting a texture or a mesh takes the game seconds, so the results are kept
// in SMLoader\content\cache (never in the game's cache), named after what they
// were made from: <path and settings>-<content>.bin
typedef void* (*MallocFn)(size_t);
MallocFn g_gameMalloc = nullptr;                  // the game frees the buffer with its own C runtime
std::wstring g_cacheDir;

uint32_t Fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}

bool IsBinary(const VFile* f) {
    size_t dot = f->gamePath.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : f->gamePath.substr(dot + 1);
    return e == "png" || e == "tga" || e == "dae" || e == "fbx" || e == "jpg" || e == "dds";
}

std::wstring CachePrefix(const VFile* f, const GameString* name, size_t verLen, const void* ver) {
    wchar_t w[32];
    uint32_t h = Fnv(f->gamePath.data(), f->gamePath.size());
    uint32_t s = Fnv(ver, verLen, Fnv(name->data(), name->size));
    swprintf(w, 32, L"%08x%08x-", h, s);
    return g_cacheDir + L"\\" + w;
}
std::wstring CacheFile(const VFile* f, const GameString* name, size_t verLen, const void* ver) {
    wchar_t w[16];
    swprintf(w, 16, L"%08x.bin", f->hash);
    return CachePrefix(f, name, verLen, ver) + w;
}

// The game's load result: 0 = hit.
int CacheLoad(const VFile* f, const void* name, uintptr_t verLen, const void* ver, uint32_t* outSize, void** outData, uint32_t* outCrc) {
    verLen &= 0xFFFFFFFF;
    if (!g_gameMalloc || !IsBinary(f) || !name || !ver || verLen > 256 || !outSize || !outData) return kCacheMiss;
    HANDLE h = CreateFileW(CacheFile(f, (const GameString*)name, verLen, ver).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return kCacheMiss;
    int result = kCacheMiss;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    if (size != INVALID_FILE_SIZE && size > 0)
        if (void* data = g_gameMalloc(size)) {
            if (ReadFile(h, data, size, &got, nullptr) && got == size) {
                *outSize = size;
                *outData = data;
                if (outCrc) *outCrc = f->hash;
                result = 0;
            }                                     // a failed read leaks `data`: only the game could free it
        }
    CloseHandle(h);
    return result;
}

int CacheStore(const VFile* f, const void* name, uintptr_t verLen, const void* ver, uintptr_t size, const void* data, uint32_t* outCrc) {
    if (outCrc) *outCrc = f->hash;
    verLen &= 0xFFFFFFFF;
    size &= 0xFFFFFFFF;
    if (!IsBinary(f) || !name || !ver || verLen > 256 || !data || !size) return kStored;
    std::lock_guard<std::mutex> lk(g_buildMx);
    std::wstring pre = CachePrefix(f, (const GameString*)name, verLen, ver);
    WIN32_FIND_DATAW fd;                          // results from older versions of the file
    HANDLE ff = FindFirstFileW((pre + L"*.bin").c_str(), &fd);
    if (ff != INVALID_HANDLE_VALUE) {
        do DeleteFileW((g_cacheDir + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(ff, &fd));
        FindClose(ff);
    }
    std::wstring path = CacheFile(f, (const GameString*)name, verLen, ver), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return kStored;
    DWORD put = 0;
    bool ok = WriteFile(h, data, (DWORD)size, &put, nullptr) && put == size;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    return kStored;
}

// (bundle, cacheName, srcPath, verLen, ver, outSize, outData, outCrc, bool)
int H_LoadBundle(void* a, void* b, void* src, void* d, void* e, void* f, void* g, void* h, void* i, void* j) {
    if (VFile* own = Ours(src)) return CacheLoad(own, b, (uintptr_t)d, e, (uint32_t*)f, (void**)g, (uint32_t*)h);
    return o_LoadBundle(a, b, src, d, e, f, g, h, i, j);
}
// (cacheName, srcPath, verLen, ver, outSize, outData, outCrc, bool)
int H_LoadFile(void* a, void* src, void* c, void* d, void* e, void* f, void* g, void* h, void* i, void* j) {
    if (VFile* own = Ours(src)) return CacheLoad(own, a, (uintptr_t)c, d, (uint32_t*)e, (void**)f, (uint32_t*)g);
    return o_LoadFile(a, src, c, d, e, f, g, h, i, j);
}
// (bundle, cacheName, srcPath, verLen, ver, size, data, outCrc)
int H_StoreBundle(void* a, void* b, void* src, void* d, void* e, void* f, void* g, void* outCrc, void* i, void* j) {
    if (VFile* own = Ours(src)) return CacheStore(own, b, (uintptr_t)d, e, (uintptr_t)f, g, (uint32_t*)outCrc);
    return o_StoreBundle(a, b, src, d, e, f, g, outCrc, i, j);
}
// (cacheName, srcPath, verLen, ver, size, data, outCrc)
int H_StoreFile(void* a, void* src, void* c, void* d, void* e, void* f, void* outCrc, void* h, void* i, void* j) {
    if (VFile* own = Ours(src)) return CacheStore(own, a, (uintptr_t)c, d, (uintptr_t)e, f, (uint32_t*)outCrc);
    return o_StoreFile(a, src, c, d, e, f, outCrc, h, i, j);
}

// --- DataLibrary ---------------------------------------------------------------
// For meshes and textures the game asks its index of shipped files, not the disk.
typedef bool (*ExistsFn)(const void* path);
ExistsFn o_FileExists = nullptr;
bool H_FileExists(const void* path) { return Ours(path) || o_FileExists(path); }
const char* kFileExistsSig =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 8B F9 48 85 DB 74 30 0F B6 83 DA 02 00 00 90 84 C0 74 24 "
    "48 8B D1 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 15";

// --- signature scan + inline hook ---------------------------------------------
struct Target {
    const char* name;
    const char* sig;      // "??" = any byte
    size_t      steal;    // whole, position-independent instructions at the start (>= 14)
    CacheFn     hook;
    CacheFn*    original;
    uint8_t*    addr;
};

Target g_targets[] = {
    {"GenericCache::LoadFromBundle",
     "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? "
     "48 33 C4 48 89 45 F7 45 8B E9 4C 89 45 97 48 8B F2 4C 8B F9 48 8B 45 6F",
     16, H_LoadBundle, &o_LoadBundle, nullptr},
    {"GenericCache::LoadFromFile",
     "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 10 FF FF FF 48 81 EC F0 01 00 00 48 8B 05 ?? ?? ?? ?? "
     "48 33 C4 48 89 85 E8 00 00 00 4C 89 4C 24 68 44 89 44 24 38 48 89 54 24 48",
     16, H_LoadFile, &o_LoadFile, nullptr},
    {"GenericCache::StoreToBundle",
     "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC F8 00 00 00 48 8B 05 ?? ?? ?? ?? "
     "48 33 C4 48 89 45 F7 45 8B E1 44 89 4C 24 68 49 8B D8 4C 8B EA 48 8B F9",
     18, H_StoreBundle, &o_StoreBundle, nullptr},
    {"GenericCache::StoreToFile",
     "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A8 FE FF FF 48 81 EC 58 02 00 00 0F 29 B4 24 40 02 00 00 "
     "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 38 01 00 00 4C 89 4D 80 45 8B E0 48 8B DA",
     21, H_StoreFile, &o_StoreFile, nullptr},
};

int Hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

const uint8_t kAbsJump[6] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0]; dq target

// Address of the only match, or null (none, or more than one). The first `steal`
// bytes may also be an absolute jump: another mod's hook.
uint8_t* ScanUnique(const char* sig, size_t steal) {
    std::vector<int> pat;
    for (const char* p = sig; *p;) {
        if (*p == ' ') { ++p; continue; }
        pat.push_back(*p == '?' ? -1 : Hex(p[0]) * 16 + Hex(p[1]));
        p += 2;
    }
    auto matches = [&](const uint8_t* p, size_t from, size_t to) {
        for (size_t k = from; k < to; ++k) if (pat[k] >= 0 && p[k] != pat[k]) return false;
        return true;
    };
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* found = nullptr;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* b = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        if (size < pat.size()) continue;
        for (uint8_t* p = b; p <= b + size - pat.size(); ++p) {
            if (p[steal] != pat[steal] || p[steal + 1] != pat[steal + 1]) continue;
            if (!matches(p, steal, pat.size())) continue;
            if (!matches(p, 0, steal) && memcmp(p, kAbsJump, sizeof kAbsJump) != 0) continue;
            if (found) return nullptr;
            found = p;
        }
    }
    return found;
}

void PutJump(uint8_t* at, const void* to) {
    memcpy(at, kAbsJump, 6);
    memcpy(at + 6, &to, 8);
}

// Rewriting code other threads may be running: they are stopped for the moment it
// takes, and none of them may be inside the bytes being replaced. Nothing between
// suspend and resume may allocate (a stopped thread could hold the heap lock).
bool PatchStopped(uint8_t* at, const uint8_t* code, size_t n) {
    static HANDLE threads[1024];
    int count = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok && count < 1024; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID)) threads[count++] = h;
        CloseHandle(snap);
    }
    DWORD old;
    bool done = false;
    if (VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        for (int attempt = 0; attempt < 200 && !done; ++attempt) {
            bool busy = false;
            for (int i = 0; i < count; ++i) SuspendThread(threads[i]);
            for (int i = 0; i < count; ++i) {
                CONTEXT c;
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(threads[i], &c) && c.Rip > (DWORD64)at && c.Rip < (DWORD64)at + n) busy = true;
            }
            if (!busy) {
                for (size_t k = 0; k < n; ++k) at[k] = code[k];
                FlushInstructionCache(GetCurrentProcess(), at, n);
                done = true;
            }
            for (int i = 0; i < count; ++i) ResumeThread(threads[i]);
            if (!done) Sleep(1);
        }
        VirtualProtect(at, n, old, &old);
    }
    for (int i = 0; i < count; ++i) CloseHandle(threads[i]);
    return done;
}

bool HookCode(uint8_t* at, size_t steal, const void* hook, void** original, bool* chained) {
    *chained = memcmp(at, kAbsJump, sizeof kAbsJump) == 0;
    uint8_t code[32];
    memset(code, 0x90, sizeof code);
    PutJump(code, hook);
    if (*chained) {
        memcpy(original, at + 6, 8);                   // the other mod's hook; it calls the game's code
    } else {
        uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tramp) return false;
        memcpy(tramp, at, steal);
        PutJump(tramp + steal, at + steal);
        DWORD old;
        VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
        *original = tramp;
    }
    return PatchStopped(at, code, *chained ? 14 : steal);
}

// For functions whose first 14 bytes can't be moved: a 5-byte jump to a stub within
// reach. `steal` = whole, position-independent instructions at the start (>= 5).
bool NearHook(uint8_t* at, size_t steal, const void* hook, void** original) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* mem = nullptr;
    for (uintptr_t d = si.dwAllocationGranularity; !mem && d < 0x70000000; d += si.dwAllocationGranularity) {
        uintptr_t low = ((uintptr_t)GetModuleHandleW(nullptr) - d) & ~(uintptr_t)(si.dwAllocationGranularity - 1);
        mem = (uint8_t*)VirtualAlloc((void*)low, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!mem) return false;
    memcpy(mem, at, steal);                       // the original's start, then back
    PutJump(mem + steal, at + steal);
    PutJump(mem + 64, hook);
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    *original = mem;
    uint8_t jmp[16];
    memset(jmp, 0x90, sizeof jmp);
    int32_t rel = (int32_t)((mem + 64) - (at + 5));
    jmp[0] = 0xE9;
    memcpy(jmp + 1, &rel, 4);
    return PatchStopped(at, jmp, steal);
}

std::wstring DiskPathFor(const std::string& gamePath) {
    static const struct { const char* alias; const wchar_t* dir; } map[] = {
        {"$SURVIVAL_DATA/", L"Survival\\"}, {"$GAME_DATA/", L"Data\\"}, {"$CHALLENGE_DATA/", L"ChallengeData\\"}};
    for (auto& m : map) {
        size_t n = strlen(m.alias);
        if (gamePath.compare(0, n, m.alias) == 0) {
            std::wstring w = m.dir + Widen(gamePath.substr(n));
            for (auto& c : w) if (c == L'/') c = L'\\';
            return w;
        }
    }
    return L"";
}

VFile* FileFor(const char* gamePath) {
    std::string path = gamePath ? gamePath : "";
    if (DiskPathFor(path).empty() || path.find("..") != std::string::npos) return nullptr;
    VFile*& f = g_byGamePath[LowerGamePath(path.data(), path.size())];
    if (!f) {
        f = new VFile;
        f->gamePath = path;
    }
    return f;
}

// --- item icons ---------------------------------------------------------------
// The game maps an item to one fixed image set per content pack
// ("ItemIconsSetSurvival0" = IconMapSurvival.png), so an icon from a separate
// file is not possible. It loads the sheet with FreeImage_LoadU: the icons are
// pasted into the empty lower part of the decoded bitmap. (The game's own icons
// end near y = 2600; SMWaystones 1.0 uses the corner at 4000,4000.)
const int   kSheet = 4096, kIconSize = 96, kIconsPerRow = kSheet / kIconSize, kLastRowY = 3888, kRows = 10;
const char* kIconMap = "$SURVIVAL_DATA/Gui/IconMapSurvival.xml";

typedef void*    (__stdcall *LoadUFn)(int, const wchar_t*, int);
typedef unsigned (__stdcall *UnsignedFn)(void*);
typedef uint8_t* (__stdcall *BitsFn)(void*);
typedef int      (__stdcall *FileTypeFn)(const wchar_t*, int);
LoadUFn    o_LoadU = nullptr;
FileTypeFn o_GetFileTypeU = nullptr;
bool       g_iconsReady = false;
UnsignedFn f_bpp, f_width, f_height, f_pitch;
BitsFn     f_bits;

bool IsSheet(const wchar_t* p) {
    static const wchar_t tail[] = L"survival/gui/iconmapsurvival.png";
    size_t n = p ? wcslen(p) : 0, t = wcslen(tail);
    if (n < t) return false;
    for (size_t i = 0; i < t; ++i) {
        wchar_t c = p[n - t + i];
        if ((c == L'\\' ? L'/' : (wchar_t)towlower(c)) != tail[i]) return false;
    }
    return true;
}

void PasteIcons(void* dib) {
    if (f_bpp(dib) != 32 || f_width(dib) != (unsigned)kSheet || f_height(dib) != (unsigned)kSheet) {
        Log("content: the icon sheet has an unexpected format - no item icons");
        return;
    }
    uint8_t* bits = f_bits(dib);
    const unsigned pitch = f_pitch(dib);
    int done = 0;
    for (const Icon& ic : g_icons) {
        auto row = [&](int y) { return bits + (size_t)(kSheet - 1 - (ic.y + y)) * pitch + ic.x * 4; };
        bool taken = false;                            // never overwrite a real icon
        for (int y = 0; y < kIconSize && !taken; ++y)
            for (int x = 0; x < kIconSize; ++x)
                if (row(y)[x * 4 + 3]) { taken = true; break; }
        if (taken) { Log("content: the icon slot of %s is taken", ic.uuid.c_str()); continue; }
        for (int y = 0; y < kIconSize; ++y) memcpy(row(y), ic.pixels.data() + (size_t)y * kIconSize * 4, kIconSize * 4);
        ++done;
    }
    Log("content: %d item icon(s) added to the sheet", done);
}

// FreeImage opens files by itself, so textures are redirected here.
void* __stdcall H_LoadU(int format, const wchar_t* path, int flags) {
    void* dib = o_LoadU(format, Redirect(path), flags);
    if (dib && g_iconsReady && IsSheet(path)) PasteIcons(dib);
    return dib;
}
int __stdcall H_GetFileTypeU(const wchar_t* path, int size) { return o_GetFileTypeU(Redirect(path), size); }

// The sheet must be the size we know.
bool SheetLooksRight(const std::wstring& root) {
    HANDLE h = CreateFileW((root + L"Survival\\Gui\\IconMapSurvival.png").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t b[24] = {};
    DWORD got = 0;
    ReadFile(h, b, sizeof b, &got, nullptr);
    CloseHandle(h);
    auto be = [&](int o) { return (unsigned)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]; };
    return got == 24 && b[1] == 'P' && b[2] == 'N' && be(16) == (unsigned)kSheet && be(20) == (unsigned)kSheet;
}

const char* EditIconMap(const char*, const char* base, size_t len, size_t* outLen, void*) {
    static std::string out;
    out.assign(base, len);
    size_t p = out.rfind("</Group>");
    if (p == std::string::npos) return nullptr;
    std::string add;
    for (const Icon& ic : g_icons)
        add += "    <Index name=\"" + ic.uuid + "\">\n                <Frame point=\"" + std::to_string(ic.x) + " " +
               std::to_string(ic.y) + "\"/>\n            </Index>\n        ";
    out.insert(p, add);
    *outLen = out.size();
    return out.data();
}

void InstallImages(const std::wstring& root, int (*patchImport)(const char*, const char*, void*, void**)) {
    HMODULE fi = GetModuleHandleW(L"FreeImage.dll");
    if (!g_icons.empty() && fi && SheetLooksRight(root)) {
        f_bpp    = (UnsignedFn)GetProcAddress(fi, "FreeImage_GetBPP");
        f_width  = (UnsignedFn)GetProcAddress(fi, "FreeImage_GetWidth");
        f_height = (UnsignedFn)GetProcAddress(fi, "FreeImage_GetHeight");
        f_pitch  = (UnsignedFn)GetProcAddress(fi, "FreeImage_GetPitch");
        f_bits   = (BitsFn)GetProcAddress(fi, "FreeImage_GetBits");
        g_iconsReady = f_bpp && f_width && f_height && f_pitch && f_bits;
    }
    if (g_iconsReady) {
        for (size_t i = 0; i < g_icons.size(); ++i) {
            g_icons[i].x = (int)(i % kIconsPerRow) * kIconSize;
            g_icons[i].y = kLastRowY - (int)(i / kIconsPerRow) * kIconSize;
        }
        Edit e;
        e.fn = EditIconMap;
        FileFor(kIconMap)->edits.push_back(e);
    } else if (!g_icons.empty()) {
        Log("content: this game's icon sheet isn't the one we know - no item icons");
    }
    patchImport("FreeImage.dll", "FreeImage_LoadU", (void*)H_LoadU, (void**)&o_LoadU);
    patchImport("FreeImage.dll", "FreeImage_GetFileTypeU", (void*)H_GetFileTypeU, (void**)&o_GetFileTypeU);
}

}  // namespace

int Content_Add(const char* gamePath, const char* text, size_t len) {
    std::lock_guard<std::mutex> lk(g_regMx);
    if (g_installed || !text) return 0;
    VFile* f = FileFor(gamePath);
    if (!f) return 0;
    Edit e;
    e.text.assign(text, len);
    f->edits.push_back(std::move(e));
    return 1;
}

int Content_Edit(const char* gamePath, SMContentEditFn fn, void* user) {
    std::lock_guard<std::mutex> lk(g_regMx);
    if (g_installed || !fn) return 0;
    VFile* f = FileFor(gamePath);
    if (!f) return 0;
    Edit e;
    e.fn = fn;
    e.user = user;
    f->edits.push_back(std::move(e));
    return 1;
}

int Content_AddIcon(const char* itemUuid, const unsigned char* bgra) {
    std::lock_guard<std::mutex> lk(g_regMx);
    if (g_installed || !itemUuid || !bgra || strlen(itemUuid) != 36 || g_icons.size() >= (size_t)kIconsPerRow * kRows) return 0;
    Icon ic;
    ic.uuid = itemUuid;
    ic.pixels.assign(bgra, bgra + kIconSize * kIconSize * 4);
    g_icons.push_back(std::move(ic));
    return 1;
}

void Content_Install(const wchar_t* loaderDirW, void (*log)(const char* fmt, ...),
                     int (*patchImport)(const char* dll, const char* func, void* hook, void** original)) {
    std::lock_guard<std::mutex> lk(g_regMx);
    Log = log;
    g_installed = true;                                    // too late to register from here on
    if (g_byGamePath.empty() && g_icons.empty()) return;

    // Verify everything first: no hook is installed unless all of them can be.
    for (auto& t : g_targets) {
        t.addr = ScanUnique(t.sig, t.steal);
        if (!t.addr) {
            Log("content: %s not found - this game version is not supported, mods can't add content", t.name);
            return;
        }
    }

    std::wstring loaderDir = loaderDirW;                                    // <game>\SMLoader
    std::wstring root = loaderDir.substr(0, loaderDir.find_last_of(L"\\/") + 1);
    g_gameRoot = NormalizeDisk(root.c_str());
    std::wstring serveDir = loaderDir + L"\\content";
    CreateDirectoryW(serveDir.c_str(), nullptr);
    g_cacheDir = serveDir + L"\\cache";
    CreateDirectoryW(g_cacheDir.c_str(), nullptr);
    if (HMODULE crt = GetModuleHandleW(L"ucrtbase.dll")) g_gameMalloc = (MallocFn)GetProcAddress(crt, "malloc");

    if (!g_icons.empty()) FileFor(kIconMap);               // the maps must be complete before any hook is live
    for (auto& kv : g_byGamePath) {
        VFile* f = kv.second;
        std::wstring flat = DiskPathFor(f->gamePath);
        f->diskPath = root + flat;
        for (auto& c : flat) if (c == L'\\') c = L'_';
        f->servedPath = serveDir + L"\\" + flat;
        g_byDiskPath[NormalizeDisk(f->diskPath.c_str())] = f;
    }

    InstallImages(root, patchImport);
    patchImport("MSVCP140.dll", "?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z", (void*)H_Fiopen, (void**)&o_Fiopen);
    patchImport("KERNEL32.dll", "CreateFileW", (void*)H_CreateFileW, (void**)&o_CreateFileW);
    patchImport("api-ms-win-crt-stdio-l1-1-0.dll", "_wfopen", (void*)H_wfopen, (void**)&o_wfopen);
    patchImport("api-ms-win-crt-stdio-l1-1-0.dll", "_wfopen_s", (void*)H_wfopen_s, (void**)&o_wfopen_s);
    patchImport("api-ms-win-crt-filesystem-l1-1-0.dll", "_wstat64", (void*)H_wstat64, (void**)&o_wstat64);
    patchImport("KERNEL32.dll", "GetFileAttributesW", (void*)H_GetFileAttributesW, (void**)&o_GetFileAttributesW);
    patchImport("KERNEL32.dll", "GetFileAttributesExW", (void*)H_GetFileAttributesExW, (void**)&o_GetFileAttributesExW);
    if (!o_Fiopen) {
        Log("content: cannot redirect file reads - mods can't add content");
        return;
    }
    bool chained = false;
    for (auto& t : g_targets) {
        bool c = false;
        if (!HookCode(t.addr, t.steal, (const void*)t.hook, (void**)t.original, &c)) {
            Log("content: hooking %s failed", t.name);
            return;
        }
        chained |= c;
    }
    uint8_t* exists = ScanUnique(kFileExistsSig, 10);
    if (!exists || !NearHook(exists, 10, (const void*)H_FileExists, (void**)&o_FileExists))
        Log("content: the file index was not found - new meshes and textures won't load in this game version");
    Log("content: ready (%zu files, %zu icons)%s", g_byGamePath.size(), g_icons.size(),
        chained ? ", sharing the game's hooks with a mod that has its own" : "");
}

uint8_t* Code_ScanUnique(const char* sig, size_t steal) { return ScanUnique(sig, steal); }

bool Code_Hook(uint8_t* at, size_t steal, const void* hook, void** original) {
    bool chained;
    return HookCode(at, steal, hook, original, &chained);
}
