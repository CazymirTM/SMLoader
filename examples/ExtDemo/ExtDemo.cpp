// ExtDemo - SMExtensions from a native (C++) mod.
//
//  * Registers two native functions for Lua:
//      SMX.fn.extdemo.add( 2, 3 )        -> 5        (JSON-based)
//      SMX.fn.extdemo.greet( "Ada" )     -> "Hello, Ada!" (raw lua_CFunction)
//  * When a world loads, injects a little Lua that uses the mod's private
//    folder (SMLoader\data\extdemo\) to count visits, and binds F7 to log the
//    count. The folder token stays in a Lua local, so Workshop scripts never
//    see it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

#include "../../include/smloader.h"

static const SMLoaderAPI* api = nullptr;

// JSON in ("[2,3]"), JSON out ("5")
static int Add(const char* args, size_t len, char* out, size_t size, void*) {
    double a = 0, b = 0;
    std::string s(args, len);
    if (sscanf(s.c_str(), "[%lf,%lf]", &a, &b) != 2) return snprintf(out, size, "add(a, b) needs two numbers"), -1;
    return snprintf(out, size, "%.15g", a + b);
}

// Raw Lua C function: full access to the Lua API
static int Greet(lua_State* L) {
    const char* name = api->lua->lua_tolstring(L, 1, nullptr);
    api->lua->lua_pushfstring(L, "Hello, %s!", name ? name : "stranger");
    return 1;
}

static void OnWorldLoaded(const char*, const char*, size_t, int, int, void*) {
    lua_State* L = api->WorldState();
    if (!L) return;
    std::string src = std::string("local TOKEN = '") + api->ExtToken("extdemo") + "'\n" + R"lua(
local fs = SMX.files( "extdemo", TOKEN )
local cfg = fs.config( { visits = 0, greeting = "Welcome back" } )   -- config.json, merged over defaults
cfg.visits = cfg.visits + 1
fs.saveConfig( cfg )
fs.append( "log.txt", SMX.date() .. " visit #" .. cfg.visits .. "\n" )
local _, err = fs.read( "../outside.txt" )                            -- the sandbox refuses this
SMX.log( "ExtDemo: " .. cfg.greeting .. " - world visit #" .. cfg.visits .. " (SMLoader " .. SMX.loader.version
         .. "); reading ../outside.txt -> " .. tostring( err ) )
SMX.input.bind( "F7", function() SMX.log( "ExtDemo: F7 pressed - visits so far: " .. cfg.visits ) end )
)lua";
    api->QueueLua(L, src.c_str(), "=ExtDemo", "game", nullptr, nullptr);
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* a) {
    if (a->version < 3) { a->Log("ExtDemo needs SMLoader 1.2.0 or newer"); return; }
    api = a;
    api->ExtRegister("extdemo.add", Add, nullptr);
    api->ExtRegisterRaw("extdemo.greet", Greet);
    api->EventOn("world.loaded", OnWorldLoaded, nullptr);
    api->Log("ExtDemo loaded");
}
