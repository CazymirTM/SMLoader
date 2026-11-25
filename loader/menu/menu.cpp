// SMModsMenu, native half:
//   * the settings store: SMLoader\config\<page>.cfg
//   * pages and options registered by native mods, and their callbacks
//   * the MODS button: one more button in the game's own pause menu, wired to
//     the menu exactly like the game wires its own
//
// How the pause menu works (reverse-engineered on 1.0.7a.903): the menu object
// (InGameMainMenu2) loads $GAME_DATA/Gui/Layouts/InGameMenu/InGameMenu.layout,
// finds each button by name and adds two delegates to it:
//     add( &button->eventClick,    makeClickDelegate( this, &Menu::onResume ) )
//     add( &button->eventSetFocus, makeHoverDelegate( this ) )
// After it has done that we find our button and do the same, with a handler of
// our own that closes the menu the way RESUME does and asks the Lua side to
// show the page.
//
// The button gets into the layout when the text reaches the GUI system: the
// game's text loader returns a Buffer { data, size, owned }, after its own file
// checks. (An SMContent edit would make the game count its core files as
// modified, which a loader without mods must not do.)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../content/content.h"
#include "menu.h"

namespace {

MenuHost g_host{};
#define LUA (g_host.lua)

const size_t kMaxText = 4096, kMaxKeys = 1024, kMaxPages = 256;

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Page and option ids: letters, digits, _ - . (a page id is also a file name).
bool ValidId(const char* s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 64 || s[0] == '.' || s[n - 1] == '.') return false;
    for (size_t i = 0; i < n; ++i)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '-' || s[i] == '.')) return false;
    std::string stem(s, strcspn(s, "."));              // Windows device names can't be files
    for (auto& c : stem) c = (char)tolower((unsigned char)c);
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    return !(stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && isdigit((unsigned char)stem[3]));
}

// --- values: JSON scalars ------------------------------------------------------
struct Value {
    enum Kind { None, Bool, Number, Text } kind = None;
    double      n = 0;
    std::string s;
};

std::string Encode(const Value& v) {
    if (v.kind == Value::Bool) return v.n != 0 ? "true" : "false";
    if (v.kind == Value::Number) {
        char b[40];
        snprintf(b, sizeof b, "%.14g", std::isfinite(v.n) ? v.n : 0.0);
        return b;
    }
    if (v.kind == Value::Text) {
        std::string out = "\"";
        for (unsigned char c : v.s) {
            if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
        return out + "\"";
    }
    return "null";
}

void AppendUtf8(std::string& out, unsigned c) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | (c >> 6 & 63)); out += (char)(0x80 | (c & 63)); }
    else { out += (char)(0xF0 | c >> 18); out += (char)(0x80 | (c >> 12 & 63)); out += (char)(0x80 | (c >> 6 & 63)); out += (char)(0x80 | (c & 63)); }
}

Value Decode(const std::string& j) {
    Value v;
    if (j == "true" || j == "false") { v.kind = Value::Bool; v.n = j == "true"; return v; }
    if (j.size() >= 2 && j.front() == '"' && j.back() == '"') {
        v.kind = Value::Text;
        for (size_t i = 1; i + 1 < j.size(); ++i) {
            if (j[i] != '\\' || i + 2 >= j.size()) { v.s += j[i]; continue; }
            char e = j[++i];
            if (e == 'n') v.s += '\n';
            else if (e == 'r') v.s += '\r';
            else if (e == 't') v.s += '\t';
            else if (e == 'b') v.s += '\b';
            else if (e == 'f') v.s += '\f';
            else if (e == 'u' && i + 4 < j.size()) {
                unsigned c = (unsigned)strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (c >= 0xD800 && c < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                    unsigned lo = (unsigned)strtoul(j.substr(i + 3, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                AppendUtf8(v.s, c);
            } else v.s += e;
        }
        return v;
    }
    char* end = nullptr;
    double d = strtod(j.c_str(), &end);
    if (!j.empty() && end && *end == 0 && std::isfinite(d)) { v.kind = Value::Number; v.n = d; }
    return v;
}

// --- settings store -------------------------------------------------------------
std::recursive_mutex g_mx;                                          // guards everything below
typedef std::map<std::string, std::string> Keys;                    // option -> JSON
std::map<std::string, Keys> g_saved;                                // page -> values
std::set<std::string> g_loaded, g_dirty, g_external;
std::map<std::string, std::string> g_override;                      // "page/option" -> the host's value (JSON)
std::map<std::string, std::string> g_session;                       // small strings kept until the game exits

std::wstring ConfigPath(const std::string& page) { return std::wstring(g_host.loaderDir) + L"\\config\\" + Widen(page) + L".cfg"; }

Keys& Store(const std::string& page) {
    Keys& keys = g_saved[page];
    if (g_external.count(page) || !g_loaded.insert(page).second) return keys;
    FILE* f = _wfopen(ConfigPath(page).c_str(), L"rb");
    if (!f) return keys;
    std::string text;
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1u << 20)) text.append(buf, got);
    fclose(f);
    auto trim = [](const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    for (size_t at = 0; at < text.size();) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (ValidId(k.c_str()) && Decode(v).kind != Value::None && keys.size() < kMaxKeys) keys[k] = v;
    }
    return keys;
}

void Save(const std::string& page) {
    if (!g_dirty.erase(page) || g_external.count(page)) return;
    CreateDirectoryW((std::wstring(g_host.loaderDir) + L"\\config").c_str(), nullptr);
    std::wstring path = ConfigPath(page), tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { g_host.log("SMModsMenu: can't write the settings of %s", page.c_str()); return; }
    std::string text = "# Settings of " + page + " (pause menu > MODS). Edit only while the game is closed.\r\n";
    for (auto& kv : g_saved[page]) text += kv.first + " = " + kv.second + "\r\n";
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        g_host.log("SMModsMenu: can't write the settings of %s", page.c_str());
        DeleteFileW(tmp.c_str());
    }
}

