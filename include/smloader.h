/*
 * smloader.h - public API for Scrap Mechanic native mods loaded by SMLoader.
 *
 * A mod is a 64-bit DLL placed in  <game>/SMLoader/mods/  that exports:
 *
 *     extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* api);
 *
 * Optionally also:
 *     extern "C" __declspec(dllexport) void SMMod_Shutdown(void);
 *
 * SMMod_Init is called once, outside the loader lock, as soon as the game
 * process has finished starting up (at the main menu, before any Lua state
 * exists). It is safe to create threads, load libraries and install hooks
 * there. Do not touch Lua from SMMod_Init: wait for OnStateCreated/OnGameTick.
 *
 * Lua access: the game runs LuaJIT 2.1 (lua51.dll). api->lua is a table of
 * the *original* (unhooked) lua51.dll exports. Only touch a lua_State from
 * inside a callback the loader gives you (OnStateCreated, OnGameTick,
 * OnScriptLoad, or a job you queued with QueueJob/QueueLua): those run on the
 * thread that owns the state, between game Lua calls.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMLOADER_API_VERSION 2
#define SMLOADER_VERSION     "1.1.0"

typedef struct lua_State lua_State;
typedef double    lua_Number;
typedef ptrdiff_t lua_Integer;
typedef int (*lua_CFunction)(lua_State* L);

#define LUA_REGISTRYINDEX (-10000)
#define LUA_ENVIRONINDEX  (-10001)
#define LUA_GLOBALSINDEX  (-10002)
#define lua_upvalueindex(i) (LUA_GLOBALSINDEX - (i))
#define LUA_MULTRET (-1)

#define LUA_TNONE          (-1)
#define LUA_TNIL           0
#define LUA_TBOOLEAN       1
#define LUA_TLIGHTUSERDATA 2
#define LUA_TNUMBER        3
#define LUA_TSTRING        4
#define LUA_TTABLE         5
#define LUA_TFUNCTION      6
#define LUA_TUSERDATA      7
#define LUA_TTHREAD        8

/* X(return type, name, (params)) - a useful subset of the LuaJIT 2.1 C API. */
#define SMLOADER_LUA_FUNCS(X) \
    X(int,          lua_gettop,        (lua_State* L)) \
    X(void,         lua_settop,        (lua_State* L, int idx)) \
    X(void,         lua_pushvalue,     (lua_State* L, int idx)) \
    X(void,         lua_remove,        (lua_State* L, int idx)) \
    X(void,         lua_insert,        (lua_State* L, int idx)) \
    X(void,         lua_replace,       (lua_State* L, int idx)) \
    X(int,          lua_checkstack,    (lua_State* L, int sz)) \
    X(int,          lua_type,          (lua_State* L, int idx)) \
    X(const char*,  lua_typename,      (lua_State* L, int tp)) \
    X(int,          lua_isnumber,      (lua_State* L, int idx)) \
    X(int,          lua_isstring,      (lua_State* L, int idx)) \
    X(lua_Number,   lua_tonumber,      (lua_State* L, int idx)) \
    X(lua_Integer,  lua_tointeger,     (lua_State* L, int idx)) \
    X(int,          lua_toboolean,     (lua_State* L, int idx)) \
    X(const char*,  lua_tolstring,     (lua_State* L, int idx, size_t* len)) \
    X(size_t,       lua_objlen,        (lua_State* L, int idx)) \
    X(void*,        lua_touserdata,    (lua_State* L, int idx)) \
    X(const void*,  lua_topointer,     (lua_State* L, int idx)) \
    X(void,         lua_pushnil,       (lua_State* L)) \
    X(void,         lua_pushnumber,    (lua_State* L, lua_Number n)) \
    X(void,         lua_pushinteger,   (lua_State* L, lua_Integer n)) \
    X(void,         lua_pushlstring,   (lua_State* L, const char* s, size_t l)) \
    X(void,         lua_pushstring,    (lua_State* L, const char* s)) \
    X(const char*,  lua_pushfstring,   (lua_State* L, const char* fmt, ...)) \
    X(void,         lua_pushcclosure,  (lua_State* L, lua_CFunction fn, int n)) \
    X(void,         lua_pushboolean,   (lua_State* L, int b)) \
    X(void,         lua_pushlightuserdata, (lua_State* L, void* p)) \
    X(void,         lua_gettable,      (lua_State* L, int idx)) \
    X(void,         lua_getfield,      (lua_State* L, int idx, const char* k)) \
    X(void,         lua_rawget,        (lua_State* L, int idx)) \
    X(void,         lua_rawgeti,       (lua_State* L, int idx, int n)) \
    X(void,         lua_createtable,   (lua_State* L, int narr, int nrec)) \
    X(int,          lua_getmetatable,  (lua_State* L, int idx)) \
    X(void,         lua_getfenv,       (lua_State* L, int idx)) \
    X(void,         lua_settable,      (lua_State* L, int idx)) \
    X(void,         lua_setfield,      (lua_State* L, int idx, const char* k)) \
    X(void,         lua_rawset,        (lua_State* L, int idx)) \
    X(void,         lua_rawseti,       (lua_State* L, int idx, int n)) \
    X(int,          lua_setmetatable,  (lua_State* L, int idx)) \
    X(int,          lua_setfenv,       (lua_State* L, int idx)) \
    X(void,         lua_call,          (lua_State* L, int nargs, int nresults)) \
    X(int,          lua_pcall,         (lua_State* L, int nargs, int nresults, int errfunc)) \
    X(int,          lua_next,          (lua_State* L, int idx)) \
    X(int,          lua_rawequal,      (lua_State* L, int idx1, int idx2)) \
    X(int,          luaL_loadbufferx,  (lua_State* L, const char* buf, size_t sz, const char* name, const char* mode)) \
    X(int,          luaL_loadstring,   (lua_State* L, const char* s)) \
    X(int,          luaL_ref,          (lua_State* L, int t)) \
    X(void,         luaL_unref,        (lua_State* L, int t, int ref)) \
    X(void,         luaL_traceback,    (lua_State* L, lua_State* L1, const char* msg, int level)) \
    /* Unhooked: states you create here are private and unknown to the game. */ \
    X(lua_State*,   luaL_newstate,     (void)) \
    X(void,         lua_close,         (lua_State* L))

