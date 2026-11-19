# SMLoader API reference (API version 1)

Everything is declared in [`include/smloader.h`](../include/smloader.h). It is
plain C, so you can write mods in C or C++.

## Entry points

```c
__declspec(dllexport) void SMMod_Init(const SMLoaderAPI* api);   // required
__declspec(dllexport) void SMMod_Shutdown(void);                 // optional
```

- `SMMod_Init` is called **once**, outside the Windows loader lock, as soon as
  the game has started (at the main menu, before any Lua state exists).
- Creating threads, loading libraries and installing hooks is fine here.
  Don't touch Lua yet: wait for `OnStateCreated` or `OnGameTick`.
- Check `api->version >= SMLOADER_API_VERSION`. New functions are only ever
  added at the end of `SMLoaderAPI`, so also check any function pointer you
  need is non-null.

## `SMLoaderAPI` fields

| Field | Description |
|---|---|
| `version` | API version (`SMLOADER_API_VERSION`) |
| `loaderDir` | Absolute UTF-8 path of `<game>\SMLoader`. Put your mod's data under it |
| `Log(fmt, ...)` | printf-style line in `SMLoader\loader.log` |
| `lua` | Table of LuaJIT C functions (`lua_gettop`, `lua_pcall`, `luaL_loadbufferx`, …). These are the **original** functions, not the hooked ones |

### `RegisterCallbacks(const SMModCallbacks* cb)`

Copied by value, so a `static` struct is fine. Every field is optional.

| Callback | When | Thread |
|---|---|---|
| `OnStateCreated(L, user)` | The game created a Lua state (one gameplay state, plus several terrain-generation states) | game |
| `OnStateClosed(L, user)` | Right before the game closes it | game |
| `OnScriptLoad(L, chunkname, &buf, &len, user)` | The game is about to compile a script. Chunk names look like `$SURVIVAL_DATA/Scripts/game/SurvivalGame.lua`. To replace the source, point `*buf`/`*len` at memory that stays valid until the callback returns, and return non-zero | game |
| `OnGameTick(L, user)` | At a safe point, at most every ~8 ms per state. The stack top is restored for you | owner of `L` |

### Running code in the game

All of these are thread-safe. Call them from anywhere.

```c
void QueueJob(lua_State* L, SMJobFn fn, void* user);
```
Runs `fn(L, user)` at the next safe point under `lua_cpcall`, so a Lua error
inside it can't escape into the game.

```c
void QueueLua(lua_State* L, const char* source, const char* chunkname, const char* envGlobal,
              void (*done)(int ok, const char* text, void* user), void* user);
```
Compiles and runs `source` at the next safe point.
- `envGlobal`:
  - `"game"`: the env game scripts share, which holds the game classes.
  - `NULL`: the state's raw globals.
  - Any other string: a global table with that name.
- `done` receives `ok` plus either the chunk's return values (tab-separated)
  or the error text. That text is only valid during the call.

```c
void QueueLuaIn(lua_State* L, const char* callback, const char* source, const char* chunkname,
                void (*done)(int ok, const char* text, void* user), void* user);
```
Like `QueueLua` with `env = "game"`, but it runs **right before the game next
calls `callback`**. `callback` is an expression naming a script function, such
as `"SurvivalGame.client_onUpdate"`.
- Your code runs inside that callback's engine sandbox, so client or server
  `sm.*` calls are allowed. It receives the callback's arguments as `...`.
- The target is re-resolved every 2 s, so hot-reloaded scripts are fine.
- If the callback isn't called within 10 s, `done` gets an error.

Useful targets in Survival:

| Target | Context | Notes |
|---|---|---|
| `SurvivalGame.client_onUpdate` | client, every frame | `self` = game script instance |
| `SurvivalGame.server_onFixedUpdate` | server, 40 Hz | host only |
| `SurvivalPlayer.server_onFixedUpdate` | server, world script | needed for physics and `sm.body`/`sm.creation` world calls |
| `SurvivalPlayer.client_onFixedUpdate` | client, world script | e.g. `sm.tool.checkLiftCollisionAtLevel` |

```c
int GetStates(lua_State** out, int max);
```
Copies up to `max` live states into `out`. Returns the total number of states.

### Hooks

```c
int PatchImport(const char* dll, const char* func, void* hook, void** original);
```
Redirects `ScrapMechanic.exe`'s import of `dll!func`, e.g. `"USER32.dll", "SetCursorPos"`.
`*original` receives the function your hook should call next. That is another
mod's hook if one got there first, so hooks chain; otherwise it's the real
function. Returns non-zero on success.

```c
int AddPresentCallback(void (*fn)(void* swapChain, void* user), void* user);
```
`fn` runs on the render thread right before every `IDXGISwapChain::Present`
(`swapChain` is an `IDXGISwapChain*`).
- Draw your overlay here.
- Create your render-target view each frame and release it afterwards, so you
  never hold the back buffer when the game resizes.
- The loader installs the hook a few seconds after the game window appears.

## Lua environments

| Env | Has | Doesn't have |
|---|---|---|
| `"game"` (shared script env) | game classes (`SurvivalGame`, `BasePlayer`, …), `sm.*`, `class`, whitelisted `string`/`table`/`math`/`os`/`bit` | `getfenv`, `rawget`, `setmetatable`, `debug`, `ffi`, `loadstring` |
| raw globals (`NULL`) | everything LuaJIT opened: `debug`, `ffi`, `os`, `package`, `unsafe_env` | the game classes (they're in the game env) |

From the raw env, you can reach the game env as the registry table whose
metatable `__index` is `unsafe_env`.