void SaveAll() {
    std::set<std::string> pages = g_dirty;
    for (auto& p : pages) Save(p);
}

// --- pages and options of native mods --------------------------------------------
struct Option {
    int type = 0;
    std::string id, label, desc, enabledBy, format, caption, defText;
    unsigned flags = 0;
    double def = 0, min = 0, max = 0, step = 0;
    int maxLength = 0;
    std::vector<std::string> choices;
    SMMenuFn fn = nullptr;
    void* user = nullptr;
};
struct Page {
    std::string id, name, version, author, desc;
    std::vector<Option> options;
};
std::vector<Page> g_pages;

Page* FindPage(const char* id) {
    if (id) for (auto& p : g_pages) if (p.id == id) return &p;
    return nullptr;
}
Option* FindOption(const char* page, const char* option) {
    Page* p = FindPage(page);
    if (p && option && *option) for (auto& o : p->options) if (o.id == option) return &o;
    return nullptr;
}

// The key names SMX.input uses.
int KeyCode(const std::string& name) {
    static const std::map<std::string, int> keys = [] {
        std::map<std::string, int> m = {
            {"BACKSPACE", 0x08}, {"TAB", 0x09}, {"ENTER", 0x0D}, {"SHIFT", 0x10}, {"CTRL", 0x11}, {"ALT", 0x12}, {"PAUSE", 0x13},
            {"CAPSLOCK", 0x14}, {"ESC", 0x1B}, {"SPACE", 0x20}, {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"END", 0x23}, {"HOME", 0x24},
            {"LEFT", 0x25}, {"UP", 0x26}, {"RIGHT", 0x27}, {"DOWN", 0x28}, {"INSERT", 0x2D}, {"DELETE", 0x2E}, {"MOUSE1", 0x01},
            {"MOUSE2", 0x02}, {"MOUSE3", 0x04}, {"MOUSE4", 0x05}, {"MOUSE5", 0x06}, {"MULTIPLY", 0x6A}, {"ADD", 0x6B},
            {"SUBTRACT", 0x6D}, {"DECIMAL", 0x6E}, {"DIVIDE", 0x6F}};
        for (int i = 0; i < 10; ++i) { m[std::string(1, (char)('0' + i))] = 0x30 + i; m["NUMPAD" + std::to_string(i)] = 0x60 + i; }
        for (int i = 0; i < 26; ++i) m[std::string(1, (char)('A' + i))] = 0x41 + i;
        for (int i = 1; i <= 12; ++i) m["F" + std::to_string(i)] = 0x6F + i;
        return m;
    }();
    auto it = keys.find(name);
    return it == keys.end() ? 0 : it->second;
}
std::string Upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

