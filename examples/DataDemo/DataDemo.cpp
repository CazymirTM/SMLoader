// DataDemo - SMPlayerData from a native (C++) mod.
//
// On the host, every online player earns 10 "datademo" XP per minute played
// and an unlock after 5 minutes in total. Every machine logs its own built-in
// stats and DataDemo progress to SMLoader\loader.log every 30 seconds. All of
// it is saved in the world, so it continues where it left off next time.
//
// The same thing in Lua is in docs/PLAYERDATA.md.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "../../include/smloader.h"

static const SMLoaderAPI* api = nullptr;
static const char* kMod = "datademo";

static double Number(int player, const char* mod, const char* key) {
    char buf[64];
    return api->DataGet(player, mod, key, buf, sizeof buf) > 0 ? atof(buf) : 0.0;
}

static void OnGameTick(lua_State*, void*) {
    static ULONGLONG nextMinute = 0, nextLog = 0;
    ULONGLONG now = GetTickCount64();

    SMNetStatus st;
    if (!api->NetGetStatus(kMod, &st) || !st.connected) { nextMinute = now + 60000; return; }

    if (st.isHost && now >= nextMinute) {
        nextMinute = now + 60000;
        // The host knows every online player: walk the ids it has seen
        // (player ids are small numbers, 1 = the host).
        for (int id = 1; id <= 32; ++id) {
            if (api->DataGet(id, "stats", "playtime", nullptr, 0) < 0) continue;
            api->DataAddXP(id, kMod, 10);
            if (Number(id, "stats", "playtime") >= 300) api->DataUnlock(id, kMod, "five_minutes");
        }
    }

    if (now >= nextLog) {
        nextLog = now + 30000;
        char unlocks[256];
        if (api->DataGet(0, kMod, "@unlocks", unlocks, sizeof unlocks) < 0) snprintf(unlocks, sizeof unlocks, "[]");
        api->Log("DataDemo: playtime %.0f s, walked %.0f m, driven %.0f m, kills %.0f, deaths %.0f | "
                 "DataDemo level %.0f (%.0f XP), unlocks %s",
                 Number(0, "stats", "playtime"), Number(0, "stats", "distanceWalked"),
                 Number(0, "stats", "distanceDriven"), Number(0, "stats", "kills"),
                 Number(0, "stats", "deaths"), Number(0, kMod, "@level"), Number(0, kMod, "@xp"), unlocks);
    }
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* a) {
    if (a->version < 3) { a->Log("DataDemo needs SMLoader 1.2.0 or newer"); return; }
    api = a;
    api->NetRegister(kMod, "1.0");   // gives us SMNetworking status (connected / host)
    static SMModCallbacks cb{};
    cb.OnGameTick = OnGameTick;
    api->RegisterCallbacks(&cb);
    api->Log("DataDemo loaded");
}
