// SMAudio: plays mods' sounds through the game's own FMOD system, in one of its
// mixer buses (so the volume sliders, the listener and the output device are the
// game's).
//
// The game (1.0.7a) uses FMOD Studio 2.02: it imports the C++ API by name, so its
// Studio::System is caught in System::update (its per-frame call). Everything else
// goes through FMOD's exported C API, resolved at run time. Only FMOD calls that
// FMOD documents as thread-safe are made; our own state is behind one mutex.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "audio.h"

namespace {

void (*Log)(const char* fmt, ...) = nullptr;
const SMLuaAPI* lua = nullptr;

struct Vec { float x, y, z; };
struct Attributes { Vec position, velocity, forward, up; };

// FMOD_CREATESOUNDEXINFO (2.02, x64); only the first two fields are used
struct ExInfo { int cbsize; unsigned length; unsigned char rest[216]; };
static_assert(sizeof(ExInfo) == 224, "FMOD_CREATESOUNDEXINFO");

enum : unsigned {
    MODE_LOOP_OFF = 0x1, MODE_LOOP_NORMAL = 0x2, MODE_2D = 0x8, MODE_3D = 0x10, MODE_CREATESAMPLE = 0x100, MODE_OPENMEMORY = 0x800,
    MODE_WORLDRELATIVE = 0x80000, MODE_INVERSETAPERED = 0x800000,
};

// --- FMOD C API -----------------------------------------------------------------
#define CORE(X) \
    X(int, FMOD_System_CreateSound, (void*, const char*, unsigned, ExInfo*, void**)) \
    X(int, FMOD_System_PlaySound, (void*, void*, void*, int, void**)) \
    X(int, FMOD_Channel_SetMode, (void*, unsigned)) \
    X(int, FMOD_Channel_SetPaused, (void*, int)) \
    X(int, FMOD_Channel_SetVolume, (void*, float)) \
    X(int, FMOD_Channel_SetPitch, (void*, float)) \
    X(int, FMOD_Channel_Set3DAttributes, (void*, const Vec*, const Vec*)) \
    X(int, FMOD_Channel_Set3DMinMaxDistance, (void*, float, float)) \
    X(int, FMOD_Channel_Set3DDopplerLevel, (void*, float)) \
    X(int, FMOD_Channel_Stop, (void*)) \
    X(int, FMOD_Channel_IsPlaying, (void*, int*))
#define STUDIO(X) \
    X(int, FMOD_Studio_System_GetCoreSystem, (void*, void**)) \
    X(int, FMOD_Studio_System_GetBus, (void*, const char*, void**)) \
    X(int, FMOD_Studio_Bus_LockChannelGroup, (void*)) \
    X(int, FMOD_Studio_Bus_GetChannelGroup, (void*, void**))
#define DECLARE(ret, name, args) ret (*name) args = nullptr;
CORE(DECLARE)
STUDIO(DECLARE)
#undef DECLARE

bool ResolveFmod() {
    static int state = 0;
    if (state) return state > 0;
    HMODULE core = GetModuleHandleW(L"fmod.dll"), studio = GetModuleHandleW(L"fmodstudio.dll");
    bool ok = core && studio;
#define GET_CORE(ret, name, args) ok = ok && (*(void**)&name = (void*)GetProcAddress(core, #name)) != nullptr;
#define GET_STUDIO(ret, name, args) ok = ok && (*(void**)&name = (void*)GetProcAddress(studio, #name)) != nullptr;
    CORE(GET_CORE)
    STUDIO(GET_STUDIO)
    state = ok ? 1 : -1;
    if (!ok) Log("audio: this game's FMOD isn't the one we know - mod sounds are off");
    return ok;
}

// --- state ------------------------------------------------------------------------
struct Sound { std::string data; void* fmod = nullptr; bool failed = false; };
struct Voice {
    void*    channel = nullptr;
    float    volume = 1, gain = 1, fadePerMs = 0;   // gain: the fade-out
    bool     is3d = false, waiting = false;
    uint64_t startAt = 0, lastTick = 0;
};
std::mutex                    g_mx;
void*                         g_studio = nullptr;
void*                         g_core = nullptr;
Attributes                    g_listener{};
std::map<std::string, Sound>  g_sounds;
std::map<std::string, void*>  g_buses;         // path -> Bus*
std::map<int, Voice>          g_voices;
int                           g_nextVoice = 1;
const size_t                  kMaxVoices = 96;

// The bus's channel group, or null (= the master group) while FMOD is creating it.
void* GroupFor(const std::string& path) {
    auto it = g_buses.find(path);
    if (it == g_buses.end()) {
        void* bus = nullptr;
        if (FMOD_Studio_System_GetBus(g_studio, path.c_str(), &bus) != 0) bus = nullptr;
        if (bus) FMOD_Studio_Bus_LockChannelGroup(bus);
        else Log("audio: the game has no mixer bus '%s'", path.c_str());
        it = g_buses.emplace(path, bus).first;
    }
    void* group = nullptr;
    if (!it->second || FMOD_Studio_Bus_GetChannelGroup(it->second, &group) != 0) return nullptr;
    return group;
}

void Tick() {                                    // g_mx held, once per frame
    if (!g_core) {
        if (!ResolveFmod() || FMOD_Studio_System_GetCoreSystem(g_studio, &g_core) != 0) { g_core = nullptr; return; }
        Log("audio: ready (%zu sounds)", g_sounds.size());
    }
    static bool warmed = false;
    if (!warmed) warmed = GroupFor("bus:/Effect") != nullptr || g_buses["bus:/Effect"] == nullptr;
    uint64_t now = GetTickCount64();
    for (auto it = g_voices.begin(); it != g_voices.end();) {
        Voice& v = it->second;
        int playing = 0;
        bool dead = FMOD_Channel_IsPlaying(v.channel, &playing) != 0 || !playing;
        if (!dead && v.waiting && now >= v.startAt) {
            v.waiting = false;
            FMOD_Channel_SetPaused(v.channel, 0);
        }
        if (!dead && v.fadePerMs > 0) {
            v.gain -= v.fadePerMs * (float)(now - v.lastTick);
            if (v.gain <= 0) { FMOD_Channel_Stop(v.channel); dead = true; }
            else FMOD_Channel_SetVolume(v.channel, v.volume * v.gain);
        }
        v.lastTick = now;
        it = dead ? g_voices.erase(it) : std::next(it);
    }
}

// --- the game's calls ----------------------------------------------------------------
typedef int (*UpdateFn)(void*);
typedef int (*ListenerFn)(void*, int, const Attributes*, const Vec*);
typedef int (*ReleaseFn)(void*);
UpdateFn   o_update = nullptr;
ListenerFn o_listener = nullptr;
ReleaseFn  o_release = nullptr;

int H_update(void* system) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_studio != system) { g_studio = system; g_core = nullptr; }
        if (!g_sounds.empty()) Tick();
    }
    return o_update(system);
}
int H_listener(void* system, int index, const Attributes* a, const Vec* attenuation) {
    if (index == 0 && a) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_listener = *a;
    }
    return o_listener(system, index, a, attenuation);
}
int H_release(void* system) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_voices.clear();
        g_buses.clear();
        g_studio = g_core = nullptr;
        for (auto& s : g_sounds) s.second.fmod = nullptr;
    }
    return o_release(system);
}

