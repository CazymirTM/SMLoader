// HelloMod - the smallest useful SMLoader mod. Copy this folder to start your own.
//
// When you load into a Survival world it posts a chat message from inside the
// game's own client callback, and logs Lua states as the game creates them.
//
// Build: .\build.ps1 (every folder under examples\ becomes <FolderName>.dll)
// Install: put HelloMod.dll in <game>\SMLoader\mods\

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstring>

#include "../../include/smloader.h"

static const SMLoaderAPI* api = nullptr;
static std::atomic<lua_State*> g_world{nullptr};   // the state running the Survival world
static std::atomic<bool> g_greeted{false};

// The game creates a Lua state (one for gameplay, several for terrain).
static void OnStateCreated(lua_State* L, void*) {
    api->Log("HelloMod: a Lua state was created: %p", (void*)L);
}

static void OnStateClosed(lua_State* L, void*) {
    if (g_world == L) { g_world = nullptr; g_greeted = false; }
}

// Every script the game compiles passes through here - we use it to spot the
// state that runs the Survival game mode. Return 0: we don't change the script.
static int OnScriptLoad(lua_State* L, const char* chunkname, const char**, size_t*, void*) {
    if (chunkname && strstr(chunkname, "/Scripts/game/SurvivalGame.lua")) g_world = L;
    return 0;
}

static void OnChatSent(int ok, const char* text, void*) {
    if (ok) return;
    // Usually the world was still loading (the callback didn't run within 10 s).
    api->Log("HelloMod: chat message not sent yet (%s) - will retry", text);
    g_greeted = false;
}

// Runs on the game's Lua thread at a safe point, a few times per frame at most.
static void OnGameTick(lua_State* L, void*) {
    if (L != g_world || g_greeted) return;
    g_greeted = true;
    // Most sm.* functions only work inside a game callback, so run the code
    // right before SurvivalGame.client_onUpdate. `...` holds that callback's
    // arguments: self (the game script instance) and dt.
    api->QueueLuaIn(L, "SurvivalGame.client_onUpdate",
                    "local self = ...\n"
                    "sm.gui.chatMessage( '#ff9900[HelloMod]#ffffff Hello from an SMLoader mod!' )",
                    "=HelloMod", OnChatSent, nullptr);
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* a) {
    if (a->version < SMLOADER_API_VERSION) return;   // loader too old for this mod
    api = a;

    static SMModCallbacks cb{};
    cb.OnStateCreated = OnStateCreated;
    cb.OnStateClosed = OnStateClosed;
    cb.OnScriptLoad = OnScriptLoad;
    cb.OnGameTick = OnGameTick;
    api->RegisterCallbacks(&cb);

    api->Log("HelloMod loaded (SMLoader dir: %s)", api->loaderDir);
}
