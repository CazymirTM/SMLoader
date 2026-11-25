// MenuDemo - every kind of option in the pause menu's MODS page (SMModsMenu).
//
// Adds two pages: one from C++ (api->MenuAddPage / MenuAddOption) and one from
// Lua (SMModsMenu.page). Open a world, press Esc, click MODS.
#include <cstdio>
#include <cstring>

#include "../../include/smloader.h"

static const SMLoaderAPI* g_api;

// --- a page from C++ -----------------------------------------------------------
static void OnMenu(const SMMenuEvent* e, void*) {
    const char* what = e->kind == SMMENU_CLICKED ? "clicked" : e->kind == SMMENU_PRESSED ? "key pressed" : "changed";
    g_api->Log("MenuDemo: %s %s (number %g, text '%s')", e->option, what, e->number, e->text);
}

static void Add(SMMenuOption o) {
    o.size = sizeof o;
    o.fn = OnMenu;
    if (!g_api->MenuAddOption("menudemo", &o)) g_api->Log("MenuDemo: option %s was refused", o.id ? o.id : "?");
}

static void AddNativePage() {
    SMMenuPage page{};
    page.size = sizeof page;
    page.id = "menudemo";
    page.name = "Menu Demo (C++)";
    page.version = "1.0";
    page.author = "SMLoader";
    page.description = "Every kind of option a native mod can have. What you change here is written to loader.log.";
    g_api->MenuAddPage(&page);

    SMMenuOption o{};
    o.type = SMMENU_SECTION; o.label = "BASICS"; Add(o);

    o = {}; o.type = SMMENU_TOGGLE; o.id = "enabled"; o.label = "Enabled"; o.description = "The options below need this one."; o.defNumber = 1; Add(o);
    o = {}; o.type = SMMENU_SLIDER; o.id = "speed"; o.label = "Speed"; o.enabledBy = "enabled"; o.min = 0.5; o.max = 5; o.step = 0.1; o.defNumber = 1; o.format = "%.1fx"; Add(o);
    static const char* modes[] = {"Easy", "Normal", "Hard", "Nightmare", nullptr};
    o = {}; o.type = SMMENU_CHOICE; o.id = "mode"; o.label = "Difficulty"; o.enabledBy = "enabled"; o.choices = modes; o.defNumber = 1; Add(o);
    o = {}; o.type = SMMENU_TEXT; o.id = "title"; o.label = "Title"; o.description = "Any text."; o.defText = "Mechanic"; o.maxLength = 24; Add(o);

    o = {}; o.type = SMMENU_SECTION; o.label = "MORE"; Add(o);
    o = {}; o.type = SMMENU_KEY; o.id = "key"; o.label = "Action key"; o.description = "Press it while playing."; o.defText = "F7"; Add(o);
    o = {}; o.type = SMMENU_COLOR; o.id = "color"; o.label = "Colour"; o.defText = "#FF9500"; Add(o);
    o = {}; o.type = SMMENU_TOGGLE; o.id = "pvp"; o.label = "Friendly fire"; o.flags = SMMENU_HOST; Add(o);
    o = {}; o.type = SMMENU_TOGGLE; o.id = "preload"; o.label = "Preload everything"; o.flags = SMMENU_RESTART; Add(o);
    o = {}; o.type = SMMENU_BUTTON; o.id = "wipe"; o.label = "Forget everything"; o.caption = "FORGET"; o.flags = SMMENU_CONFIRM; Add(o);
    o = {}; o.type = SMMENU_INFO; o.label = "A paragraph of text, for example to explain how the options above work together. It wraps when it is longer than one line."; Add(o);

    char title[64];
    g_api->MenuGetText("menudemo", "title", title, sizeof title);
    g_api->Log("MenuDemo: saved values: enabled %g, speed %g, mode %g, title '%s'", g_api->MenuGetNumber("menudemo", "enabled", -1),
               g_api->MenuGetNumber("menudemo", "speed", -1), g_api->MenuGetNumber("menudemo", "mode", -1), title);
}

// --- a page from Lua -------------------------------------------------------------
static const char kLua[] = R"LUA(
local page = SMModsMenu.page{ id = "menudemo.lua", name = "Menu Demo (Lua)", version = "1.0", author = "SMLoader",
	description = "The same from Lua. Shows a message for what you change." }
page:section( "PLAYER" )
page:toggle{ id = "greet", label = "Greet me", desc = "Say hello in the chat when this is turned on.", default = false,
	onChange = function( on ) if on then sm.gui.chatMessage( "Hello from the MODS page!" ) end end }
page:slider{ id = "volume", label = "Volume", min = 0, max = 100, step = 5, default = 50, unit = "%" }
page:choice{ id = "style", label = "Style", choices = { "Plain", "Fancy" }, visible = function( p ) return p:get( "greet" ) end }
page:choice{ id = "team", label = "Team", choices = { { value = 1, label = "Red" }, { value = 2, label = "Blue" }, { value = 3, label = "Green" } }, default = 2 }
page:key{ id = "wave", label = "Wave key", default = "F8", onPress = function( p ) sm.gui.displayAlertText( "Volume: " .. p:get( "volume" ), 2 ) end }
page:color{ id = "tint", label = "Tint", default = "#0A84FF" }
page:text{ id = "nick", label = "Nickname", default = "", max = 20 }
page:button{ id = "count", label = function() return "Clicked " .. ( MenuDemoClicks or 0 ) .. " times" end, caption = "CLICK ME",
	onClick = function( p ) MenuDemoClicks = ( MenuDemoClicks or 0 ) + 1 p:refresh() end }
page:info( "Values are saved in SMLoader\\config\\menudemo.lua.cfg." )
page:onChange( function( id, value ) SMX.log( "MenuDemo (Lua):", id, "=", value ) end )
)LUA";

static void OnInstalled(int ok, const char* text, void*) {
    if (!ok) g_api->Log("MenuDemo: %s", text);
}

// Arrives once a world's scripts and the SDK are running.
static void OnWorld(const char*, const char*, size_t, int, int, void*) {
    if (lua_State* L = g_api->WorldState()) g_api->QueueLua(L, kLua, "=MenuDemo", "game", OnInstalled, nullptr);
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* api) {
    g_api = api;
    if (api->version < 5) { api->Log("MenuDemo needs SMLoader 1.6 or newer"); return; }
    AddNativePage();
    api->EventOn("world.loaded", OnWorld, nullptr);
}
