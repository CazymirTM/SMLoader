-- SMAudio - mods' own sounds (global `SMAudio`), played by the game's sound engine.
--
-- SMLoader compiles this file into winmm.dll and installs it in every world.
-- Sounds are registered by native mods (api->AudioAdd). Client side only: each
-- player's machine plays its own sounds. Full guide: docs/AUDIO.md.

local E, NX = ...                     -- game env, natives table from the loader
if SMAudio and SMAudio.__env == E then return "already installed" end

local A = {}
SMAudio = A
A.__env = E
A.VERSION = "1.5.0"

-- Play a sound. Returns a handle for set/stop, or nil if it can't play.
--   position     sm.vec3 (3D); without it the sound is 2D, at the same volume everywhere
--   velocity     sm.vec3, for the Doppler effect
--   volume 1, pitch 1, loop false, delay 0 (seconds), doppler 1
--   minDistance 4, maxDistance 200 (metres): full volume up to min, silent at max
--   bus          the game's mixer bus, default "bus:/Effect" (the effects slider)
function A.play( name, o )
	o = o or {}
	local p, v = o.position, o.velocity
	return NX.play( name, p ~= nil,
		p and p.x or 0, p and p.y or 0, p and p.z or 0,
		v and v.x or 0, v and v.y or 0, v and v.z or 0,
		o.volume or 1, o.pitch or 1, o.loop == true,
		o.minDistance or 4, o.maxDistance or 200, o.bus or "bus:/Effect",
		( o.delay or 0 ) * 1000, o.doppler or 1 )
end

-- Change a playing sound: { position =, velocity =, volume =, pitch = }.
-- Returns true while it is playing.
function A.set( handle, o )
	if not handle then return false end
	local p, v = o.position, o.velocity
	return NX.set( handle, p and p.x, p and p.y, p and p.z,
		v and v.x or 0, v and v.y or 0, v and v.z or 0, o.volume, o.pitch ) == true
end

-- Stop a sound, at once or faded out over `fade` seconds.
function A.stop( handle, fade )
	if handle then NX.stop( handle, ( fade or 0 ) * 1000 ) end
end

function A.isPlaying( handle )
	return handle ~= nil and NX.playing( handle ) == true
end

-- Is a sound with this name registered on this machine?
function A.exists( name )
	return NX.exists( name ) == true
end

-- Where the player's ears are: position, forward direction.
function A.listener()
	local x, y, z, fx, fy, fz = NX.listener()
	return sm.vec3.new( x, y, z ), sm.vec3.new( fx, fy, fz )
end

return "installed"