// The value as this option type stores it; None if it can't be used.
Value Fit(const Option& o, const Value& in) {
    Value v;
    switch (o.type) {
    case SMMENU_TOGGLE:
        if (in.kind == Value::Bool || in.kind == Value::Number) { v.kind = Value::Bool; v.n = in.n != 0; }
        break;
    case SMMENU_SLIDER:
        if (in.kind != Value::Number) break;
        v.kind = Value::Number;
        v.n = in.n;
        if (o.step > 0) v.n = o.min + std::round((v.n - o.min) / o.step) * o.step;
        v.n = std::min(std::max(v.n, o.min), o.max);
        break;
    case SMMENU_CHOICE:
        if (in.kind == Value::Number && in.n >= 0 && in.n < (double)o.choices.size()) { v.kind = Value::Text; v.s = o.choices[(size_t)in.n]; }
        else if (in.kind == Value::Text) for (auto& c : o.choices) if (c == in.s) { v.kind = Value::Text; v.s = c; break; }
        break;
    case SMMENU_TEXT:
        if (in.kind == Value::Text) { v.kind = Value::Text; v.s = in.s.substr(0, kMaxText); }
        break;
    case SMMENU_KEY:
        if (in.kind != Value::Text) break;
        v.s = Upper(in.s);
        if (v.s.empty() || KeyCode(v.s)) v.kind = Value::Text;
        break;
    case SMMENU_COLOR:
        if (in.kind == Value::Text && in.s.size() == 7 && in.s[0] == '#' && in.s.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos) {
            v.kind = Value::Text;
            v.s = Upper(in.s);
        }
        break;
    }
    return v;
}

Value Default(const Option& o) {
    Value v;
    if (o.type == SMMENU_TOGGLE) { v.kind = Value::Bool; v.n = o.def != 0; }
    else if (o.type == SMMENU_SLIDER || o.type == SMMENU_CHOICE) { v.kind = Value::Number; v.n = o.def; }
    else if (o.type == SMMENU_TEXT || o.type == SMMENU_KEY || o.type == SMMENU_COLOR) { v.kind = Value::Text; v.s = o.defText; }
    Value f = Fit(o, v);
    if (f.kind == Value::None) {                      // a bad default must not break the option
        if (o.type == SMMENU_CHOICE && !o.choices.empty()) { f.kind = Value::Text; f.s = o.choices[0]; }
        if (o.type == SMMENU_KEY) f.kind = Value::Text, f.s.clear();
        if (o.type == SMMENU_COLOR) { f.kind = Value::Text; f.s = "#FFFFFF"; }
    }
    return f;
}

// What applies right now: the host's value, else the saved one, else the default.
Value Current(const std::string& page, const std::string& option) {
    Option* o = FindOption(page.c_str(), option.c_str());
    auto pick = [&](const std::string& json) { Value v = Decode(json); return o ? Fit(*o, v) : v; };
    auto ov = g_override.find(page + "/" + option);
    if (ov != g_override.end()) { Value v = pick(ov->second); if (v.kind != Value::None) return v; }
    if (ValidId(page.c_str())) {
        Keys& keys = Store(page);
        auto it = keys.find(option);
        if (it != keys.end()) { Value v = pick(it->second); if (v.kind != Value::None) return v; }
    }
    return o ? Default(*o) : Value{};
}

double AsNumber(const Option* o, const Value& v, double fallback) {
    if (v.kind == Value::Bool || v.kind == Value::Number) return v.n;
    if (v.kind == Value::Text && o && o->type == SMMENU_CHOICE) {
        for (size_t i = 0; i < o->choices.size(); ++i) if (o->choices[i] == v.s) return (double)i;
    }
    if (v.kind == Value::Text && o && o->type == SMMENU_KEY) return KeyCode(v.s);
    return fallback;
}
std::string AsText(const Value& v) {
    if (v.kind == Value::Text) return v.s;
    if (v.kind == Value::None) return "";
    return Encode(v);
}

// Tell a native option what happened. Called without the lock held.
void Fire(const std::string& page, const std::string& option, int kind) {
    SMMenuFn fn = nullptr;
    void* user = nullptr;
    SMMenuEvent e{};
    std::string text;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        Option* o = FindOption(page.c_str(), option.c_str());
        if (!o || !o->fn) return;
        fn = o->fn;
        user = o->user;
        Value v = Current(page, option);
        e.number = AsNumber(o, v, 0);
        text = AsText(v);
    }
    e.size = sizeof e;
    e.kind = kind;
    e.page = page.c_str();
    e.option = option.c_str();
    e.text = text.c_str();
    try { fn(&e, user); }
    catch (...) { g_host.log("SMModsMenu: the handler of %s/%s threw an exception", page.c_str(), option.c_str()); }
}

int SetValue(const char* page, const char* option, const Value& in) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        Option* o = FindOption(page, option);
        if (!o) return 0;
        if (g_override.count(std::string(page) + "/" + option)) return 0;      // the host decides
        Value v = Fit(*o, in);
        if (v.kind == Value::None) return 0;
        std::string json = Encode(v);
        if (json == Encode(Current(page, option))) return 1;
        Store(page)[option] = json;
        g_dirty.insert(page);
        Save(page);
    }
    Fire(page, option, SMMENU_CHANGED);
    g_host.callLua("__fromNative", page, option);
    return 1;
}

