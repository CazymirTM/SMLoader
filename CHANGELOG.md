# Changelog

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
