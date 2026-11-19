# SMPlayerData

SMPlayerData is SMLoader's built-in system for **persistent player statistics
and progression**. It comes with SMLoader 1.2+, so there's nothing extra to
install. In every world, every machine running SMLoader gets a global Lua table,
`SMData`, in the game's script environment. Native (C++) mods use the `Data*`
functions in `smloader.h`.

What it gives you:

| | |
|---|---|
| **Automatic stats** | Playtime, sessions, distance on foot and in vehicles. In Survival also deaths, damage taken, robot kills (total and per robot type), damage dealt to robots, crops harvested and food eaten |
| **Mod data** | Per-player, per-mod key/value data, saved in the world. Private to the player unless you publish a key |
| **Progression** | XP with level curves, level-up events, and unlocks (achievements) |
| **Sync** | Each player always has their own full record. Everyone's stats and published keys are visible to all |
| **Leaderboards** | Sorted by any stat. On the host this includes players who are offline |

It builds on [SMNetworking](NETWORKING.md), which it uses for syncing.

## How it works

- **The host owns everything.** Each player's record is saved inside the world
  save, in the world's own script storage, keyed by player id (the engine keys
  player data by that id too). Progress is per world and travels with the save
  file. If the host doesn't have SMLoader 1.2+, nothing is tracked.
- **Saving** happens automatically every 10 s for changed records and when a
  player leaves. `SMData.saveNow()` saves immediately; it needs a server
  callback context, such as an `SMNet.onServerTick` listener.
- **Stats are observed, not wrapped.** SMPlayerData uses callback hooks and
  tick listeners, so no game function is replaced and other mods are never
  affected.
- **Sync.**
  - The host sends each player their own record when they join, then sends
    changes every 2 s.
  - Stats and published keys go out to everyone as SMNet shared state every 3 s.

## Quick start (Lua)

```lua
local MOD = "fishing"
SMData.setLevelCurve( MOD, { base = 50, growth = 1.4, max = 30 } )   -- on every machine
SMData.publish( MOD, "biggestFish" )                                 -- everyone may see this key

-- host: a player caught a fish
local function onCatch( player, weight )
    SMData.add( player, MOD, "caught", 1 )
    SMData.addStat( player, "fishing.caught", 1 )           -- shows up next to the built-in stats
    if weight > ( SMData.get( player, MOD, "biggestFish" ) or 0 ) then
        SMData.set( player, MOD, "biggestFish", weight )
    end
    local level, gained = SMData.addXP( player, MOD, 10 + weight )
    if SMData.get( player, MOD, "caught" ) == 100 then SMData.unlock( player, MOD, "hundred_fish" ) end
end

SMData.onLevelUp( MOD, function( player, level )
    SMNet.sendToClient( player, MOD, "levelUp", level )      -- tell them (SMNetworking)
end )

-- any machine: my own progress
local level, into, need = SMData.myLevel( MOD )
local caught = SMData.mine( MOD, "caught" ) or 0
local walked = SMData.myStat( "distanceWalked" )
```

## Built-in stats