// --- Lua natives -------------------------------------------------------------------
std::string Arg(lua_State* L, int i) {
    size_t n = 0;
    const char* s = LUA->lua_type(L, i) == LUA_TSTRING ? LUA->lua_tolstring(L, i, &n) : nullptr;
    return s ? std::string(s, n) : std::string();
}
void PushStr(lua_State* L, const std::string& s) { LUA->lua_pushlstring(L, s.data(), s.size()); }
void SetStr(lua_State* L, const char* k, const std::string& v) { if (!v.empty()) { PushStr(L, v); LUA->lua_setfield(L, -2, k); } }
void SetNum(lua_State* L, const char* k, double v) { LUA->lua_pushnumber(L, v); LUA->lua_setfield(L, -2, k); }
void SetBool(lua_State* L, const char* k, bool v) { if (v) { LUA->lua_pushboolean(L, 1); LUA->lua_setfield(L, -2, k); } }

// get(page, option) -> saved JSON | nil
int N_get(lua_State* L) {
    std::string page = Arg(L, 1), option = Arg(L, 2), json;
    if (ValidId(page.c_str())) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (g_saved.count(page) || g_saved.size() < kMaxPages) {
            Keys& keys = Store(page);
            auto it = keys.find(option);
            if (it != keys.end()) json = it->second;
        }
    }
    if (json.empty()) LUA->lua_pushnil(L);
    else PushStr(L, json);
    return 1;
}

// set(page, option, json | nil, save) -> ok
int N_set(lua_State* L) {
    std::string page = Arg(L, 1), option = Arg(L, 2), json = Arg(L, 3);
    bool save = LUA->lua_toboolean(L, 4) != 0, ok = false;
    if (ValidId(page.c_str()) && ValidId(option.c_str()) && json.size() <= kMaxText * 2) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (g_saved.count(page) || g_saved.size() < kMaxPages) {
            Keys& keys = Store(page);
            if (json.empty()) { keys.erase(option); ok = true; }
            else if (Decode(json).kind != Value::None && json.find_first_of("\r\n") == std::string::npos &&
                     (keys.count(option) || keys.size() < kMaxKeys)) { keys[option] = json; ok = true; }
            if (ok) g_dirty.insert(page);
            if (save) Save(page);
        }
    }
    LUA->lua_pushboolean(L, ok);
    return 1;
}

int N_flush(lua_State*) { Menu_Flush(); return 0; }

// override(page, option, json | nil): the host's value (nil: back to our own)
int N_override(lua_State* L) {
    std::string key = Arg(L, 1) + "/" + Arg(L, 2), json = Arg(L, 3);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (json.empty()) g_override.erase(key);
    else if (g_override.size() < kMaxKeys && json.size() <= kMaxText * 2) g_override[key] = json;
    return 0;
}

// event(page, option, kind): run a native option's handler
int N_event(lua_State* L) {
    std::string kind = Arg(L, 3);
    Fire(Arg(L, 1), Arg(L, 2), kind == "click" ? SMMENU_CLICKED : kind == "press" ? SMMENU_PRESSED : SMMENU_CHANGED);
    return 0;
}

// session(key [, value]): a string kept until the game exits
int N_session(lua_State* L) {
    std::string key = Arg(L, 1);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (LUA->lua_gettop(L) >= 2) {
        std::string v = Arg(L, 2);
        if ((g_session.count(key) || g_session.size() < kMaxKeys) && key.size() < 256 && v.size() <= kMaxText) g_session[key] = v;
        return 0;
    }
    auto it = g_session.find(key);
    if (it == g_session.end()) LUA->lua_pushnil(L);
    else PushStr(L, it->second);
    return 1;
}

