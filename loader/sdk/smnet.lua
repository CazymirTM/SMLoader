-- SMNetworking - synchronized multiplayer utilities for SMLoader mods.
--
-- SMLoader compiles this file into winmm.dll and runs it inside the shared
-- game-script environment of every world (once the game-mode class such as
-- SurvivalGame exists), on every machine that has SMLoader 1.1+. It defines the
-- global table `SMNet`. Full guide: docs/NETWORKING.md.
--
-- How it works
--   Transport  Two methods are added to the game-mode class (sv_smnet and
--              cl_smnet) and called through the game's own network object, so
--              messages ride the normal multiplayer connection. Methods are
--              added, never wrapped: other mods' hooks are not disturbed.
--   Ticks      SMLoader calls SMNet.__svTick / SMNet.__clTick right before the
--              game's server_onFixedUpdate / client_onFixedUpdate (a loader
--              "callback hook"), inside the right engine sandbox.
--   Outboxes   Every send is queued and flushed on the next tick as one batch,
--              so sends work from anywhere (game callbacks, loader jobs,
--              native mods) and cost one network call per tick.
--   Handshake  Each client says hello to the host, listing its SMNet mods. The
--              host answers only clients that said hello, so players without
--              SMLoader are never sent calls they cannot handle. A client
--              stops trying after 30 s if the host doesn't answer (host
--              without SMLoader 1.1+).

local E = ...                          -- the game env (passed by the loader)
if SMNet and SMNet.__env == E then return "already installed" end

local N = {}
SMNet = N
N.__env = E
N.VERSION = "1.2.0"
N.PROTOCOL = 1
N.HELLO_GRACE = 25                     -- seconds a joining player has to say hello
N.HELLO_GIVE_UP = 30                   -- seconds a client keeps trying to reach the host
N.MAX_QUEUE = 512                      -- packets held while not connected

local SV_CB, CL_CB = "sv_smnet", "cl_smnet"
local SIDE_SERVER, SIDE_CLIENT = 1, 2
local TO_SERVER, TO_ALL, TO_OTHERS, BROADCAST = -1, -2, -3, -4

local function ty( v ) local ok, r = pcall( type, v ) return ok and r or "?" end
local function now() return os.clock() end
local function log( msg ) print( "[SMNet] " .. tostring( msg ) ) end
local function pidOf( p )
	if ty( p ) == "number" then return p end
	local ok, id = pcall( function() return p.id end )
	return ok and id or nil
end
local function safeCall( f, ... )
	if not f then return end
	local ok, err = pcall( f, ... )
	if not ok then log( err ) end
end
local function slot( t, k ) local s = t[k] if not s then s = {} t[k] = s end return s end
local function native( ... )
	local f = N.__native
	if f then local ok, err = pcall( f, ... ) if not ok then log( err ) end end
end

-- Registered on this machine ---------------------------------------------------
N.mods = {}                            -- [mod] = version string
N.handlers = { [SIDE_SERVER] = {}, [SIDE_CLIENT] = {} }   -- [side][mod][name] = fn
N.requestHandlers = {}                 -- [mod][name] = fn( data, player ) -> reply
N.sharedListeners = {}                 -- [mod] = { fn( key, value ), ... }
N.rosterListeners = {}                 -- { fn( players ), ... }
N.nativeMods = {}                      -- [mod] = true: also deliver to native (C++) handlers
N.serverTickers = {}                   -- { fn( game ), ... } run every host tick
N.clientTickers = {}                   -- { fn( game ), ... } run every client tick

-- Client state (every machine, including the host)
local C = { start = nil, helloAt = -100, welcomed = false, gaveUp = false, out = {},
            pending = {}, nextReq = 1, shared = {}, roster = nil, localId = nil,
            modsDirty = false, statusAt = 0 }
-- Server state (host only)
local S = { players = {}, out = {}, shared = {}, dirty = {}, roster = nil, rosterSig = nil, scanAt = -100 }
N.__c, N.__s = C, S