// The game's FMOD space is (x, z, y): up is y there.
Vec ToFmod(float x, float y, float z) { return {x, z, y}; }

// --- Lua -------------------------------------------------------------------------------
float Num(lua_State* L, int i, float def) {
    if (!lua->lua_isnumber(L, i)) return def;
    float v = (float)lua->lua_tonumber(L, i);
    return std::isfinite(v) ? v : def;
}

// play(name, is3d, x,y,z, vx,vy,vz, volume, pitch, loop, min, max, bus, delayMs, doppler) -> id or nil
int L_play(lua_State* L) {
    size_t n = 0;
    const char* name = lua->lua_tolstring(L, 1, &n);
    if (!name) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_sounds.find(std::string(name, n));
    if (it == g_sounds.end() || !g_core || g_voices.size() >= kMaxVoices) return 0;
    Sound& s = it->second;
    if (!s.fmod && !s.failed) {
        ExInfo ex;
        memset(&ex, 0, sizeof ex);
        ex.cbsize = sizeof ex;
        ex.length = (unsigned)s.data.size();
        int r = FMOD_System_CreateSound(g_core, s.data.data(), MODE_OPENMEMORY | MODE_CREATESAMPLE | MODE_3D | MODE_WORLDRELATIVE | MODE_INVERSETAPERED | MODE_LOOP_OFF, &ex, &s.fmod);
        if (r != 0) {
            s.failed = true;
            s.fmod = nullptr;
            Log("audio: FMOD can't read the sound '%s' (error %d)", it->first.c_str(), r);
        }
    }
    if (!s.fmod) return 0;
    Voice v;
    v.is3d = lua->lua_toboolean(L, 2) != 0;
    Vec pos = ToFmod(Num(L, 3, 0), Num(L, 4, 0), Num(L, 5, 0)), vel = ToFmod(Num(L, 6, 0), Num(L, 7, 0), Num(L, 8, 0));
    v.volume = std::clamp(Num(L, 9, 1), 0.0f, 4.0f);
    float pitch = std::clamp(Num(L, 10, 1), 0.05f, 8.0f);
    bool loop = lua->lua_toboolean(L, 11) != 0;
    float minD = std::max(0.1f, Num(L, 12, 4)), maxD = std::max(minD + 0.1f, Num(L, 13, 200));
    const char* bus = lua->lua_tolstring(L, 14, &n);
    float delay = std::max(0.0f, Num(L, 15, 0));
    if (FMOD_System_PlaySound(g_core, s.fmod, GroupFor(bus ? std::string(bus, n) : "bus:/Effect"), 1, &v.channel) != 0) return 0;
    FMOD_Channel_SetMode(v.channel, (loop ? MODE_LOOP_NORMAL : MODE_LOOP_OFF) |
                                        (v.is3d ? MODE_3D | MODE_WORLDRELATIVE | MODE_INVERSETAPERED : MODE_2D));
    FMOD_Channel_SetVolume(v.channel, v.volume);
    FMOD_Channel_SetPitch(v.channel, pitch);
    if (v.is3d) {
        FMOD_Channel_Set3DMinMaxDistance(v.channel, minD, maxD);
        FMOD_Channel_Set3DAttributes(v.channel, &pos, &vel);
        FMOD_Channel_Set3DDopplerLevel(v.channel, std::clamp(Num(L, 16, 1), 0.0f, 5.0f));
    }
    v.lastTick = GetTickCount64();
    if (delay > 0) {
        v.waiting = true;
        v.startAt = v.lastTick + (uint64_t)delay;
    } else {
        FMOD_Channel_SetPaused(v.channel, 0);
    }
    int id = g_nextVoice++;
    g_voices[id] = v;
    lua->lua_pushinteger(L, id);
    return 1;
}