// pages() -> the native mods' pages
int N_pages(lua_State* L) {
    static const char* types[] = {"", "section", "info", "toggle", "slider", "choice", "text", "key", "color", "button"};
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    LUA->lua_checkstack(L, 16);
    LUA->lua_createtable(L, (int)g_pages.size(), 0);
    for (size_t i = 0; i < g_pages.size(); ++i) {
        const Page& p = g_pages[i];
        LUA->lua_createtable(L, 0, 8);
        SetStr(L, "id", p.id);
        SetStr(L, "name", p.name);
        SetStr(L, "version", p.version);
        SetStr(L, "author", p.author);
        SetStr(L, "desc", p.desc);
        SetBool(L, "locked", g_external.count(p.id) != 0);
        LUA->lua_createtable(L, (int)p.options.size(), 0);
        for (size_t k = 0; k < p.options.size(); ++k) {
            const Option& o = p.options[k];
            LUA->lua_createtable(L, 0, 14);
            SetStr(L, "type", types[o.type]);
            SetStr(L, "id", o.id);
            SetStr(L, "label", o.label);
            SetStr(L, "desc", o.desc);
            SetStr(L, "enabledBy", o.enabledBy);
            SetStr(L, "format", o.format);
            SetStr(L, "caption", o.caption);
            SetBool(L, "host", (o.flags & SMMENU_HOST) != 0);
            SetBool(L, "restart", (o.flags & SMMENU_RESTART) != 0);
            SetBool(L, "confirm", (o.flags & SMMENU_CONFIRM) != 0);
            if (o.type == SMMENU_SLIDER) { SetNum(L, "min", o.min); SetNum(L, "max", o.max); SetNum(L, "step", o.step); }
            if (o.type == SMMENU_TEXT && o.maxLength > 0) SetNum(L, "max", o.maxLength);
            Value d = Default(o);
            if (d.kind == Value::Bool) { LUA->lua_pushboolean(L, d.n != 0); LUA->lua_setfield(L, -2, "default"); }
            else if (d.kind == Value::Number) SetNum(L, "default", d.n);
            else if (d.kind == Value::Text) { PushStr(L, d.s); LUA->lua_setfield(L, -2, "default"); }
            if (!o.choices.empty()) {
                LUA->lua_createtable(L, (int)o.choices.size(), 0);
                for (size_t c = 0; c < o.choices.size(); ++c) { PushStr(L, o.choices[c]); LUA->lua_rawseti(L, -2, (int)c + 1); }
                LUA->lua_setfield(L, -2, "choices");
            }
            LUA->lua_rawseti(L, -2, (int)k + 1);
        }
        LUA->lua_setfield(L, -2, "options");
        LUA->lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

std::atomic<bool> g_openWanted{false};
std::string g_openPage;                               // guarded by g_mx

// takeOpen() -> page id ("" = whichever was last) | nil
int N_takeOpen(lua_State* L) {
    if (!g_openWanted.exchange(false)) { LUA->lua_pushnil(L); return 1; }
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    PushStr(L, g_openPage);
    g_openPage.clear();
    return 1;
}

// --- the MODS button: layout -----------------------------------------------------
const char kLayout[] = "gui/layouts/ingamemenu/ingamemenu.layout";
const char* kPanels[]  = {"NormalPanel", "ChallengePanel", "ChallengeBuilderPanel"};
const char* kButtons[] = {"SMLoaderMods", "SMLoaderMods2", "SMLoaderMods3"};   // one per panel; at most 15 characters

struct Child { std::string name; size_t posAt, posLen, end; double x, y, w, h; };

// Where an attribute's text is in the tag that starts at `tag`, or npos.
size_t Attr(const std::string& t, size_t tag, const char* name, size_t* len) {
    size_t close = t.find('>', tag), at = t.find(std::string(" ") + name + "=\"", tag);
    if (at == std::string::npos || close == std::string::npos || at > close) return std::string::npos;
    at += strlen(name) + 3;
    size_t e = t.find('"', at);
    if (e == std::string::npos) return std::string::npos;
    *len = e - at;
    return at;
}
bool ReadPos(const std::string& s, double* x, double* y, double* w, double* h) { return sscanf(s.c_str(), "%lf %lf %lf %lf", x, y, w, h) == 4; }
std::string WritePos(double x, double y, double w, double h) {
    char b[128];
    snprintf(b, sizeof b, "%.6g %.6g %.6g %.6g", x, y, w, h);
    return b;
}

// Add our button to one panel: the panel grows by one row and everything below
// the new button moves down. These panels' children have no children.
bool EditPanel(std::string& t, const char* panel, const char* button) {
    size_t at = t.find(std::string("name=\"") + panel + "\"");
    if (at == std::string::npos) return false;
    size_t tag = t.rfind("<Widget", at), len = 0;
    size_t pos = tag == std::string::npos ? tag : Attr(t, tag, "position_real", &len);
    double px, py, pw, ph;
    if (pos == std::string::npos || !ReadPos(t.substr(pos, len), &px, &py, &pw, &ph) || ph <= 0) return false;

    std::vector<Child> kids;
    for (size_t i = t.find('>', tag) + 1;;) {
        size_t open = t.find("<Widget", i), close = t.find("</Widget>", i);
        if (close == std::string::npos) return false;
        if (open == std::string::npos || close < open) break;         // the panel's own end
        Child c;
        size_t n = 0, p = Attr(t, open, "name", &n);
        if (p != std::string::npos) c.name = t.substr(p, n);
        c.posAt = Attr(t, open, "position_real", &c.posLen);
        if (c.posAt == std::string::npos || !ReadPos(t.substr(c.posAt, c.posLen), &c.x, &c.y, &c.w, &c.h)) return false;
        size_t gt = t.find('>', open);
        if (t[gt - 1] == '/') c.end = gt + 1;                          // <Widget ... />
        else {
            size_t e = t.find("</Widget>", gt), nested = t.find("<Widget", gt);
            if (e == std::string::npos || (nested != std::string::npos && nested < e)) return false;
            c.end = e + 9;
        }
        kids.push_back(c);
        i = c.end;
    }
    // Below UNSTUCK; in panels without it, below the last button above the divider.
    const Child* after = nullptr;
    double divider = 2, above = -1;
    for (auto& c : kids) if (c.name == "Divider") divider = c.y;
    for (auto& c : kids) if (c.name == "Unstuck") after = &c;
    if (!after) for (auto& c : kids) if (c.y < divider - 1e-6 && (!after || c.y >= after->y)) after = &c;
    if (!after) return false;
    for (auto& c : kids) if (c.y < after->y - 1e-6 && c.y > above) above = c.y;
    if (above < 0) return false;
    const double baseY = after->y, row = baseY - above, scale = 1 / (1 + row);
    const size_t insertAt = after->end;

    std::string widget =
        "\n        <Widget type=\"Button\" skin=\"SettingsButton\" name=\"" + std::string(button) + "\" position_real=\"" +
        WritePos(after->x, (baseY + row) * scale, after->w, after->h * scale) + "\">\n"
        "            <Property key=\"Caption\" value=\"MODS\" />\n"
        "            <Property key=\"FontName\" value=\"SM_Button\" />\n"
        "            <Property key=\"TextAlign\" value=\"Center\" />\n"
        "        </Widget>";
    for (size_t k = kids.size(); k-- > 0;) {                           // back to front: offsets stay valid
        const Child& c = kids[k];
        if (c.end == insertAt) t.insert(insertAt, widget);
        t.replace(c.posAt, c.posLen, WritePos(c.x, (c.y + (c.y > baseY + 1e-6 ? row : 0)) * scale, c.w, c.h * scale));
    }
    t.replace(pos, len, WritePos(px, py, pw, ph * (1 + row)));
    return true;
}

// The game's text loader: out = { data, size, owned }; data may point into a
// cache file (then owned is false and nobody frees it).
struct GameBuffer { void* data; uint32_t size; uint8_t owned; };
struct GameStringView { union { char buf[16]; const char* ptr; }; size_t size, cap; const char* data() const { return cap > 15 ? ptr : buf; } };
typedef GameBuffer* (*LoadTextFn)(GameBuffer* out, const GameStringView* path, void* bundle, void* version);
LoadTextFn o_LoadText = nullptr;
void* (__cdecl *g_gameMalloc)(size_t) = nullptr;
void  (__cdecl *g_gameFree)(void*) = nullptr;

bool IsLayout(const GameStringView* path) {
    const size_t n = sizeof kLayout - 1;
    if (!path || path->size < n) return false;
    const char* tail = path->data() + path->size - n;
    for (size_t i = 0; i < n; ++i) {
        char c = tail[i] == '\\' ? '/' : (char)tolower((unsigned char)tail[i]);
        if (c != kLayout[i]) return false;
    }
    return true;
}

GameBuffer* H_LoadText(GameBuffer* out, const GameStringView* path, void* bundle, void* version) {
    bool ours = IsLayout(path);
    GameBuffer* r = o_LoadText(out, path, bundle, version);
    if (!ours || !r || !r->data || !r->size) return r;
    std::string text((const char*)r->data, r->size);
    int done = 0;
    for (size_t i = 0; i < std::size(kPanels); ++i) done += EditPanel(text, kPanels[i], kButtons[i]);
    void* mem = done ? g_gameMalloc(text.size()) : nullptr;
    if (!mem) {
        g_host.log("SMModsMenu: the pause menu's layout is not the one we know - no MODS button");
        return r;
    }
    memcpy(mem, text.data(), text.size());
    if (r->owned) g_gameFree(r->data);
    r->data = mem;
    r->size = (uint32_t)text.size();
    r->owned = 1;
    return r;
}

// --- the MODS button: wiring -------------------------------------------------------
// MSVC std::string, short enough to live in its own buffer.
struct GameString { char buf[16]; size_t size, cap; };
GameString MakeString(const char* s) {
    GameString g{};
    g.size = strlen(s);
    g.cap = 15;
    memcpy(g.buf, s, g.size);
    return g;
}

typedef void* (*InitFn)(void* self, void* b, void* c, void* d);
typedef void* (*FindFn)(void* widget, const GameString* name);
typedef void  (*MethodFn)(void* self, void* sender);
typedef void* (*ClickFn)(void* self, MethodFn method);
typedef void* (*HoverFn)(void* self);
typedef void  (*AddFn)(void* event, void* delegate);
InitFn   o_Init = nullptr;
FindFn   g_find = nullptr;
ClickFn  g_click = nullptr;
HoverFn  g_hover = nullptr;
AddFn    g_add = nullptr;
MethodFn g_resume = nullptr;
int32_t  g_clickAt = 0, g_hoverAt = 0;                 // where a widget keeps these events

void OnMods(void* self, void* sender) {
    if (!Menu_Open("")) g_host.log("SMModsMenu: the SDK is not running in this game mode, no page to show");
    g_resume(self, sender);                            // click sound, close the pause menu
}

void* H_Init(void* self, void* b, void* c, void* d) {
    void* r = o_Init(self, b, c, d);
    void* root = *(void**)((char*)self + 8);
    int n = 0;
    for (const char* name : kButtons) {
        GameString s = MakeString(name);
        void* w = root ? g_find(root, &s) : nullptr;
        if (!w) continue;
        g_add((char*)w + g_clickAt, g_click(self, OnMods));
        g_add((char*)w + g_hoverAt, g_hover(self));
        ++n;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        g_host.log("SMModsMenu: MODS button %s (%d panels)", n ? "added to the pause menu" : "NOT in the pause menu", n);
    }
    return r;
}

const char* kInitSig =
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00 0F 29 70 C8 "
    "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 28 48 8B F1 48 83 79 08 00 74 19";
const char* kFindSig =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 6A 18 48 8B F2 48 8B 5A 10 48 8B F9 "
    "0F 1F 40 00 66 66 0F 1F 84 00 00 00 00 00 48 8D 97";

const char* kTextSig =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC F8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 0F "
    "4D 8B F1 4D 8B F8 48 8B DA 48 8B F9 48 89 4D A7 45 33 E4 44 89 65 93 41 BD 01 00 00 00";

const uint8_t* Target(const uint8_t* rel32) { int32_t d; memcpy(&d, rel32, 4); return rel32 + 4 + d; }

// The first button the menu's set-up code wires is RESUME:
//   lea rdx, onResume; mov rcx, rsi; call makeClick; mov rdx, rax; lea rcx, [r14+click]; call add
//   mov rcx, rsi; call makeHover; mov rdx, rax; lea rcx, [r14+hover]; call add
bool ReadWiring(const uint8_t* init) {
    static const int code[] = {0x48, 0x8D, 0x15, -1, -1, -1, -1, 0x48, 0x8B, 0xCE, 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0xD0,
                               0x49, 0x8D, 0x8E, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0xCE, 0xE8, -1, -1, -1, -1,
                               0x48, 0x8B, 0xD0, 0x49, 0x8D, 0x8E, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1};
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)init, &base, nullptr);
    if (!rf || base + rf->BeginAddress != (DWORD64)init) return false;
    const uint8_t* end = (const uint8_t*)base + rf->EndAddress;
    for (const uint8_t* p = init; p + std::size(code) <= end; ++p) {
        bool match = true;
        for (size_t i = 0; match && i < std::size(code); ++i) match = code[i] < 0 || p[i] == code[i];
        if (!match) continue;
        g_resume = (MethodFn)Target(p + 3);
        g_click = (ClickFn)Target(p + 11);
        memcpy(&g_clickAt, p + 21, 4);
        g_add = (AddFn)Target(p + 26);
        g_hover = (HoverFn)Target(p + 34);
        memcpy(&g_hoverAt, p + 44, 4);
        return g_add == (AddFn)Target(p + 49) && g_clickAt > 0 && g_clickAt < 0x400 && g_hoverAt > 0 && g_hoverAt < 0x400;
    }
    return false;
}

}  // namespace

