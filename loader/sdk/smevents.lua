-- SMEvents - one event bus shared by Lua mods, native (C++) mods and the SDK.
--
-- SMLoader compiles this file into winmm.dll and installs it after
-- SMNetworking (and before SMPlayerData) in every world. It defines the global
-- table `SMEvents`. Full guide: docs/EVENTS.md.
--
--  Local      SMEvents.emit(name, data) calls every matching listener on this
--             machine: Lua listeners right away, native listeners through the
--             loader (data arrives there as JSON). Native EventEmit reaches
--             Lua listeners here (JSON arrives as a table).
--  Patterns   A listener name ending in "*" matches every event starting with
--             the part before it: "player.*", "mymod:*", "*".
--  Network    SMEvents.emitTo(target, name, data) sends the event over
--             SMNetworking; it is emitted on the receiving machine(s) with
--             ev.remote = true and ev.from = the sender's player id.
--  Built-in   world.loaded, net.connected, player.joined, player.left (here),
--             player.death, player.damaged, robot.killed, crop.harvested,
--             food.eaten, data.levelUp, data.unlock (SMPlayerData), and
--             world.closed (native listeners only: emitted by the loader).

local E = ...
if SMEvents and SMEvents.__env == E then return "already installed" end
if not SMNet then error( "SMEvents needs SMNetworking", 0 ) end
local NET = SMNet

local V = {}
SMEvents = V
V.__env = E
V.VERSION = "1.2.0"
V.MOD = "smevents"
V.MAX_DEPTH = 16                       -- events emitted from listeners, nested

local function ty( v ) local ok, r = pcall( type, v ) return ok and r or "?" end
local function log( msg ) print( "[SMEvents] " .. tostring( msg ) ) end
local function native( ... )
	local f = NET.__native
	if f then local ok, err = pcall( f, ... ) if not ok then log( err ) end end
end

local listeners = {}                   -- { { pattern, prefix|nil, fn, priority, once, id }, ... } sorted
local nextId = 1
local depth = 0
V.__native = {}                        -- [pattern] = count of native listeners (kept by the loader)

