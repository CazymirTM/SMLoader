# Changelog

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
