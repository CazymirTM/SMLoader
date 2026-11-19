// EventDemo - SMEvents from a native (C++) mod.
//
// Logs SDK events (players joining and leaving, deaths, robot kills, level
// ups, world load and close) to SMLoader\loader.log. When this machine
// connects to a world it greets everyone with a networked event; every
// machine with EventDemo logs the greeting.
//
// Lua mods see the same events: SMEvents.on( "eventdemo.hello", fn ).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

#include "../../include/smloader.h"

static const SMLoaderAPI* api = nullptr;

static void OnAny(const char* name, const char* json, size_t len, int from, int remote, void*) {
    api->Log("EventDemo: %s %.*s%s", name, (int)len, json, remote ? " (from the network)" : "");
}

static void OnConnected(const char*, const char*, size_t, int, int, void*) {
    api->EventEmitTo(SMNET_TO_ALL, "eventdemo.hello", "{\"text\":\"hello from EventDemo\"}");
}

static void OnHello(const char*, const char* json, size_t len, int from, int, void*) {
    api->Log("EventDemo: player %d says %.*s", from, (int)len, json);
}

// Lua -> C++ -> Lua: answer "eventdemo.ping" with a local "eventdemo.pong"
// carrying the same data. Try it from Lua:
//   SMEvents.on( "eventdemo.pong", function( d ) print( d.n ) end )
//   SMEvents.emit( "eventdemo.ping", { n = 1 } )
static void OnPing(const char*, const char* json, size_t len, int, int, void*) {
    std::string data(json, len);
    api->EventEmit("eventdemo.pong", data.c_str());
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* a) {
    if (a->version < 3) { a->Log("EventDemo needs SMLoader 1.2.0 or newer"); return; }
    api = a;
    static const char* const kLogged[] = {"player.joined", "player.left", "player.death", "robot.killed",
                                          "data.levelUp", "data.unlock", "world.*"};
    for (const char* e : kLogged) api->EventOn(e, OnAny, nullptr);
    api->EventOn("net.connected", OnConnected, nullptr);
    api->EventOn("eventdemo.hello", OnHello, nullptr);
    api->EventOn("eventdemo.ping", OnPing, nullptr);
    api->Log("EventDemo loaded");
}