void Menu_Init(const MenuHost& host) { g_host = host; }

void Menu_InstallButton() {
    uint8_t* init = Code_ScanUnique(kInitSig, 15);
    uint8_t* text = Code_ScanUnique(kTextSig, 18);
    g_find = (FindFn)Code_ScanUnique(kFindSig, 15);
    if (HMODULE crt = GetModuleHandleW(L"ucrtbase.dll")) {
        g_gameMalloc = (decltype(g_gameMalloc))GetProcAddress(crt, "malloc");
        g_gameFree = (decltype(g_gameFree))GetProcAddress(crt, "free");
    }
    if (!init || !text || !g_find || !g_gameMalloc || !g_gameFree || !ReadWiring(init)) {
        g_host.log("SMModsMenu: the pause menu of this game version is not recognised - no MODS button");
        return;
    }
    if (!Code_Hook(init, 15, (const void*)H_Init, (void**)&o_Init) || !Code_Hook(text, 18, (const void*)H_LoadText, (void**)&o_LoadText))
        g_host.log("SMModsMenu: can't hook the pause menu - no MODS button");
}

int Menu_PushNatives(lua_State* L) {
    LUA->lua_createtable(L, 0, 8);
    struct { const char* name; lua_CFunction fn; } fns[] = {
        {"get", N_get}, {"set", N_set}, {"flush", N_flush}, {"override", N_override},
        {"event", N_event}, {"session", N_session}, {"pages", N_pages}, {"takeOpen", N_takeOpen}};
    for (auto& f : fns) { LUA->lua_pushcclosure(L, f.fn, 0); LUA->lua_setfield(L, -2, f.name); }
    return 1;
}

