# SMEvents

SMEvents is one **event bus shared by Lua mods, native (C++) mods and the SDK
itself**. It is built into SMLoader 1.2+:
- Lua mods use the global `SMEvents` table.
- Native mods use `EventOn` / `EventOff` / `EventEmit` / `EventEmitTo`.
- An event emitted on one side reaches listeners on both sides.

| | |
|---|---|
| **Publish / subscribe** | `on`, `once`, `off`, `emit`, with priorities and `*` patterns |
| **Cross-language** | Lua tables arrive in C++ as JSON; C++ JSON arrives in Lua as tables |
| **Networked** | `emitTo("server" / "all" / "others" / "clients" / player, ...)` over [SMNetworking](NETWORKING.md) |
| **Built-in events** | World, connection and player events from the SDK, plus gameplay events from [SMPlayerData](PLAYERDATA.md) |

## Quick start

Lua:
```lua
-- react to SDK events
SMEvents.on( "robot.killed", function( e )
    print( "player " .. e.id .. " destroyed a " .. e.robot )
end )

-- your own events: name them "<mod>.<what>" (or "<mod>:<what>")
SMEvents.on( "mymod.*", function( data, ev ) print( ev.name, data ) end )
SMEvents.emit( "mymod.opened", { page = "shop" } )

-- tell everyone in the world
SMEvents.emitTo( "all", "mymod.announce", { text = "Boss spawned!" } )
```

C++:
```cpp
static void OnKill(const char* name, const char* json, size_t len, int from, int remote, void* user) {
    api->Log("%s: %.*s", name, (int)len, json);        // robot.killed: {"robot":"HaybotUnit","id":1}
}
api->EventOn("robot.killed", OnKill, nullptr);
api->EventEmit("mymod.opened", "{\"page\":\"shop\"}");             // Lua listeners get { page = "shop" }
api->EventEmitTo(SMNET_TO_ALL, "mymod.announce", "{\"text\":\"Boss spawned!\"}");
```

## Built-in events

| Event | Data | Where |
|---|---|---|
| `world.loaded` | `{ host }` | every machine, when SMEvents starts in a world |
| `world.closed` | none | **native listeners only** (Lua is shutting down), when the world's Lua state closes |
| `net.connected` | `{ host, id }` | every machine, when the host answers SMNetworking's handshake |
| `player.joined` | `{ id, name, initial? }` | every machine. `initial = true` for players already present when this machine joined |
| `player.left` | `{ id, name }` | every machine |
| `player.death` | `{ id }` | host |
| `player.damaged` | `{ id, amount, hp }` | host (Survival) |
| `robot.killed` | `{ id, robot }` | host (Survival). `robot` is the class, e.g. `TotebotGreenUnit`; `id` is the killer |
| `crop.harvested`, `food.eaten` | `{ id }` | host (Survival) |
| `data.levelUp` | `{ id, mod, level, old }` | host |
| `data.unlock` | `{ id, mod, unlock }` | host |

`id` is always a player id. Host events can be forwarded to players with
`emitTo`.

## Lua API

| Function | Notes |
|---|---|
| `SMEvents.on(name, fn(data, ev), opts?)` | Returns a handle. `opts.priority`: higher runs first (default 0); `opts.once` |
| `SMEvents.once(name, fn, opts?)` | Runs at most once |
| `SMEvents.off(handleOrName)` | Removes one listener (by handle) or every listener of a name |
| `SMEvents.emit(name, data)` | Emits on this machine. Returns the number of listeners called |
| `SMEvents.emitTo(target, name, data)` | Sends over the network. Targets: `"server"` (the host), `"all"` (everyone, the sender too), `"others"` (everyone else), and host-only `"clients"` or a player / player id |
| `SMEvents.list()` | `{ [name] = count }` of Lua listeners (for debugging) |

`ev` = `{ name, remote, from, source }`:
- `remote` is `true` for networked events.
- `from` is the sender's player id, or `nil` for the host's server side and for
  local events.
- `source` is `"lua"`, `"native"` or `"network"`.

**Patterns:** a name ending in `*` matches every event that starts with the
part before it, so `"player.*"`, `"mymod:*"` or just `"*"`.

**Rules:**
- **Listeners run in priority order, then registration order.**
- **Errors are contained.** A listener that errors is logged and the others
  still run.
- **Nesting:** listeners may emit events themselves, up to 16 levels deep.
- **Data is passed by reference** between Lua listeners on the same machine, so
  don't modify a table you receive unless that's the intent.

## Native API

```c
typedef void (*SMEventFn)(const char* name, const char* json, size_t len,
                          int fromPlayerId, int remote, void* user);
int  EventOn(const char* name, SMEventFn fn, void* user);   // returns an id for EventOff
void EventOff(int listenerId);
int  EventEmit(const char* name, const char* json);         // json may be NULL
int  EventEmitTo(int target, const char* name, const char* json);
```

- **Threads:** a listener runs on the thread that emitted the event. Events
  from Lua, the SDK and the network arrive on the game thread. `EventEmit`
  calls native listeners immediately on your thread and queues Lua listeners
  for the game thread.
- **Without a world:** `EventEmit` still reaches native listeners, which makes
  it handy for mod-to-mod messages at the main menu. `EventEmitTo` needs a
  world.
- **Targets** for `EventEmitTo` are `SMNET_TO_SERVER`, `SMNET_TO_ALL`,
  `SMNET_TO_OTHERS`, `SMNET_BROADCAST`, or a player id (host only).
- **JSON** is only built for Lua events when a native listener matches, so
  chatty events cost nothing when no C++ code is listening.

`examples/EventDemo` is a complete native example. It logs SDK events, greets
everyone over the network, and answers `eventdemo.ping` with `eventdemo.pong`
(so you can test Lua → C++ → Lua).

## SMEvents vs SMNet messages

Use **SMEvents** for "something happened" notifications that anyone (Lua, C++,
other mods) might care about. Use **SMNet messages** for a mod's own
request/response protocol between its client and host parts. Networked events
use SMNet under the hood.