local function matches( pattern, prefix, name )
	if prefix then return name:sub( 1, #prefix ) == prefix end
	return pattern == name
end
local function prefixOf( pattern )
	if pattern:sub( -1 ) == "*" then return pattern:sub( 1, -2 ) end
	return nil
end

-- ---------------------------------------------------------------------------
-- Public API
-- ---------------------------------------------------------------------------
-- fn( data, ev ): ev = { name, from = playerId|nil, remote = bool, source = "lua"|"native"|"network" }
-- opts: { priority = number (higher runs first, default 0), once = bool }
-- Returns a handle for SMEvents.off.
function V.on( name, fn, opts )
	if ty( name ) ~= "string" or name == "" then error( "SMEvents.on: event name must be a non-empty string", 2 ) end
	if ty( fn ) ~= "function" then error( "SMEvents.on: listener must be a function", 2 ) end
	opts = ty( opts ) == "table" and opts or {}
	local l = { pattern = name, prefix = prefixOf( name ), fn = fn, priority = tonumber( opts.priority ) or 0,
	            once = opts.once and true or false, id = nextId }
	nextId = nextId + 1
	listeners[#listeners + 1] = l
	table.sort( listeners, function( a, b )
		if a.priority ~= b.priority then return a.priority > b.priority end
		return a.id < b.id
	end )
	return l.id
end

function V.once( name, fn, opts )
	opts = ty( opts ) == "table" and opts or {}
	opts.once = true
	return V.on( name, fn, opts )
end

-- Remove a listener by handle, or every listener of a pattern with
-- SMEvents.off( "player.joined" ). Returns how many were removed.
function V.off( handleOrName )
	local n = 0
	for i = #listeners, 1, -1 do
		local l = listeners[i]
		if l.id == handleOrName or l.pattern == handleOrName then table.remove( listeners, i ) n = n + 1 end
	end
	return n
end

local function nativeWants( name )
	for pattern, count in pairs( V.__native ) do
		if count > 0 and matches( pattern, prefixOf( pattern ), name ) then return true end
	end
	return false
end

local function jsonOf( v )
	if v == nil then return "null" end
	local ok, s = pcall( NET.json.encode, v )
	return ok and s or "null"
end

-- Dispatch to Lua listeners (and native ones unless skipNative). Returns the
-- number of listeners called.
local function dispatch( name, data, ev, skipNative )
	if depth >= V.MAX_DEPTH then log( "event '" .. name .. "' dropped: emitted " .. V.MAX_DEPTH .. " levels deep" ) return 0 end
	depth = depth + 1
	local count = 0
	local snapshot = {}
	for i, l in ipairs( listeners ) do snapshot[i] = l end
	for _, l in ipairs( snapshot ) do
		if matches( l.pattern, l.prefix, name ) then
			if l.once then V.off( l.id ) end
			count = count + 1
			local ok, err = pcall( l.fn, data, ev )
			if not ok then log( "listener for '" .. l.pattern .. "' failed on '" .. name .. "': " .. tostring( err ) ) end
		end
	end
	if not skipNative and nativeWants( name ) then
		native( "event", name, ty( data ) == "string" and data or jsonOf( data ), ev.from or 0, ev.remote and 1 or 0 )
		count = count + 1
	end
	depth = depth - 1
	return count
end

-- Emit on this machine. Returns the number of listeners called.
function V.emit( name, data )
	if ty( name ) ~= "string" or name == "" then error( "SMEvents.emit: event name must be a non-empty string", 2 ) end
	return dispatch( name, data, { name = name, remote = false, source = "lua" } )
end

-- Emit over the network (SMNetworking). target:
--   "server"  -> the host
--   "all"     -> every player with SMLoader (sender included), via the host
--   "others"  -> the same without the sender
--   "clients" -> host only: every player
--   a Player or player id -> host only: that player
function V.emitTo( target, name, data )
	if ty( name ) ~= "string" or name == "" then error( "SMEvents.emitTo: event name must be a non-empty string", 2 ) end
	local pkt = { n = name, d = data }
	if target == "server" then return NET.sendToServer( V.MOD, "e", pkt )
	elseif target == "all" then return NET.sendToAll( V.MOD, "e", pkt )
	elseif target == "others" then return NET.sendToOthers( V.MOD, "e", pkt )
	elseif target == "clients" then return NET.broadcast( V.MOD, "e", pkt ) end
	return NET.sendToClient( target, V.MOD, "e", pkt )
end

-- Debug: { pattern = listenerCount, ... } (Lua listeners; native in V.__native)
function V.list()
	local t = {}
	for _, l in ipairs( listeners ) do t[l.pattern] = ( t[l.pattern] or 0 ) + 1 end
	return t
end

-- ---------------------------------------------------------------------------
-- Network receive
-- ---------------------------------------------------------------------------
local function pidOf( p ) local ok, id = pcall( function() return p.id end ) return ok and id or nil end
local function receive( pkt, from )
	if ty( pkt ) ~= "table" or ty( pkt.n ) ~= "string" then return end
	dispatch( pkt.n, pkt.d, { name = pkt.n, from = from and pidOf( from ) or nil, remote = true, source = "network" } )
end
NET.register( V.MOD, V.VERSION )
NET.onServer( V.MOD, "e", function( pkt, player ) receive( pkt, player ) end )
NET.onClient( V.MOD, "e", function( pkt, fromPlayer ) receive( pkt, fromPlayer ) end )

-- ---------------------------------------------------------------------------
-- Built-in events from SMNetworking state
-- ---------------------------------------------------------------------------
local known = nil                      -- [id] = name, from the last roster
NET.onRoster( function( players )
	local now = {}
	for _, p in ipairs( players or {} ) do now[p.id] = p.name end
	if known then
		for id, name in pairs( now ) do if not known[id] then V.emit( "player.joined", { id = id, name = name } ) end end
		for id, name in pairs( known ) do if not now[id] then V.emit( "player.left", { id = id, name = name } ) end end
	else
		for id, name in pairs( now ) do V.emit( "player.joined", { id = id, name = name, initial = true } ) end
	end
	known = now
end )

local wasConnected, loadedSent = false, false
NET.onClientTick( function()
	if not loadedSent then
		loadedSent = true
		V.emit( "world.loaded", { host = NET.isHost() } )
	end
	local c = NET.isConnected()
	if c and not wasConnected then V.emit( "net.connected", { host = NET.isHost(), id = NET.localPlayerId() } ) end
	wasConnected = c
end )

-- ---------------------------------------------------------------------------
-- Native (C++) bridge - called by SMLoader
-- ---------------------------------------------------------------------------
local function decode( json )
	if json == nil or json == "" or json == "null" then return nil end
	local ok, v = pcall( NET.json.decode, json )
	if ok then return v end
	return json
end
-- A native mod emitted locally: native listeners already ran; run Lua ones.
function V.__fromNative( name, json )
	dispatch( name, decode( json ), { name = name, remote = false, source = "native" }, true )
end
function V.__emitTo( target, name, json )
	local t = ( { [-1] = "server", [-2] = "all", [-3] = "others", [-4] = "clients" } )[target] or target
	return V.emitTo( t, name, decode( json ) )
end
function V.__setNative( pattern, count ) V.__native[pattern] = count > 0 and count or nil end

return "installed"
