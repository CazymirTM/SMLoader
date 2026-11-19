# SMNetworking

SMNetworking is SMLoader's toolkit for **multiplayer mods that stay in sync**.
It is built into SMLoader 1.1+, so nothing extra needs installing. In every
world, every machine running SMLoader gets a global Lua table, `SMNet`, in the
game's script environment. Native (C++) mods get the same features through the
`Net*` functions in `smloader.h`.

What it gives you:

| | |
|---|---|
| **Messages** | client → host, host → one player, host → everyone, and player → everyone (relayed by the host) |
| **Requests** | ask the host something and get a reply (with a timeout) |
| **Shared state** | host-owned key/value data per mod, mirrored to every player, including late joiners |
| **Roster** | who is in the world, who has SMLoader, and which mods (and versions) each player has |
| **"Everyone has my mod" check** | `SMNet.isReady(mod)` and `SMNet.missing(mod)`, so a mod can pause itself when someone lacks it |

## How it works (short version)

- **Transport.** SMNet adds two methods (`sv_smnet`, `cl_smnet`) to the
  game-mode class (`SurvivalGame`, `CreativeGame`, ...). It sends through the
  game's own `network` object, so messages use the normal multiplayer
  connection. It adds methods and never wraps existing ones.
- **Ticks.** SMLoader calls SMNet right before the game's
  `server_onFixedUpdate` / `client_onFixedUpdate` (a **callback hook**, see
  below), inside the right engine sandbox.
- **Batching.** Every send is queued and flushed on the next tick (40 per
  second) as one network call. That means you can call `SMNet.send*` from
  anywhere: game callbacks, loader jobs or native code.
- **Handshake.** Each client says hello to the host and lists its mods. The
  host only ever sends to clients that said hello, so **players without
  SMLoader are never sent anything they can't handle**. If the host itself
  doesn't have SMLoader 1.1+, clients give up after 30 s and
  `SMNet.isConnected()` stays false.

The host is also a client of its own world: messages the host sends to "all
players" reach the host's own client handlers too. That's why everything works
the same in single player.

## Quick start (Lua)

Run your mod's Lua in the `"game"` environment (`QueueLua(L, src, name, "game", ...)`).
`SMNet` exists there as soon as the world's game-mode script has loaded.

```lua
local MOD = "mychat"
SMNet.register( MOD, "1.0" )                   -- "this machine has mychat 1.0"

-- host: a player sent a chat line -> pass it to everyone who has the mod
SMNet.onServer( MOD, "say", function( text, player )
    if type( text ) ~= "string" or #text > 200 then return end   -- never trust clients
    SMNet.broadcast( MOD, "line", { from = player.name, text = text } )
end )

-- every player: show lines from the host
SMNet.onClient( MOD, "line", function( msg )
    sm.gui.chatMessage( "#66ccff[" .. msg.from .. "]#ffffff " .. msg.text )
end )

-- send one (from any machine)
SMNet.sendToServer( MOD, "say", "hello everyone" )
```

## Lua API

### Setup and handlers

| Function | Runs on | Notes |
|---|---|---|
| `SMNet.register(mod, version)` | any | Announce that this machine has `mod`. Required for the roster and `isReady`. |
| `SMNet.onServer(mod, name, fn(data, player))` | host | Messages sent with `sendToServer`. `player` is the sender, provided by the engine (trustworthy). |
| `SMNet.onClient(mod, name, fn(data, fromPlayer))` | every machine | Messages from the host (`fromPlayer == nil`) or relayed from a player (`sendToAll` / `sendToOthers`). |
| `SMNet.onRequest(mod, name, fn(data, player) -> reply)` | host | Answers `SMNet.request`. An error in `fn` is sent back as the error. |
| `SMNet.onShared(mod, fn(key, value))` | every machine | A shared value changed. `key == nil` means "full copy received" (after joining). |
| `SMNet.onRoster(fn(players))` | every machine | Someone joined, left, or changed their mods. |

Use the name `"*"` to catch every message of a mod: the handler then gets
`fn(name, data, from)`.

### Sending

| Function | From | To |
|---|---|---|
| `SMNet.sendToServer(mod, name, data)` | any machine | the host's `onServer` handler |
| `SMNet.sendToAll(mod, name, data)` | any machine | every player with the mod (the sender too), via the host |
| `SMNet.sendToOthers(mod, name, data)` | any machine | the same, minus the sender |
| `SMNet.request(mod, name, data, cb(reply, err), timeout?)` | any machine | the host's `onRequest` handler. `cb` gets the reply, or `nil` plus an error such as `"timeout"` (default 10 s). |
| `SMNet.sendToClient(player, mod, name, data)` | host | one player (a `Player` or a player id) |
| `SMNet.broadcast(mod, name, data)` | host | every player with the mod |

`data` can be anything the game can send over the network: numbers, strings,
booleans, tables, and game values such as `Vec3`, `Quat`, `Color`, `Uuid` and
`Player`. Keep messages small (a few KB).
Players who don't have the mod never receive its messages.