typedef struct SMLuaAPI {
#define SMLOADER_X_FIELD(ret, name, params) ret (*name) params;
    SMLOADER_LUA_FUNCS(SMLOADER_X_FIELD)
#undef SMLOADER_X_FIELD
} SMLuaAPI;

/* Callbacks a mod can register. Any field may be NULL. */
typedef struct SMModCallbacks {
    /* A Lua state was created by the game (luaL_newstate). The game has not
       populated it yet. */
    void (*OnStateCreated)(lua_State* L, void* user);
    /* The game is about to close this state (lua_close). */
    void (*OnStateClosed)(lua_State* L, void* user);
    /* The game is about to load a chunk. You may replace the source by
       writing new values to *buf / *len (the memory must stay valid until the
       callback returns; the loader copies nothing). Return nonzero to
       indicate you replaced it. */
    int  (*OnScriptLoad)(lua_State* L, const char* chunkname,
                         const char** buf, size_t* len, void* user);
    /* Called on the owning thread at a safe point (right before the game's
       outermost C++ -> Lua call), throttled to at most once per ~8 ms per
       state. The loader restores the stack top afterwards. */
    void (*OnGameTick)(lua_State* L, void* user);
    void* user;
} SMModCallbacks;

typedef void (*SMJobFn)(lua_State* L, void* user);

/* --- SMNetworking (API version 2) ----------------------------------------
   Synchronized multiplayer messaging between machines that run SMLoader 1.1+.
   Lua mods use the global table SMNet (see docs/NETWORKING.md); native mods
   use the functions below. Payloads for native mods are byte strings, usually
   JSON: Lua tables sent to a native mod arrive JSON-encoded. */

/* NetSend targets (any value >= 0 is a player id: host -> that player) */
#define SMNET_TO_SERVER  (-1)  /* this machine -> the host                      */
#define SMNET_TO_ALL     (-2)  /* -> every player with the mod, via the host    */
#define SMNET_TO_OTHERS  (-3)  /* like SMNET_TO_ALL, but not back to the sender */
#define SMNET_BROADCAST  (-4)  /* host only: -> every player with the mod       */

/* NetOn sides */
#define SMNET_SIDE_SERVER 1    /* messages sent to the host (SMNET_TO_SERVER)   */
#define SMNET_SIDE_CLIENT 2    /* messages sent to players                      */
#define SMNET_SIDE_BOTH   3

/* Called on the game thread. fromPlayerId: the sender's player id, or 0 when
   the host's server side sent it (SMNET_BROADCAST / to a player). `data` is
   valid only during the call and is not NUL-terminated (use len). */
typedef void (*SMNetHandler)(const char* modId, const char* msg, const char* data, size_t len,
                             int fromPlayerId, int side, void* user);

typedef struct SMNetStatus {
    int  inWorld;          /* SMNetworking is running in the current world        */
    int  connected;        /* the host answered this machine: messages can flow   */
    int  isHost;           /* this machine hosts the world                        */
    int  ready;            /* every player in the world has this mod              */
    int  playerCount;
    int  missingCount;     /* players known not to have this mod                  */
    int  localPlayerId;
    char missing[256];     /* their names, comma-separated (UTF-8, truncated)     */
} SMNetStatus;

