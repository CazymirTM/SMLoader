// NetDemo - SMNetworking from a native (C++) mod.
//
// Every player with NetDemo says hello to the host when they join a world. The
// host answers that player privately and tells everyone who joined. Everything
// is written to SMLoader\loader.log, so it also works in single player (the
// host is a client of its own world, so messages loop back).
//
// The same conversation in Lua is in docs/NETWORKING.md.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "../../include/smloader.h"

static const SMLoaderAPI* api = nullptr;
static const char* kMod = "netdemo";

// Host: a player said hello -> reply to them, then tell everyone.
static void OnHello(const char*, const char*, const char* data, size_t len, int from, int, void*) {
    api->Log("NetDemo [host]: player %d says hello: %.*s", from, (int)len, data);
    char reply[128];
    int n = snprintf(reply, sizeof reply, "{\"text\":\"Welcome! You are player %d.\"}", from);
    api->NetSend(kMod, "welcome", from, reply, (size_t)n);
    n = snprintf(reply, sizeof reply, "{\"player\":%d}", from);
    api->NetSend(kMod, "joined", SMNET_BROADCAST, reply, (size_t)n);
}

// Every player: messages from the host.
static void OnFromHost(const char*, const char* msg, const char* data, size_t len, int from, int, void*) {
    api->Log("NetDemo [client]: %s from %s: %.*s", msg, from ? "a player" : "the host", (int)len, data);
}

// Say hello once per world, as soon as the host has answered SMNetworking's
// own handshake. OnGameTick runs often; checking once a second is plenty.
static void OnGameTick(lua_State*, void*) {
    static ULONGLONG nextCheck = 0;
    static std::atomic<bool> saidHello{false};
    static int lastReady = -1;
    ULONGLONG now = GetTickCount64();
    if (now < nextCheck) return;
    nextCheck = now + 1000;

    SMNetStatus st;
    if (!api->NetGetStatus(kMod, &st) || !st.connected) {
        if (!st.inWorld) { saidHello = false; lastReady = -1; }   // left the world
        return;
    }
    if (!saidHello.exchange(true)) {
        const char hello[] = "{\"text\":\"hi from NetDemo\"}";
        api->NetSend(kMod, "hello", SMNET_TO_SERVER, hello, sizeof hello - 1);
    }
    if (st.ready != lastReady) {
        lastReady = st.ready;
        if (st.ready) api->Log("NetDemo: all %d player(s) have NetDemo", st.playerCount);
        else api->Log("NetDemo: %d player(s) don't have NetDemo: %s", st.missingCount, st.missing);
    }
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* a) {
    if (a->version < 2) { a->Log("NetDemo needs SMLoader 1.1.0 or newer"); return; }
    api = a;
    api->NetRegister(kMod, "1.0");
    api->NetOn(kMod, "hello", SMNET_SIDE_SERVER, OnHello, nullptr);
    api->NetOn(kMod, "*", SMNET_SIDE_CLIENT, OnFromHost, nullptr);

    static SMModCallbacks cb{};
    cb.OnGameTick = OnGameTick;
    api->RegisterCallbacks(&cb);
    api->Log("NetDemo loaded");
}