-- ---------------------------------------------------------------------------
-- JSON (used for native mods; also handy for Lua mods): SMNet.json
-- ---------------------------------------------------------------------------
local json = {}
N.json = json
local ESC = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
local function encStr( s )
	return '"' .. ( s:gsub( '[%c"\\]', function( c ) return ESC[c] or string.format( "\\u%04x", c:byte() ) end ) ) .. '"'
end
local function isArray( t )
	local n = 0
	for k in pairs( t ) do
		if ty( k ) ~= "number" or k <= 0 or k % 1 ~= 0 then return false end
		n = n + 1
	end
	for i = 1, n do if t[i] == nil then return false end end
	return n > 0, n
end
function json.encode( v, depth )
	depth = depth or 0
	if depth > 32 then error( "SMNet.json.encode: nested too deeply", 0 ) end
	local t = ty( v )
	if t == "nil" then return "null"
	elseif t == "boolean" then return v and "true" or "false"
	elseif t == "number" then
		if v ~= v or v == math.huge or v == -math.huge then return "null" end
		if v % 1 == 0 and v > -1e15 and v < 1e15 then return string.format( "%d", v ) end
		return string.format( "%.14g", v )
	elseif t == "string" then return encStr( v )
	elseif t == "table" then
		local out = {}
		local arr, n = isArray( v )
		if arr then
			for i = 1, n do out[i] = json.encode( v[i], depth + 1 ) end
			return "[" .. table.concat( out, "," ) .. "]"
		end
		for k, x in pairs( v ) do out[#out + 1] = encStr( tostring( k ) ) .. ":" .. json.encode( x, depth + 1 ) end
		return "{" .. table.concat( out, "," ) .. "}"
	elseif t == "Vec3" then
		return string.format( '{"x":%.9g,"y":%.9g,"z":%.9g}', v.x, v.y, v.z )
	elseif t == "Player" then
		return tostring( pidOf( v ) or 0 )
	end
	return encStr( tostring( v ) )      -- other game userdata: the game's own text form
end

local function utf8char( c )
	if c < 0x80 then return string.char( c ) end
	if c < 0x800 then return string.char( 0xC0 + math.floor( c / 0x40 ), 0x80 + c % 0x40 ) end
	if c < 0x10000 then return string.char( 0xE0 + math.floor( c / 0x1000 ), 0x80 + math.floor( c / 0x40 ) % 0x40, 0x80 + c % 0x40 ) end
	return string.char( 0xF0 + math.floor( c / 0x40000 ), 0x80 + math.floor( c / 0x1000 ) % 0x40,
	                    0x80 + math.floor( c / 0x40 ) % 0x40, 0x80 + c % 0x40 )
end
local UNESC = { b = "\b", f = "\f", n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
function json.decode( s )
	if ty( s ) ~= "string" then error( "SMNet.json.decode: expected a string", 0 ) end
	local pos = 1
	local function fail( m ) error( "SMNet.json.decode: " .. m .. " at character " .. pos, 0 ) end
	local function ws() pos = s:find( "[^ \t\r\n]", pos ) or #s + 1 end
	local value
	local function str()
		pos = pos + 1
		local buf = {}
		while true do
			local c = s:sub( pos, pos )
			if c == "" then fail( "unterminated string" )
			elseif c == '"' then pos = pos + 1 return table.concat( buf )
			elseif c == "\\" then
				local e = s:sub( pos + 1, pos + 1 )
				if UNESC[e] then buf[#buf + 1] = UNESC[e] pos = pos + 2
				elseif e == "u" then
					local h = tonumber( s:sub( pos + 2, pos + 5 ), 16 ) or fail( "bad \\u escape" )
					pos = pos + 6
					if h >= 0xD800 and h <= 0xDBFF and s:sub( pos, pos + 1 ) == "\\u" then
						local lo = tonumber( s:sub( pos + 2, pos + 5 ), 16 )
						if lo and lo >= 0xDC00 and lo <= 0xDFFF then h = 0x10000 + ( h - 0xD800 ) * 0x400 + ( lo - 0xDC00 ) pos = pos + 6 end
					end
					buf[#buf + 1] = utf8char( h )
				else fail( "bad escape" ) end
			else
				local j = s:find( '["\\]', pos ) or #s + 1
				buf[#buf + 1] = s:sub( pos, j - 1 )
				pos = j
			end
		end
	end
	value = function( depth )
		if depth > 64 then fail( "nested too deeply" ) end
		ws()
		local c = s:sub( pos, pos )
		if c == "{" then
			local t = {}
			pos = pos + 1 ws()
			if s:sub( pos, pos ) == "}" then pos = pos + 1 return t end
			while true do
				ws()
				if s:sub( pos, pos ) ~= '"' then fail( "expected a key" ) end
				local k = str()
				ws()
				if s:sub( pos, pos ) ~= ":" then fail( "expected ':'" ) end
				pos = pos + 1
				t[k] = value( depth + 1 )
				ws()
				local d = s:sub( pos, pos )
				pos = pos + 1
				if d == "}" then return t elseif d ~= "," then fail( "expected ',' or '}'" ) end
			end
		elseif c == "[" then
			local t = {}
			pos = pos + 1 ws()
			if s:sub( pos, pos ) == "]" then pos = pos + 1 return t end
			while true do
				t[#t + 1] = value( depth + 1 )
				ws()
				local d = s:sub( pos, pos )
				pos = pos + 1
				if d == "]" then return t elseif d ~= "," then fail( "expected ',' or ']'" ) end
			end
		elseif c == '"' then return str()
		elseif s:sub( pos, pos + 3 ) == "true" then pos = pos + 4 return true
		elseif s:sub( pos, pos + 4 ) == "false" then pos = pos + 5 return false
		elseif s:sub( pos, pos + 3 ) == "null" then pos = pos + 4 return nil
		else
			local num = s:match( "^-?%d+%.?%d*[eE]?[-+]?%d*", pos )
			if not num or num == "" then fail( "unexpected '" .. c .. "'" ) end
			pos = pos + #num
			return tonumber( num ) or fail( "bad number" )
		end
	end
	local v = value( 0 )
	ws()
	if pos <= #s then fail( "trailing characters" ) end
	return v
end

-- ---------------------------------------------------------------------------
-- Public API
-- ---------------------------------------------------------------------------

-- Declare that this machine has mod `mod`. Every player's list of mods is sent
-- to the host, which builds the roster (SMNet.players / SMNet.isReady).
function N.register( mod, version )
	if ty( mod ) ~= "string" or mod == "" then error( "SMNet.register: mod id must be a non-empty string", 2 ) end
	N.mods[mod] = tostring( version or "1" )
	C.modsDirty = true
	return true
end

-- Handlers. Server handlers run on the host: fn( data, player ).
-- Client handlers run on every machine: fn( data, fromPlayer ), where
-- fromPlayer is the player who sent it (relayed messages) or nil (the host).
function N.onServer( mod, name, fn ) slot( N.handlers[SIDE_SERVER], mod )[name] = fn end
function N.onClient( mod, name, fn ) slot( N.handlers[SIDE_CLIENT], mod )[name] = fn end
-- Request handlers run on the host and return the reply: fn( data, player ) -> reply
function N.onRequest( mod, name, fn ) slot( N.requestHandlers, mod )[name] = fn end
-- Shared state listener (every machine): fn( key, value ); key == nil means
-- "everything was replaced" (first sync after joining).
function N.onShared( mod, fn ) local s = slot( N.sharedListeners, mod ) s[#s + 1] = fn end
-- Roster listener (every machine): fn( players ) whenever someone joins, leaves
-- or changes their mods.
function N.onRoster( fn ) N.rosterListeners[#N.rosterListeners + 1] = fn end
-- Per-tick callbacks without wrapping any game function. Server: host only,
-- inside the game-mode server_onFixedUpdate sandbox (40/s). Client: every
-- machine, inside client_onFixedUpdate. fn( game ) - game is the game-mode
-- script instance (self).
function N.onServerTick( fn ) N.serverTickers[#N.serverTickers + 1] = fn end
function N.onClientTick( fn ) N.clientTickers[#N.clientTickers + 1] = fn end
-- Callback hook for Lua mods (same as the native AddCallbackHook, but for this
-- world only): before the game calls `callback`, call the function named by
-- `handler` with the same arguments. Both are expressions in the game env.
function N.addCallbackHook( callback, handler ) native( "hook", callback, handler ) end

local function clQueue( pkt )
	if C.gaveUp then return false end
	local o = C.out
	if #o >= N.MAX_QUEUE then table.remove( o, 1 ) log( "send queue full, dropped the oldest message" ) end
	o[#o + 1] = pkt
	return true
end
local function svQueue( target, pkt )
	if not N.isHost() then log( "only the host can send to clients (" .. tostring( pkt.m ) .. "/" .. tostring( pkt.n ) .. ")" ) return false end
	S.out[#S.out + 1] = { t = target, p = pkt }
	return true
end

-- Client -> host
function N.sendToServer( mod, name, data ) return clQueue( { k = "m", m = mod, n = name, d = data } ) end
-- Client -> every player with the mod, relayed by the host (sender included / excluded)
function N.sendToAll( mod, name, data ) return clQueue( { k = "r", m = mod, n = name, d = data } ) end
function N.sendToOthers( mod, name, data ) return clQueue( { k = "r", m = mod, n = name, d = data, x = true } ) end
-- Client -> host, with a reply: callback( reply, err ); err is a string on failure.
function N.request( mod, name, data, callback, timeout )
	local id = C.nextReq
	C.nextReq = id + 1
	C.pending[id] = { cb = callback, at = now(), timeout = timeout or 10 }
	return clQueue( { k = "q", m = mod, n = name, d = data, i = id } )
end
-- Host -> one player / every player with the mod
function N.sendToClient( player, mod, name, data )
	local id = pidOf( player )
	if not id then return false end
	return svQueue( id, { k = "m", m = mod, n = name, d = data } )
end
function N.broadcast( mod, name, data ) return svQueue( "mod", { k = "m", m = mod, n = name, d = data } ) end

-- Shared state: host-owned key/value pairs per mod, mirrored to every player
-- with the mod (late joiners get a full copy). Session only: not saved.
function N.setShared( mod, key, value )
	if not N.isHost() then log( "setShared: only the host can change shared state" ) return false end
	slot( S.shared, mod )[key] = value
	slot( S.dirty, mod )[key] = true
	return true
end
function N.getShared( mod, key )
	local t = ( N.isHost() and S.shared[mod] ) or C.shared[mod] or {}
	if key == nil then return t end
	return t[key]
end

-- Status and roster
function N.isHost() local ok, v = pcall( function() return sm.isHost end ) return ok and v == true end
function N.isConnected() return C.welcomed end
function N.localPlayerId() return C.localId end
local function roster() return ( N.isHost() and S.roster ) or C.roster end
-- { { id = 1, name = "...", smnet = true, mods = { [mod] = version } }, ... }
function N.players() local r = roster() return r and r.p or {} end
function N.hasMod( player, mod )
	local id = pidOf( player )
	for _, p in ipairs( N.players() ) do
		if p.id == id then return p.mods[mod] ~= nil, p.mods[mod] end
	end
	return false
end
-- true once every player in the world has announced `mod`
function N.isReady( mod ) local r = roster() return r ~= nil and r.ready[mod] == true end
-- names of players known not to have `mod` (players still joining aren't listed)
function N.missing( mod ) local r = roster() return r and r.missing[mod] or {} end

-- ---------------------------------------------------------------------------
-- Dispatch
-- ---------------------------------------------------------------------------
local function playerById( id )
	if not id then return nil end
	local ok, list = pcall( sm.player.getAllPlayers )
	if ok then for _, p in ipairs( list ) do if pidOf( p ) == id then return p end end end
	return nil
end

local function toNativeData( d )
	if d == nil then return "" end
	if ty( d ) == "string" then return d end
	local ok, s = pcall( json.encode, d )
	return ok and s or ""
end

local function dispatch( side, mod, name, data, from, fromId )
	local h = N.handlers[side][mod]
	h = h and ( h[name] or h["*"] )
	if h then
		if h == N.handlers[side][mod]["*"] then safeCall( h, name, data, from ) else safeCall( h, data, from ) end
	end
	if N.nativeMods[mod] then native( "msg", side, mod, name, toNativeData( data ), fromId or 0 ) end
end

local function fireShared( mod, key, value )
	for _, fn in ipairs( N.sharedListeners[mod] or {} ) do safeCall( fn, key, value ) end
end

-- ---------------------------------------------------------------------------
-- Host side
-- ---------------------------------------------------------------------------
local function newPlayer( id, pl, t )
	local ok, name = pcall( function() return pl.name end )
	return { id = id, name = ok and name or ( "Player " .. id ), player = pl, seen = t, hello = false, mods = {} }
end

local function buildRoster( t )
	local list, ready, missing, all = {}, {}, {}, {}
	for _, p in pairs( S.players ) do for m in pairs( p.mods ) do all[m] = true end end
	for m in pairs( all ) do ready[m] = true missing[m] = {} end
	local sig = {}
	for id, p in pairs( S.players ) do
		local mods = {}
		for m, v in pairs( p.mods ) do mods[m] = v end
		list[#list + 1] = { id = id, name = p.name, smnet = p.hello, mods = mods }
		local late = not p.hello and ( t - p.seen ) > N.HELLO_GRACE
		for m in pairs( all ) do
			if not p.mods[m] then
				ready[m] = false
				if p.hello or late then local ms = missing[m] ms[#ms + 1] = p.name end
			end
		end
		local ms = {}
		for m, v in pairs( p.mods ) do ms[#ms + 1] = m .. "=" .. v end
		table.sort( ms )
		sig[#sig + 1] = id .. ":" .. tostring( p.name ) .. ":" .. ( p.hello and "h" or ( late and "l" or "p" ) ) .. ":" .. table.concat( ms, "," )
	end
	table.sort( list, function( a, b ) return a.id < b.id end )
	table.sort( sig )
	for _, ms in pairs( missing ) do table.sort( ms ) end
	return { p = list, ready = ready, missing = missing }, table.concat( sig, ";" )
end

local function svFlush( game )
	if #S.out == 0 then return end
	local batches = {}
	for _, e in ipairs( S.out ) do
		local pkt = e.p
		if e.t == "mod" or e.t == "all" then
			for id, p in pairs( S.players ) do
				if p.hello and ( e.t == "all" or p.mods[pkt.m] ) then local b = slot( batches, id ) b[#b + 1] = pkt end
			end
		else
			local p = S.players[e.t]
			if p and p.hello then local b = slot( batches, e.t ) b[#b + 1] = pkt end
		end
	end
	S.out = {}
	for id, b in pairs( batches ) do
		local p = S.players[id]
		local ok, err = pcall( function() game.network:sendToClient( p.player, CL_CB, { v = N.PROTOCOL, b = b } ) end )
		if not ok then log( "send to " .. tostring( p.name ) .. " failed: " .. tostring( err ) ) end
	end
end

function N.__svTick( game )
	if ty( game ) ~= "table" or not game.network then return end
	local t = now()
	if t - S.scanAt >= 1 then
		S.scanAt = t
		local seen = {}
		local ok, list = pcall( sm.player.getAllPlayers )
		for _, pl in ipairs( ok and list or {} ) do
			local id = pidOf( pl )
			if id then
				seen[id] = true
				local p = S.players[id]
				if not p then S.players[id] = newPlayer( id, pl, t ) else p.player = pl end
			end
		end
		for id in pairs( S.players ) do if not seen[id] then S.players[id] = nil end end
		local r, sig = buildRoster( t )
		S.roster = r
		if sig ~= S.rosterSig then
			S.rosterSig = sig
			svQueue( "all", { k = "R", r = r } )
		end
	end
	for mod, keys in pairs( S.dirty ) do
		local set, del = {}, {}
		local vals = S.shared[mod] or {}
		for k in pairs( keys ) do
			if vals[k] == nil then del[#del + 1] = k else set[k] = vals[k] end
		end
		svQueue( "mod", { k = "S", m = mod, s = set, x = del } )
	end
	S.dirty = {}
	for _, fn in ipairs( N.serverTickers ) do safeCall( fn, game ) end
	svFlush( game )
end

local function svReceive( game, pkt, player )
	if ty( pkt ) ~= "table" or ty( pkt.b ) ~= "table" then return end
	local id = pidOf( player )
	if not id then return end
	local p = S.players[id]
	if not p then p = newPlayer( id, player, now() ) S.players[id] = p end
	for _, m in ipairs( pkt.b ) do
		if ty( m ) ~= "table" then
		elseif m.k == "h" then
			local first = not p.hello
			local old = p.mods
			p.hello = true
			p.mods = ty( m.mods ) == "table" and m.mods or {}
			if first then
				svQueue( id, { k = "W", id = id, shared = S.shared } )
			else
				-- mods registered after joining: send their shared state now (any
				-- changes broadcast before this hello skipped this player)
				for mod in pairs( p.mods ) do
					if not old[mod] and S.shared[mod] then
						local copy = {}
						for k, v in pairs( S.shared[mod] ) do copy[k] = v end
						svQueue( id, { k = "S", m = mod, s = copy, x = {} } )
					end
				end
			end
			S.scanAt = -100                -- rebuild the roster on the next tick
		elseif not p.hello then
			-- ignore traffic from a client that hasn't said hello
		elseif m.k == "m" then
			dispatch( SIDE_SERVER, m.m, m.n, m.d, player, id )
		elseif m.k == "r" then
			for qid, q in pairs( S.players ) do
				if q.hello and q.mods[m.m] and not ( m.x and qid == id ) then
					svQueue( qid, { k = "m", m = m.m, n = m.n, d = m.d, f = id } )
				end
			end
		elseif m.k == "q" then
			local hs = N.requestHandlers[m.m]
			local h = hs and hs[m.n]
			local ok, reply = false, "no request handler for " .. tostring( m.m ) .. "/" .. tostring( m.n ) .. " on the host"
			if h then
				ok, reply = pcall( h, m.d, player )
				if not ok then log( reply ) reply = tostring( reply ) end
			end
			svQueue( id, { k = "a", i = m.i, ok = ok, d = reply } )
		end
	end
end

-- ---------------------------------------------------------------------------
-- Client side
-- ---------------------------------------------------------------------------
local function helloPacket()
	local mods = {}
	for m, v in pairs( N.mods ) do mods[m] = v end
	return { k = "h", v = N.PROTOCOL, mods = mods }
end

function N.__clTick( game )
	if ty( game ) ~= "table" or not game.network then return end
	local t = now()
	C.start = C.start or t
	if not C.localId then
		local ok, pl = pcall( sm.localPlayer.getPlayer )
		if ok and pl then C.localId = pidOf( pl ) end
	end
	local batch = {}
	if not C.welcomed then
		if not C.gaveUp then
			if t - C.start > N.HELLO_GIVE_UP then
				C.gaveUp = true
				C.out = {}
				log( "the host did not answer: it doesn't have SMLoader 1.1+, so SMNetworking is off in this world" )
			elseif t - C.helloAt >= 2 then
				C.helloAt = t
				C.modsDirty = false
				batch[1] = helloPacket()
			end
		end
	else
		if C.modsDirty then C.modsDirty = false batch[#batch + 1] = helloPacket() end
		for _, p in ipairs( C.out ) do batch[#batch + 1] = p end
		C.out = {}
	end
	for _, fn in ipairs( N.clientTickers ) do safeCall( fn, game ) end
	for _, p in ipairs( C.welcomed and C.out or {} ) do batch[#batch + 1] = p end
	if C.welcomed then C.out = {} end
	if #batch > 0 then
		local ok, err = pcall( function() game.network:sendToServer( SV_CB, { v = N.PROTOCOL, b = batch } ) end )
		if not ok then log( "send to host failed: " .. tostring( err ) ) end
	end
	for id, r in pairs( C.pending ) do
		if t - r.at > r.timeout then C.pending[id] = nil safeCall( r.cb, nil, "timeout" ) end
	end
	if t - C.statusAt >= 1 then
		C.statusAt = t
		for mod in pairs( N.nativeMods ) do
			native( "status", mod, C.welcomed, N.isHost(), N.isReady( mod ), #N.players(),
			        #N.missing( mod ), table.concat( N.missing( mod ), ", " ), C.localId or 0 )
		end
	end
end

local function clReceive( game, pkt )
	if ty( pkt ) ~= "table" or ty( pkt.b ) ~= "table" then return end
	for _, m in ipairs( pkt.b ) do
		if ty( m ) ~= "table" then
		elseif m.k == "W" then
			C.welcomed = true
			C.localId = m.id or C.localId
			C.shared = ty( m.shared ) == "table" and m.shared or {}
			for mod in pairs( N.sharedListeners ) do fireShared( mod, nil, nil ) end
		elseif m.k == "R" then
			C.roster = m.r
			for _, fn in ipairs( N.rosterListeners ) do safeCall( fn, m.r.p ) end
		elseif m.k == "m" then
			dispatch( SIDE_CLIENT, m.m, m.n, m.d, playerById( m.f ), m.f )
		elseif m.k == "S" then
			local t = slot( C.shared, m.m )
			for k, v in pairs( m.s or {} ) do t[k] = v fireShared( m.m, k, v ) end
			for _, k in ipairs( m.x or {} ) do t[k] = nil fireShared( m.m, k, nil ) end
		elseif m.k == "a" then
			local r = C.pending[m.i]
			if r then
				C.pending[m.i] = nil
				if m.ok then safeCall( r.cb, m.d, nil ) else safeCall( r.cb, nil, tostring( m.d ) ) end
			end
		end
	end
end

-- ---------------------------------------------------------------------------
-- Native (C++) bridge - called by SMLoader
-- ---------------------------------------------------------------------------
function N.__nativeRegister( mod, version )
	N.nativeMods[mod] = true
	if version then N.register( mod, version ) end
end
function N.__nativeSend( mod, name, target, data )
	if target == TO_SERVER then return N.sendToServer( mod, name, data )
	elseif target == TO_ALL then return N.sendToAll( mod, name, data )
	elseif target == TO_OTHERS then return N.sendToOthers( mod, name, data )
	elseif target == BROADCAST then return N.broadcast( mod, name, data ) end
	return N.sendToClient( target, mod, name, data )
end

-- ---------------------------------------------------------------------------
-- Install into the game-mode class (called by SMLoader every second; cheap)
-- ---------------------------------------------------------------------------
local GAME_CLASSES = { "SurvivalGame", "CreativeGame", "CreativeTerrainGame", "CreativeFlatGame",
                       "ClassicCreativeGame", "CreativeCustomGame", "ChallengeGame", "Game" }
local function svCallback( self, data, player ) safeCall( svReceive, self, data, player ) end
local function clCallback( self, data ) safeCall( clReceive, self, data ) end
N.__scanAt = -100
function N.__scan()
	local t = now()
	if N.__scanned and t - N.__scanAt < 10 then return "ok" end
	N.__scanAt = t
	local svSeen, clSeen, found = {}, {}, 0
	for _, name in ipairs( GAME_CLASSES ) do
		local cls = E[name]
		if ty( cls ) == "table" then
			found = found + 1
			if cls[SV_CB] ~= svCallback then cls[SV_CB] = svCallback end
			if cls[CL_CB] ~= clCallback then cls[CL_CB] = clCallback end
			local f = cls.server_onFixedUpdate
			if ty( f ) == "function" and not svSeen[f] then
				svSeen[f] = true
				native( "hook", name .. ".server_onFixedUpdate", "SMNet.__svTick" )
			end
			local cb = "client_onFixedUpdate"
			local g = cls[cb]
			if ty( g ) ~= "function" then cb = "client_onUpdate" g = cls[cb] end
			if ty( g ) == "function" and not clSeen[g] then
				clSeen[g] = true
				native( "hook", name .. "." .. cb, "SMNet.__clTick" )
			end
		end
	end
	if found > 0 and not N.__scanned then log( "SMNetworking " .. N.VERSION .. " ready" ) end
	N.__scanned = found > 0
	return N.__scanned and "ok" or "no game class yet"
end

return "installed"
