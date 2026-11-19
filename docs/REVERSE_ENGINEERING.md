# How Scrap Mechanic runs Lua (reverse-engineering notes)

These findings come from Scrap Mechanic **1.0.7a (build 903)**, Steam, x64.
They came from the import table, from strings and code in the binary
(examined with Ghidra), and from probing the running game through SMLoader.

## Lua runtime

- The game uses **LuaJIT 2.1** from a separate `lua51.dll`. `ScrapMechanic.exe`
  imports about 70 `lua_*`/`luaL_*` functions from it, including
  `luaL_newstate`, `luaL_loadbufferx`, `lua_pcall`, `lua_setfenv`, `luaL_ref`
  and most `luaopen_*`. It does **not** import `luaopen_io` or `luaopen_jit`.
- There is **one gameplay state**, created on the main thread but driven from
  worker threads, plus **several terrain-generation states** created on a
  worker thread.
- Script chunk names are paths like `$SURVIVAL_DATA/Scripts/game/SurvivalGame.lua`,
  with no `@` prefix.

## The sandbox

- A bootstrap chunk embedded in the exe builds **`unsafe_env`**: a whitelist of
  base functions, `class`, a curated `sm.*`, and parts of
  `string`/`table`/`math`/`bit`/`os`. A second variant exists for the terrain
  states (`sm.terrainTile`, `sm.terrainGeneration`, …). `class()` itself is
  also defined in embedded Lua.
- All game scripts share **one environment table**: a registry entry
  (`luaL_ref`) whose metatable is `{ __index = unsafe_env }`. It holds every
  game class, about 415 in Survival.
- The state's **raw globals** still contain the full libraries (`debug`,
  `ffi`, `os`, `package`). The global `type` is replaced by the game's own C
  function, which errors on some engine values.

## Callback sandbox

- A global script-manager object holds the current callback state: a flag for
  "inside a callback" and another for "server callback". `sm.*` functions check
  these flags and raise:
  - `Sandbox violation: callback while no sandbox is present.`
  - `Sandbox violation: calling %s function from %s callback.`
  - `Sandbox violation: mismatching scriptRef / instanceID.`
- The flags are set before C++ calls `lua_pcall` on a callback. That's why
  SMLoader's `QueueLuaIn` runs code right before a specific callback's `pcall`.
- World-dependent functions (`sm.physics.*`, `sm.body.getAllBodies`,
  `body:destroyCreation`, `sm.tool.checkLiftCollisionAtLevel`, …) also refuse
  to run from the game-mode script: *"Calling world dependent functions in a
  no world script!"*. Player, unit, interactable and world scripts can call
  them.
- The engine resolves callbacks **by name on every call**, so replacing a class
  method at runtime takes effect immediately.

## Creations

- `sm.creation.importFromString(world, json, pos, rot)` imports blueprints.
  Bodies with no `"type"` field import **static** (anchored). `"type": 0`
  means dynamic, which is what the game's own vehicle blueprints use.
- Body handles returned by an import go stale: the engine regroups bodies after
  spawning, and the Lift re-creates them when it moves a creation. Track
  creations by their shapes instead.
- `sm.player.placeLift` called from a script moves the player's Lift but won't
  attach script-imported bodies. The Lift tool only selects all-dynamic
  creations.

## Rendering

- The game uses D3D11 (`D3D11CreateDeviceAndSwapChain`).
- The Steam overlay hooks `IDXGISwapChain::Present` through the vtable, and
  re-hooks it if it changes. A second vtable hook that calls "whatever was
  there" ends up in a mutual-recursion stack overflow.
- Hooking `ResizeBuffers` crashed the game during back-buffer resizes.

## Input

- The game reads the mouse through Raw Input plus `SetCursorPos`/`ClipCursor`
  recentring, and polls keys with `GetAsyncKeyState`/`GetKeyState`, all
  imported from `USER32`. An overlay that wants the mouse has to neutralise
  those imports while it's open; `PatchImport` does this.