Voice* Find(lua_State* L) {
    auto it = g_voices.find((int)lua->lua_tointeger(L, 1));
    return it == g_voices.end() ? nullptr : &it->second;
}

// set(id, x,y,z, vx,vy,vz, volume, pitch); nil = leave. Returns true while it plays.
int L_set(lua_State* L) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = Find(L);
    if (!v) return 0;
    if (v->is3d && lua->lua_isnumber(L, 2)) {
        Vec pos = ToFmod(Num(L, 2, 0), Num(L, 3, 0), Num(L, 4, 0)), vel = ToFmod(Num(L, 5, 0), Num(L, 6, 0), Num(L, 7, 0));
        FMOD_Channel_Set3DAttributes(v->channel, &pos, &vel);
    }
    if (lua->lua_isnumber(L, 8)) {
        v->volume = std::clamp(Num(L, 8, 1), 0.0f, 4.0f);
        FMOD_Channel_SetVolume(v->channel, v->volume * v->gain);
    }
    if (lua->lua_isnumber(L, 9)) FMOD_Channel_SetPitch(v->channel, std::clamp(Num(L, 9, 1), 0.05f, 8.0f));
    lua->lua_pushboolean(L, 1);
    return 1;
}

// stop(id, fadeMs)
int L_stop(lua_State* L) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = Find(L);
    if (!v) return 0;
    float fade = Num(L, 2, 0);
    if (fade > 1 && !v->waiting) {
        v->fadePerMs = v->gain / fade;
        v->lastTick = GetTickCount64();
    } else {
        FMOD_Channel_Stop(v->channel);
        g_voices.erase((int)lua->lua_tointeger(L, 1));
    }
    return 0;
}

int L_playing(lua_State* L) {
    std::lock_guard<std::mutex> lk(g_mx);
    lua->lua_pushboolean(L, Find(L) != nullptr);
    return 1;
}

int L_exists(lua_State* L) {
    size_t n = 0;
    const char* name = lua->lua_tolstring(L, 1, &n);
    std::lock_guard<std::mutex> lk(g_mx);
    lua->lua_pushboolean(L, name && g_sounds.count(std::string(name, n)));
    return 1;
}

// listener() -> x,y,z, forward x,y,z
int L_listener(lua_State* L) {
    Attributes a;
    { std::lock_guard<std::mutex> lk(g_mx); a = g_listener; }
    for (float f : {a.position.x, a.position.z, a.position.y, a.forward.x, a.forward.z, a.forward.y}) lua->lua_pushnumber(L, f);
    return 6;
}

}  // namespace

void Audio_Init(const SMLuaAPI* api, void (*log)(const char* fmt, ...), int (*patchImport)(const char*, const char*, void*, void**)) {
    lua = api;
    Log = log;
    patchImport("fmodstudio.dll", "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)H_update, (void**)&o_update);
    patchImport("fmodstudio.dll", "?setListenerAttributes@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_3D_ATTRIBUTES@@PEBUFMOD_VECTOR@@@Z",
                (void*)H_listener, (void**)&o_listener);
    patchImport("fmodstudio.dll", "?release@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)H_release, (void**)&o_release);
}

int Audio_Add(const char* name, const void* data, size_t len) {
    if (!name || !*name || !data || len < 16 || len > 64u * 1024 * 1024) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    Sound& s = g_sounds[name];
    if (s.fmod) return 0;                         // already in use
    s.data.assign((const char*)data, len);
    s.failed = false;
    return 1;
}

int Audio_PushNatives(lua_State* L) {
    lua->lua_createtable(L, 0, 6);
    struct { const char* name; lua_CFunction fn; } fns[] = {
        {"play", L_play}, {"set", L_set}, {"stop", L_stop}, {"playing", L_playing}, {"exists", L_exists}, {"listener", L_listener}};
    for (auto& f : fns) { lua->lua_pushcclosure(L, f.fn, 0); lua->lua_setfield(L, -2, f.name); }
    return 1;
}

void Audio_StopAll() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& v : g_voices) FMOD_Channel_Stop(v.second.channel);
    g_voices.clear();
}
