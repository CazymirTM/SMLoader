# Changelog

## 1.2.0 — SMPlayerData, SMEvents and SMExtensions

- **SMPlayerData:** persistent player statistics and progression, built into
  the loader and saved in the world. Every world gets a global `SMData` table
  (Lua), and native mods get `DataGet` / `DataSet` / `DataAdd` / `DataAddXP` /
  `DataUnlock` (API version 3).
  - Automatic stats: playtime, sessions, distance walked and driven. In
    Survival also deaths, damage taken, robot kills (per type), robot damage,
    crops harvested and food eaten.
  - Per-player mod data (private or published), XP with level curves,
    level-up events and unlocks.
  - Leaderboards, including offline players on the host.
  - Each player's own record is synced to them; everyone's stats and published
    keys are synced to all players.
  - Guide: `docs/PLAYERDATA.md`.
- **SMEvents:** one event bus shared by Lua mods (`SMEvents`), native mods
  (`EventOn` / `EventOff` / `EventEmit` / `EventEmitTo`) and the SDK.
  - Priorities, `once`, and `*` patterns.
  - Lua tables reach C++ as JSON and C++ JSON reaches Lua as tables.
  - Networked events over SMNetworking.
  - Built-in events: `world.loaded`, `world.closed`, `net.connected`,
    `player.joined`, `player.left`, `player.death`, `player.damaged`,
    `robot.killed`, `crop.harvested`, `food.eaten`, `data.levelUp`,
    `data.unlock`.
  - Guide: `docs/EVENTS.md`.
- **SMExtensions:** more functionality for Lua (`SMX`).
  - Native functions callable from Lua (`ExtRegister` / `ExtRegisterRaw` →
    `SMX.fn.<mod>.<name>`).
  - Timers (client or host).
  - Hotkeys (only while the game is focused).
  - A private, token-protected folder per mod (`SMLoader\data\<mod>\`) with
    JSON and config helpers.
  - `setmetatable` / `rawget` / `compile`.
  - Table, string and math utilities, `pretty`, `crc32`, `base64`, a
    high-resolution clock, `date`, `profile` and `log`.
  - `WorldState()` gives native mods the world's Lua state.
  - Guide: `docs/EXTENSIONS.md`.
- **SMNetworking:** `SMNet.onServerTick` / `SMNet.onClientTick` (per-tick
  listeners without wrapping) and `SMNet.addCallbackHook` for Lua.
- New examples: **DataDemo** (native SMPlayerData), **EventDemo** (native
  SMEvents) and **ExtDemo** (SMExtensions).
- API version 3. Mods built for 1.0.0 and 1.1.0 keep working unchanged.

## 1.1.0 — SMNetworking

- **SMNetworking:** tools for multiplayer mods that stay in sync, built into
  the loader. Every world gets a global `SMNet` table (Lua) and native mods get
  `NetRegister` / `NetOn` / `NetSend` / `NetGetStatus` (API version 2).
  - Messages: client → host, host → player, host → everyone, and player →
    everyone (relayed by the host).
  - Requests with replies and timeouts.
  - Host-owned shared state, mirrored to every player, including late
    joiners.
  - A roster of players and their mods, plus `SMNet.isReady(mod)` /
    `SMNet.missing(mod)`.
  - A handshake, so players without SMLoader are never sent mod traffic.
  - `SMNet.json` encoder and decoder.
  - Guide: `docs/NETWORKING.md`.
- **Callback hooks** (`AddCallbackHook`): run a Lua function before any game
  callback without wrapping it, so several mods can hook the same callback
  safely.
- New example: **NetDemo** (native networking).
- Release builds no longer embed local build paths.
- API version 2. Mods built for 1.0.0 keep working unchanged.

## 1.0.0 — first release

- `winmm.dll` proxy loader for Scrap Mechanic 1.0.7a (Steam, x64).
- Loads native mods from `SMLoader\mods\*.dll` (`SMMod_Init` entry point).
- Lua access: tracks the game's LuaJIT states. Mods can run Lua at safe points
  (`QueueLua`/`QueueJob`) or inside a named game callback, so `sm.*` calls are
  allowed (`QueueLuaIn`).
- `OnScriptLoad`: read or rewrite any game script as it compiles. File
  overrides go in `SMLoader\overrides\`.
- `PatchImport`: redirect the game's imports. When several mods hook the same
  import, the hooks chain.
- `AddPresentCallback`: one shared D3D11 `Present` hook for overlay mods. It is
  safe alongside the Steam overlay.
- Crash reporter: `loader.log` records the faulting module and a stack trace.
- Examples: HelloMod (template), LuaConsole (developer console + named pipe),
  DevCommands (script-patching example).
