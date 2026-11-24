// SMAudio (SMLoader 1.5.0): mods' own sounds, played by the game's FMOD. See audio.cpp
// and docs/AUDIO.md.
#pragma once
#include "../../include/smloader.h"

// Call once, before the mods load (the game creates its sound system later).
void Audio_Init(const SMLuaAPI* lua, void (*log)(const char* fmt, ...),
                int (*patchImport)(const char* dll, const char* func, void* hook, void** original));

// SMLoaderAPI.AudioAdd: registers an .ogg/.wav/.mp3 file under a name.
int Audio_Add(const char* name, const void* data, size_t len);

// Pushes the natives table for smaudio.lua. Returns 1.
int Audio_PushNatives(lua_State* L);

// The world closed: silence.
void Audio_StopAll();