### Shared state

```lua
-- host
SMNet.setShared( "arena", "round", 3 )
SMNet.setShared( "arena", "scores", { alice = 12, bob = 9 } )
SMNet.setShared( "arena", "round", nil )          -- delete

-- anyone
local round = SMNet.getShared( "arena", "round" )
local all = SMNet.getShared( "arena" )            -- the whole table (read-only)
SMNet.onShared( "arena", function( key, value ) ... end )
```

Changes are batched and sent on the next tick. Shared state lasts for the
session only; save anything permanent with `sm.storage` on the host.

### Status and roster

| Function | Returns |
|---|---|
| `SMNet.isHost()` | `true` on the machine hosting the world |
| `SMNet.isConnected()` | `true` once the host has answered this machine's hello |
| `SMNet.localPlayerId()` | this machine's player id |
| `SMNet.players()` | `{ { id, name, smnet, mods = { [mod] = version } }, ... }` |
| `SMNet.hasMod(player, mod)` | `true, version` or `false` |
| `SMNet.isReady(mod)` | `true` when every player in the world has announced `mod` |
| `SMNet.missing(mod)` | names of players known not to have `mod` |

Joining players get 25 s to say hello. Until then they count as "not ready"
but are not listed as missing.

### Ticks and hooks (Lua)

| Function | Notes |
|---|---|
| `SMNet.onServerTick(fn(game))` | Every host tick (40/s), inside the game-mode `server_onFixedUpdate` sandbox |
| `SMNet.onClientTick(fn(game))` | Every client tick on every machine, inside `client_onFixedUpdate` |
| `SMNet.addCallbackHook(callback, handler)` | Lua version of `AddCallbackHook` for this world, e.g. `("SurvivalPlayer.server_onFixedUpdate", "MyMod.playerTick")` |

None of these wrap game functions.

### JSON

`SMNet.json.encode(value)` and `SMNet.json.decode(text)` are a small JSON
encoder and decoder. When a Lua table is sent to a native mod, it arrives as
JSON.

## Native (C++) API

Check `api->version >= 2` first. The functions below are thread-safe and can
be called from `SMMod_Init`.

```cpp
api->NetRegister("mymod", "1.0");
api->NetOn("mymod", "ping", SMNET_SIDE_SERVER, OnPing, nullptr);   // host side
api->NetOn("mymod", "*",    SMNET_SIDE_CLIENT, OnAny,  nullptr);   // every machine

static void OnPing(const char* mod, const char* msg, const char* data, size_t len,
                   int fromPlayerId, int side, void* user) {
    api->NetSend("mymod", "pong", fromPlayerId, data, len);       // host -> that player
}

api->NetSend("mymod", "ping", SMNET_TO_SERVER, "{\"n\":1}", 7);

SMNetStatus st;
if (api->NetGetStatus("mymod", &st) && st.connected && st.ready) { /* everyone has it */ }
```

- **Targets:** `SMNET_TO_SERVER`, `SMNET_TO_ALL`, `SMNET_TO_OTHERS`,
  `SMNET_BROADCAST` (host only), or a player id (host only).
- **Payloads** are byte strings. Strings from Lua arrive unchanged, and Lua
  tables arrive JSON-encoded. Native and Lua code can talk to each other with
  the same mod id.
- **Handlers** run on the game thread inside the network callback, and `data`
  is valid only during the call. `fromPlayerId` is 0 when the host's server
  side sent the message.

`examples/NetDemo` is a complete native example.

## Callback hooks (`AddCallbackHook`)

SMNet's ticks use a general loader feature that any mod can use:

```cpp
api->AddCallbackHook("SurvivalGame.server_onFixedUpdate", "MyMod.onServerTick");
```

From then on, every time the game is about to call `SurvivalGame.server_onFixedUpdate`, SMLoader first
calls the Lua function `MyMod.onServerTick` (looked up in the game env) with
the same arguments (`self, timeStep`), inside the same engine sandbox.

- **Why use this instead of wrapping (`SurvivalGame.server_onFixedUpdate = function(...)`):**
  nothing is replaced, so any number of mods can hook the same callback without
  interfering with each other, and the game's own hot reload can't remove the
  hook.
- **Re-resolving:** both expressions are re-evaluated every 2 s, which follows
  hot reloads.
- **Errors:** they are logged to `loader.log`, at most once every 5 s per hook.

## Security

- The **host is the authority.** Validate everything in `onServer` and
  `onRequest`: clients can send anything.
- `player` in server handlers comes from the engine, so it can't be faked by
  the sender. Relayed messages carry the original sender's id.
- Only the host can call `broadcast`, `sendToClient` and `setShared`.

## Limits and notes

- **Message size:** keep messages to a few KB. The game's own limit on network
  messages hasn't been measured. Split anything large across several messages.
- **Game modes:** it works in Survival, Creative and Challenge (any game-mode
  class SMNet finds). It does nothing on the main menu.
- **Timing:** messages arrive in order per sender. They are flushed once per
  game tick (about 25 ms), plus network latency.
