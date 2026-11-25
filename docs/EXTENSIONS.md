# SMExtensions

SMExtensions gives Lua the things the game's script sandbox leaves out. It is
built into SMLoader 1.2+. Every world gets a global table, `SMX`, in the
game's script environment, and native mods can add their own functions to it.

| | |
|---|---|
| **Native functions** | A C++ mod registers `mymod.doThing`; Lua calls `SMX.fn.mymod.doThing(...)` |
| **Timers** | `SMX.after`, `SMX.every`, `SMX.cancel`, on the client or the host |
| **Hotkeys** | `SMX.input.bind("F6", fn)`, `SMX.input.isKeyDown(key)`, plus `input.keyDown` / `input.keyUp` events |
| **Files and config** | Each mod's private folder `SMLoader\data\<mod>\`: read, write, append, list, JSON, `config()` |
| **Lua helpers** | `setmetatable`, `getmetatable` (tables only), `rawget`, `rawset`, `rawequal`, and `compile` (code into the game env) |
| **Utilities** | Table, string and math helpers, `pretty`, `json`, `crc32`, `base64`, `clock`, `date`, `profile`, `log` |
| **Info** | `SMX.loader` = `{ version, api, mods }` |

## Security

The game's Lua environment is **shared with Workshop scripts** (custom games,
mods from the Steam Workshop). Anything in `SMX` is visible to them too, so
everything that touches your PC is locked down:

- **Files:**
  - Only inside `SMLoader\data\<mod>\`.
  - Only with that mod's **secret token**, which only the mod's native code can
    get (`api->ExtToken`).
  - Paths may not contain `..`, drive letters or odd characters.
  - At most 4 MB per write and 64 MB per mod.
- **Keys:** only readable while the game window is in the foreground.
- **No network access and no clipboard reading:** nothing that could send your
  data off the PC.

Keep your token in a Lua **local**. A global would be visible to every script.

## Native functions

C++:
```cpp
// JSON in, JSON out: the arguments arrive as a JSON array
static int Add(const char* args, size_t len, char* out, size_t size, void* user) {
    double a, b;
    if (sscanf(std::string(args, len).c_str(), "[%lf,%lf]", &a, &b) != 2)
        return snprintf(out, size, "add(a, b) needs two numbers"), -1;   // negative = error
    return snprintf(out, size, "%.15g", a + b);                          // length of the JSON result
}
api->ExtRegister("mymod.add", Add, nullptr);

