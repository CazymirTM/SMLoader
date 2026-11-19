// DevCommands - an SMLoader mod that turns on Survival's built-in developer
// chat commands (/god, /spawn, /timeofday, /starterkit, /sethp, ...) in worlds
// you host.
//
// How: SurvivalGame.lua only binds those commands when the global
// g_survivalDev is true, which the game normally only sets for internal dev
// builds. While SurvivalGame.lua is being compiled we append a few lines that
// wrap server_onCreate and set the flag first. Nothing on disk is modified.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>

#include "../../include/smloader.h"

static const SMLoaderAPI* api = nullptr;
static thread_local std::string t_patched;   // must outlive the OnScriptLoad call

static const char* kPatch = R"lua(

-- [SMLoader DevCommands] enable developer commands for the host
do
	local sv_onCreate = SurvivalGame.server_onCreate
	SurvivalGame.server_onCreate = function( self, ... )
		g_survivalDev = true
		return sv_onCreate( self, ... )
	end
end
)lua";

static int OnScriptLoad(lua_State*, const char* chunkname, const char** buf, size_t* len, void*) {
    if (!chunkname || !strstr(chunkname, "Scripts/game/SurvivalGame.lua")) return 0;
    t_patched.assign(*buf, *len);
    t_patched += kPatch;
    *buf = t_patched.data();
    *len = t_patched.size();
    api->Log("DevCommands: patched %s", chunkname);
    return 1;
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* a) {
    if (a->version < SMLOADER_API_VERSION) return;
    api = a;
    static SMModCallbacks cb{};
    cb.OnScriptLoad = OnScriptLoad;
    api->RegisterCallbacks(&cb);
    api->Log("DevCommands ready");
}
