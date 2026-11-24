# SMAudio: your own sounds (API v4, SMLoader 1.5.0)

Plays a mod's sound files through the game's own sound engine (FMOD): 3D
position, Doppler, and the player's volume sliders all work.

## Register (C++)

```c
int AudioAdd(const char* name, const void* data, size_t len);   // from SMMod_Init
```
- `.ogg`, `.wav` or `.mp3`, up to 64 MB; the data is copied. Use mono for 3D sounds.
- Use a name nobody else has: `"mymod.shot"`.

## Play (Lua, client side)

```lua
local h = SMAudio.play( "mymod.engine", { position = pos, loop = true, maxDistance = 80 } )
SMAudio.set( h, { position = newPos, velocity = vel, pitch = 1.1 } )
SMAudio.stop( h, 0.3 )                       -- fades out over 0.3 s
SMAudio.play( "mymod.click", { volume = 0.6 } )          -- 2D: no position
```

| Option | Default | |
|---|---|---|
| `position`, `velocity` | none, 0 | `sm.vec3`; without a position the sound is 2D |
| `volume`, `pitch` | 1, 1 | |
| `loop` | false | stop it yourself |
| `minDistance`, `maxDistance` | 4, 200 | metres: full volume up to min, silent at max |
| `delay` | 0 | seconds |
| `doppler` | 1 | 0 = off |
| `bus` | `"bus:/Effect"` | one of the game's mixer buses |

- `play` returns a handle, or `nil` if the sound doesn't exist or too many (96)
  are playing.
- `SMAudio.isPlaying( h )`, `SMAudio.exists( name )`, `SMAudio.listener()`.
- Each machine plays its own sounds: to make everyone hear one, send a message
  (`self.network:sendToClients`, or SMNet) and play it in the client callback.
- Everything stops when the world closes.