void Menu_WorldClosed() {
    g_openWanted = false;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_override.clear();
    SaveAll();
}

void Menu_Flush() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    SaveAll();
}

void Menu_SeedExternal(const char* page, const char* option, bool on) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_external.insert(page);
    g_saved[page][option] = on ? "true" : "false";
}

static std::string Copy(const char* s) { return std::string(s ? s : "").substr(0, kMaxText); }

int Menu_AddPage(const SMMenuPage* in) {
    if (!in || in->size < sizeof(SMMenuPage) || !ValidId(in->id)) return 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        Page* p = FindPage(in->id);
        if (!p) {
            if (g_pages.size() >= kMaxPages) return 0;
            g_pages.emplace_back();
            p = &g_pages.back();
        }
        p->id = in->id;
        p->name = Copy(in->name);
        if (p->name.empty()) p->name = p->id;
        p->version = Copy(in->version);
        p->author = Copy(in->author);
        p->desc = Copy(in->description);
    }
    g_host.callLua("__nativePages", "", "");
    return 1;
}

int Menu_AddOption(const char* page, const SMMenuOption* in) {
    if (!in || in->size < sizeof(SMMenuOption) || in->type < SMMENU_SECTION || in->type > SMMENU_BUTTON) return 0;
    bool needsId = in->type != SMMENU_SECTION && in->type != SMMENU_INFO;
    if (needsId && !ValidId(in->id)) return 0;
    Option o;
    o.type = in->type;
    o.id = Copy(in->id);
    o.label = Copy(in->label);
    if (o.label.empty()) o.label = o.id;
    o.desc = Copy(in->description);
    o.enabledBy = Copy(in->enabledBy);
    o.format = Copy(in->format);
    o.caption = Copy(in->caption);
    o.defText = Copy(in->defText);
    o.flags = in->flags;
    o.def = in->defNumber;
    o.min = in->min;
    o.max = in->max;
    o.step = in->step;
    o.maxLength = in->maxLength;
    o.fn = in->fn;
    o.user = in->user;
    if (in->choices) for (const char* const* c = in->choices; *c && o.choices.size() < 256; ++c) o.choices.push_back(Copy(*c));
    if (o.type == SMMENU_SLIDER && !(o.max > o.min)) return 0;
    if (o.type == SMMENU_CHOICE && o.choices.empty()) return 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        Page* p = FindPage(page);
        if (!p || p->options.size() >= kMaxKeys) return 0;
        if (Option* old = needsId ? FindOption(page, o.id.c_str()) : nullptr) *old = o;
        else p->options.push_back(o);
    }
    g_host.callLua("__nativePages", "", "");
    return 1;
}

double Menu_GetNumber(const char* page, const char* option, double fallback) {
    if (!page || !option) return fallback;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    return AsNumber(FindOption(page, option), Current(page, option), fallback);
}

int Menu_GetText(const char* page, const char* option, char* out, size_t size) {
    if (out && size) out[0] = 0;
    if (!page || !option) return -1;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Value v = Current(page, option);
    if (v.kind == Value::None) return -1;
    std::string s = AsText(v);
    if (out && size) snprintf(out, size, "%s", s.c_str());
    return (int)s.size();
}

int Menu_SetNumber(const char* page, const char* option, double value) {
    if (!page || !option) return 0;
    Value v;
    v.kind = Value::Number;
    v.n = value;
    return SetValue(page, option, v);
}

int Menu_SetText(const char* page, const char* option, const char* text) {
    if (!page || !option || !text) return 0;
    Value v;
    v.kind = Value::Text;
    v.s = text;
    return SetValue(page, option, v);
}

int Menu_Open(const char* page) {
    if (!g_host.callLua("__wake", "", "")) return 0;   // no world: nothing could show it
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        g_openPage = page ? page : "";
    }
    g_openWanted = true;
    return 1;
}