// Or full control with a plain lua_CFunction (use api->lua)
static int Greet(lua_State* L) {
    api->lua->lua_pushfstring(L, "Hello, %s!", api->lua->lua_tolstring(L, 1, nullptr));
    return 1;
}
api->ExtRegisterRaw("mymod.greet", Greet);
```

Lua:
```lua
SMX.fn.mymod.add( 2, 3 )            --> 5
SMX.fn.mymod.greet( "Ada" )         --> "Hello, Ada!"
SMX.call( "mymod.add", 2, 3 )       -- the same, by name (raises an error if it's missing)
```

- Functions run on the game thread, inside whatever Lua called them.
- Registration works at any time, including from `SMMod_Init`, and applies to
  every world.
- **Return values:**
  - If the result doesn't fit in `out`, return its full length and you'll be
    called again with a big enough buffer.
  - Leave `out` empty to return `nil`.

## Timers

```lua
local id = SMX.after( 2.5, function() print( "2.5 s later" ) end )           -- client (every machine)
SMX.every( 60, function() SMData.addXP( ... ) end, "server" )               -- host only
SMX.cancel( id )
```
- **Game time:** timers count game ticks (40 per second), so they pause when
  the game pauses.
- **Context:** they run inside the client (`"client"`, the default) or server
  (`"server"`) tick's sandbox, so the matching `sm.*` calls work.

## Hotkeys

```lua
SMX.input.bind( "F6", function( key ) print( key .. " pressed" ) end )   -- returns an id for unbind
if SMX.input.isKeyDown( "SHIFT" ) then ... end
SMEvents.on( "input.keyDown", function( e ) print( e.key ) end )          -- for bound keys
```

- **Key names:** `A`–`Z`, `0`–`9`, `F1`–`F12`, `SHIFT`, `CTRL`, `ALT`,
  `SPACE`, `ENTER`, `ESC`, `TAB`, the arrows, `INSERT`, `DELETE`, `HOME`, `END`,
  `PAGEUP`, `PAGEDOWN`, `NUMPAD0`–`NUMPAD9`, `MOUSE1`–`MOUSE5`, or a Windows
  virtual-key code.
- **Chat counts too:** keys typed in the chat also trigger binds, so prefer
  F-keys or key combinations.

## Files and config

Your native code passes the token to your Lua code:

```cpp
std::string src = std::string("local TOKEN = '") + api->ExtToken("mymod") + "'\n" + myLuaCode;
api->QueueLua(api->WorldState(), src.c_str(), "=MyMod", "game", nullptr, nullptr);
```

```lua
local fs = SMX.files( "mymod", TOKEN )

fs.write( "notes/today.txt", "hello" )          -- folders are created as needed
local text = fs.read( "notes/today.txt" )       -- nil, "not found" if missing
fs.append( "log.txt", "line\n" )
fs.exists( "log.txt" )  fs.remove( "log.txt" )  fs.list( "notes" )   -- names; folders end in "/"
fs.writeJson( "state.json", { a = 1 } )  local t = fs.readJson( "state.json" )

local cfg = fs.config( { volume = 0.8, showHud = true } )   -- config.json merged over the defaults
cfg.volume = 0.5
fs.saveConfig( cfg )
```

Failures return `nil, message`. Watch out for Lua's `tonumber(fs.read(path))`:
it passes the error message as `tonumber`'s second argument, which is the
number base. Write `tonumber((fs.read(path)))` instead.

The data is **per user, not per world**. For per-world, per-player data use
[SMPlayerData](PLAYERDATA.md).

## Lua helpers and utilities

| Function | Notes |
|---|---|
| `SMX.compile(source, name?)` | Returns a function running in the game env, or `nil, err` |
| `SMX.setmetatable(t, mt)`, `SMX.getmetatable(t)` | Tables only |
| `SMX.rawget`, `SMX.rawset`, `SMX.rawequal` | |
| `SMX.table.copy/deepCopy/merge(dst, src, deep)/keys/values/count/find/map/filter` | |
| `SMX.string.split(s, sep, plain?)/trim/startsWith/endsWith/pad(s, n, ch, left)` | |
| `SMX.math.clamp/lerp/round(v, decimals)/map/sign` | |
| `SMX.pretty(value, asJson?)` | Readable text (or indented JSON), with keys sorted |
| `SMX.json` | The same as `SMNet.json` (`encode`, `decode`) |
| `SMX.crc32(s)`, `SMX.base64.encode/decode` | |
| `SMX.clock()` | High-resolution milliseconds |
| `SMX.date(fmt?, unixTime?)` | Like `os.date`; the default format is `"%Y-%m-%d %H:%M:%S"` |
| `SMX.profile(name, thresholdMs, fn, ...)` | Runs `fn`, logs the time taken to `loader.log` if it's at least the threshold, and returns `fn`'s results |
| `SMX.log(...)` | A line in `SMLoader\loader.log`, prefixed `[lua]` |
| `SMX.loader` | `{ version = "1.5.1", api = 4, mods = { "MyMod.dll", ... } }` |

## Native API

```c
int ExtRegister(const char* name, SMExtFn fn, void* user);    // "mod.function"
int ExtRegisterRaw(const char* name, lua_CFunction fn);
const char* ExtToken(const char* modId);                     // the mod's file token
lua_State* WorldState(void);                                 // the world's Lua state, or NULL
```

`examples/ExtDemo` is a complete example. It registers both kinds of function,
counts world visits in its config file, and binds F7.