typedef struct SMLoaderAPI {
    uint32_t    version;            /* SMLOADER_API_VERSION */
    const char* loaderDir;          /* absolute path to <game>/SMLoader (UTF-8) */
    const char* gameVersion;        /* reserved, may be NULL */

    void (*Log)(const char* fmt, ...);
    void (*RegisterCallbacks)(const SMModCallbacks* cb);

    /* Lua states the game currently has alive. Returns total count. */
    int  (*GetStates)(lua_State** out, int max);
    /* Run fn(L,user) on L's owning thread at the next safe point. Thread-safe. */
    void (*QueueJob)(lua_State* L, SMJobFn fn, void* user);
    /* Compile and run Lua source on L at the next safe point. Thread-safe.
       envGlobal: which environment the chunk runs in.
         "game"       = the shared environment every game script runs in: a
                        registry table with metatable {__index = unsafe_env}
                        holding all game classes (SurvivalGame, BasePlayer, ...).
                        Exists once the world's scripts are loaded.
         NULL         = the state's real globals (full base/debug/ffi/os/...;
                        note the global `type` is replaced by the game).
         any other    = the global table with that name, e.g. "unsafe_env"
                        (the whitelist template the game env inherits from).
       Errors and the chunk's return values are written to the log on error,
       and passed to `done` if non-NULL (text is valid only during the call). */
    void (*QueueLua)(lua_State* L, const char* source, const char* chunkname,
                     const char* envGlobal,
                     void (*done)(int ok, const char* text, void* user), void* user);

    const SMLuaAPI* lua;            /* original lua51.dll exports */

    /* Like QueueLua (env "game"), but the chunk runs right before the game
       next invokes `callback` - an expression naming a script function in the
       game env, e.g. "SurvivalGame.client_onUpdate" or
       "SurvivalGame.server_onFixedUpdate". It runs inside that callback's
       engine sandbox, so sm.* functions that need a client/server callback
       context work, and it receives the callback's arguments as `...`
       (typically self, dt). Thread-safe.
       Why: the engine rejects most sm.* calls outside callbacks with
       "Sandbox violation: callback while no sandbox is present." */
    void (*QueueLuaIn)(lua_State* L, const char* callback, const char* source,
                       const char* chunkname,
                       void (*done)(int ok, const char* text, void* user), void* user);

    /* Redirect ScrapMechanic.exe's import of dll!func to `hook`. *original
       receives the function to call next: another mod's hook if one was
       installed first (so hooks chain), otherwise the real function.
       Returns nonzero on success. Example: intercept "USER32.dll",
       "SetCursorPos" to stop the game recentring the mouse. */
    int (*PatchImport)(const char* dll, const char* func, void* hook, void** original);

    /* Draw on top of the game: `fn(swapChain, user)` is called on the render
       thread right before every IDXGISwapChain::Present (swapChain is an
       IDXGISwapChain*). The loader owns the single Present hook for the whole
       process and always calls the real dxgi Present afterwards, so several
       overlay mods (and the Steam overlay) can coexist without recursion.
       The hook is installed a few seconds after the game window appears. */
    int (*AddPresentCallback)(void (*fn)(void* swapChain, void* user), void* user);

    /* ---- API version 2 (SMLoader 1.1.0) -----------------------------------
       Check api->version >= 2 before touching these: older loaders don't have
       them. */

    /* Callback hook: every time the game invokes `callback` (an expression in
       the game env, e.g. "SurvivalGame.server_onFixedUpdate"), first call the
       Lua function `handler` (also an expression, e.g. "MyMod.onTick") with
       the same arguments, inside the callback's engine sandbox. Nothing is
       wrapped or replaced, so any number of mods can hook the same callback
       without interfering. Applies to every state, now and later; both
       expressions are re-resolved every 2 s (hot reloads are followed). */
    int  (*AddCallbackHook)(const char* callback, const char* handler);

    /* SMNetworking. NetRegister announces that this machine has `modId`
       (shown to everyone in the roster; SMNetStatus.ready checks it).
       NetOn receives messages for modId/msg ("*" = every message of modId).
       NetSend returns nonzero if queued (it is sent on the next game tick);
       0 if no world is running. All three are thread-safe and may be called
       from SMMod_Init. */
    void (*NetRegister)(const char* modId, const char* version);
    void (*NetOn)(const char* modId, const char* msg, int side, SMNetHandler fn, void* user);
    int  (*NetSend)(const char* modId, const char* msg, int target, const char* data, size_t len);
    /* Fills *out (zeroed if unknown); returns nonzero once the status for
       modId is known (about 1 s after joining a world). */
    int  (*NetGetStatus)(const char* modId, SMNetStatus* out);
} SMLoaderAPI;

typedef void (*SMMod_InitFn)(const SMLoaderAPI* api);
typedef void (*SMMod_ShutdownFn)(void);

#ifdef __cplusplus
}
#endif
