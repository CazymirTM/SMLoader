// SMModsMenu (SMLoader 1.6.0): the MODS button in the pause menu, the settings
// store behind it, and the native API. The page itself is loader/sdk/smmenu.lua.
// See docs/MODSMENU.md.
#pragma once
#include "../../include/smloader.h"

struct MenuHost {
    const SMLuaAPI* lua;
    const wchar_t*  loaderDir;                         // <game>\SMLoader
    void (*log)(const char* fmt, ...);
    // Queue a function of the SMModsMenu table (page, option) on the game
    // thread; false if no world is loaded.
    bool (*callLua)(const char* fn, const char* page, const char* option);
};

// Before the mods are loaded.
void Menu_Init(const MenuHost& host);
// After the mods: adds the button if this game version is recognised.
void Menu_InstallButton();
// Pushes the natives table smmenu.lua gets as its second argument. Returns 1.
int  Menu_PushNatives(lua_State* L);
// The world closed: values set by its host no longer apply.
void Menu_WorldClosed();
// For the loader's own page: an on/off value that lives in loader.ini instead
// of config\. Call before adding the option.
void Menu_SeedExternal(const char* page, const char* option, bool on);
// Write anything unsaved (game exit).
void Menu_Flush();

// API entry points (SMLoaderAPI.Menu*).
int    Menu_AddPage(const SMMenuPage* page);
int    Menu_AddOption(const char* page, const SMMenuOption* option);
double Menu_GetNumber(const char* page, const char* option, double fallback);
int    Menu_GetText(const char* page, const char* option, char* out, size_t size);
int    Menu_SetNumber(const char* page, const char* option, double value);
int    Menu_SetText(const char* page, const char* option, const char* text);
int    Menu_Open(const char* page);