| Stat | Meaning | Modes |
|---|---|---|
| `playtime` | Seconds in this world (game ticks, so pause time doesn't count) | all |
| `sessions` | Times the player joined this world | all |
| `firstSeen`, `lastSeen` | Unix time (`os.time()`) | all |
| `distanceWalked`, `distanceDriven` | Metres on foot and while seated (teleports are ignored) | all |
| `deaths` | Times knocked out (Survival) / downed | all |
| `damageTaken` | Health actually lost (after perks and armour) | Survival |
| `kills`, `kills.<RobotClass>` | Robots killed, e.g. `kills.TotebotGreenUnit` | Survival |
| `robotDamage` | Damage dealt to robots | Survival |
| `cropsHarvested`, `foodEaten` | | Survival |

Mods can add their own stats with `SMData.addStat(player, "mymod.thing", n)`.
Stats are always public and work with `SMData.leaderboard`.

## Lua API

`player` can be a `Player`, a player id, or `0` (the local player).

### Mod data (host writes)

| Function | Notes |
|---|---|
| `SMData.get(player, mod, key)` | Host: any player. Clients: their own data, or other players' published keys. With `key == nil` it returns the whole table |
| `SMData.set(player, mod, key, value)` | Host only. `value` can be anything `sm.storage` can save (numbers, strings, booleans, tables). `nil` deletes the key |
| `SMData.add(player, mod, key, n)` | Host only. Adds `n` (default 1) and returns the new value |
| `SMData.publish(mod, key)` | Lets every player see that key (call it on the host) |

### Stats

| Function | Notes |
|---|---|
| `SMData.stat(player, name)` | Any machine (all stats are public) |
| `SMData.addStat(player, name, n)` / `SMData.setStat(player, name, v)` | Host only |
| `SMData.leaderboard(stat, count)` | `{ {id, name, value}, ... }`, highest first. The host includes offline players |

### Progression

| Function | Notes |
|---|---|
| `SMData.setLevelCurve(mod, curve)` | `{ base = 100, growth = 1.5, max = 100 }`: the XP needed for the next level is `floor(base * level ^ growth)`. Or `{ fn = function(level) return xp end, max = 50 }`. Register it on every machine |
| `SMData.addXP(player, mod, amount)` | Host only. Returns `newLevel, levelsGained` |
| `SMData.xp(player, mod)`, `SMData.level(player, mod)` | `level` returns `level, xpIntoLevel, xpForNext` |
| `SMData.levelOf(mod, xp)` | The curve maths on its own |
| `SMData.unlock(player, mod, id)` | Host only. Returns `true` the first time |
| `SMData.isUnlocked(player, mod, id)`, `SMData.unlocks(player, mod)` | |

### Events (host)

`SMData.onChange(mod, fn(player, key, value))`, `SMData.onLevelUp(mod, fn(player, level, oldLevel))`,
`SMData.onUnlock(mod, fn(player, id))`

### Your own data (every machine)

| Function | Notes |
|---|---|
| `SMData.mine(mod, key)` | Own mod data (`key == nil` returns the table) |
| `SMData.myStat(name)`, `SMData.myStats()` | |
| `SMData.myXP(mod)`, `SMData.myLevel(mod)`, `SMData.myUnlocks(mod)` | |
| `SMData.onMine(fn(mod, key, value))` | Own data changed (`mod == nil`: full record received) |
| `SMData.isSynced()` | The host has sent this machine its record |
| `SMData.publicOf(playerId)` | `{ name, stats, pub = { [mod] = { key = value } } }` |

## Native (C++) API

Check `api->version >= 3` first.

```cpp
// host: reward a player
api->DataAdd(playerId, "mymod", "coins", 5);
api->DataSet(playerId, "mymod", "title", "\"Fisher\"");     // JSON value
api->DataAddXP(playerId, "mymod", 25);
api->DataUnlock(playerId, "mymod", "first_catch");

// any machine: read (JSON text). playerId 0 = the local player
char buf[64];
if (api->DataGet(0, "stats", "playtime", buf, sizeof buf) > 0) { double seconds = atof(buf); }
api->DataGet(0, "mymod", "@level", buf, sizeof buf);       // also "@xp", "@unlocks"
```

- **Writes** are queued for the game thread and only take effect on the host.
- **Reads** come from a cache SMPlayerData keeps up to date:
  - the host knows every online player;
  - other machines know their own data plus everyone's stats.
- `examples/DataDemo` is a complete native example.

## Limits and notes

- **Per world:** each world has its own progress, because data lives in the
  world save. Back up the world and you back up the progress.
- **Size:** keep each player's mod data reasonably small (a few KB). The whole
  record is rewritten when it changes.
- **Kill credit** goes to the last player who hit the robot within 3 s of its
  death.
- **Damage taken** counts every kind of health loss (robots, falls, drowning,
  starvation).
