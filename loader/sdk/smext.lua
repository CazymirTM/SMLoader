-- SMExtensions - additional functionality exposed to Lua (global `SMX`).
--
-- SMLoader compiles this file into winmm.dll and installs it after the other
-- SDK modules in every world. Full guide: docs/EXTENSIONS.md.
--
-- Security model: the game's Lua environment is shared with Workshop scripts,
-- so anything here that touches the PC is guarded:
--   * files: only inside SMLoader\data\<mod>\, only with that mod's secret
--     token (handed to the mod's own native code by the loader), with path and
--     size limits;
--   * key state: only while the game window is in the foreground;
--   * no network access, no clipboard reading, no arbitrary paths.

local E, NX = ...                     -- game env, natives table from the loader
if SMX and SMX.__env == E then return "already installed" end
if not SMNet then error( "SMExtensions needs SMNetworking", 0 ) end
local NET = SMNet

local X = {}
SMX = X
X.__env = E
X.VERSION = "1.2.0"

local function ty( v ) local ok, r = pcall( type, v ) return ok and r or "?" end
local function plog( msg ) print( "[SMX] " .. tostring( msg ) ) end
local bit = bit                         -- the game env has LuaJIT's bit library

-- ---------------------------------------------------------------------------
-- Loader info, logging, time
-- ---------------------------------------------------------------------------
X.loader = NX.info()                    -- { version, api, mods = { "WokyMenu.dll", ... } }

-- Write a line to SMLoader\loader.log (arguments joined with spaces).
function X.log( ... )
	local parts = {}
	for i = 1, select( "#", ... ) do parts[i] = tostring( ( select( i, ... ) ) ) end
	NX.log( table.concat( parts, " " ) )
end

-- High-resolution real time in milliseconds (for measuring, not for dates).
X.clock = NX.clock
-- os.date-style formatting of a unix time (default now), e.g. "%Y-%m-%d %H:%M".
function X.date( fmt, t ) return NX.date( fmt or "%Y-%m-%d %H:%M:%S", t ) end

-- Run fn(...) and return its results; logs how long it took when it's slow
-- (or always with threshold 0).
local function from( t, i, n ) if i > n then return end return t[i], from( t, i + 1, n ) end
function X.profile( name, threshold, fn, ... )
	local t0 = NX.clock()
	local res = { n = 0 }
	local function keep( ... ) res.n = select( "#", ... ) for i = 1, res.n do res[i] = ( select( i, ... ) ) end end
	keep( pcall( fn, ... ) )
	local ms = NX.clock() - t0
	if ms >= ( threshold or 0 ) then NX.log( string.format( "[profile] %s took %.3f ms", tostring( name ), ms ) ) end
	if not res[1] then error( res[2], 2 ) end
	return from( res, 2, res.n )
end

-- ---------------------------------------------------------------------------
-- Lua helpers the game sandbox leaves out
-- ---------------------------------------------------------------------------
X.setmetatable = NX.setmetatable
-- getmetatable for tables only (strings and game objects are left alone)
function X.getmetatable( t ) if ty( t ) == "table" then return NX.getmetatable( t ) end return nil end
X.rawget, X.rawset, X.rawequal = NX.rawget, NX.rawset, NX.rawequal
-- Compile Lua source into a function that runs in the game env.
-- Returns fn, or nil and the error message.
function X.compile( source, chunkname ) return NX.compile( tostring( source ), chunkname or "=SMX.compile" ) end

-- ---------------------------------------------------------------------------
-- Native (C++) functions registered by mods: SMX.fn.<mod>.<name>(...)
-- ---------------------------------------------------------------------------
X.fn = {}
-- Call a registered function by its full name: SMX.call( "mymod.doThing", a, b )
function X.call( name, ... )
	local args = { ... }
	local ok, json = pcall( NET.json.encode, args )
	if not ok then error( "SMX.call: arguments can't be converted to JSON: " .. tostring( json ), 2 ) end
	local result, err = NX.call( name, json )
	if err then error( err, 2 ) end
	if result == nil or result == "" or result == "null" then return nil end
	local okd, v = pcall( NET.json.decode, result )
	if okd then return v end
	return result
end
local function placeFn( name, f )
	local mod, fname = name:match( "^([%w_%-]+)%.(.+)$" )
	if not mod then return end
	local t = X.fn[mod]
	if not t then t = {} X.fn[mod] = t end
	t[fname] = f
end
function X.__addJson( name ) placeFn( name, function( ... ) return X.call( name, ... ) end ) end
function X.__addRaw( name, cf ) placeFn( name, cf ) end

-- ---------------------------------------------------------------------------
-- Timers (driven by SMNet ticks: no game function is wrapped)
-- ---------------------------------------------------------------------------
local timers = { client = {}, server = {} }
local nextTimer = 1
local function tick() local ok, t = pcall( sm.game.getCurrentTick ) return ok and t or 0 end
local function addTimer( side, seconds, fn, every )
	if ty( fn ) ~= "function" then error( "SMX timer: fn must be a function", 3 ) end
	side = side == "server" and "server" or "client"
	local id = nextTimer
	nextTimer = nextTimer + 1
	local ticks = math.max( 1, math.floor( ( tonumber( seconds ) or 0 ) * 40 + 0.5 ) )
	timers[side][id] = { fn = fn, at = tick() + ticks, every = every and ticks or nil }
	return id
end
-- Call fn once after `seconds` (game time: pausing pauses it). side: "client"
-- (default, every machine, client callback context) or "server" (host only,
-- server callback context). Returns an id for SMX.cancel.
function X.after( seconds, fn, side ) return addTimer( side, seconds, fn, false ) end
function X.every( seconds, fn, side ) return addTimer( side, seconds, fn, true ) end
function X.cancel( id ) timers.client[id] = nil timers.server[id] = nil end
local function runTimers( list, game )
	local now = tick()
	for id, t in pairs( list ) do
		if now >= t.at then
			if t.every then t.at = now + t.every else list[id] = nil end
			local ok, err = pcall( t.fn, game )
			if not ok then plog( "timer failed: " .. tostring( err ) ) end
		end
	end
end
NET.onServerTick( function( game ) runTimers( timers.server, game ) end )

-- ---------------------------------------------------------------------------
-- Input: key state and hotkeys (client, only while the game is focused)
-- ---------------------------------------------------------------------------
X.input = {}
local KEYS = { BACKSPACE = 0x08, TAB = 0x09, ENTER = 0x0D, SHIFT = 0x10, CTRL = 0x11, ALT = 0x12, PAUSE = 0x13,
               CAPSLOCK = 0x14, ESC = 0x1B, SPACE = 0x20, PAGEUP = 0x21, PAGEDOWN = 0x22, END = 0x23, HOME = 0x24,
               LEFT = 0x25, UP = 0x26, RIGHT = 0x27, DOWN = 0x28, INSERT = 0x2D, DELETE = 0x2E,
               MOUSE1 = 0x01, MOUSE2 = 0x02, MOUSE3 = 0x04, MOUSE4 = 0x05, MOUSE5 = 0x06,
               NUMPAD0 = 0x60, MULTIPLY = 0x6A, ADD = 0x6B, SUBTRACT = 0x6D, DECIMAL = 0x6E, DIVIDE = 0x6F }
for i = 0, 9 do KEYS[tostring( i )] = 0x30 + i KEYS["NUMPAD" .. i] = 0x60 + i end
for i = 0, 25 do KEYS[string.char( 65 + i )] = 0x41 + i end
for i = 1, 12 do KEYS["F" .. i] = 0x6F + i end
X.input.KEYS = KEYS
local function vkOf( key )
	if ty( key ) == "number" then return key end
	return KEYS[tostring( key ):upper()]
end
-- true while the key is held (and the game window is focused)
function X.input.isKeyDown( key ) local vk = vkOf( key ) return vk ~= nil and NX.keyDown( vk ) end
local binds = {}                        -- [vk] = { down = bool, list = { { fn, id } } }
local nextBind = 1
-- Call fn( keyName ) each time `key` is pressed. Also emits the SMEvents
-- "input.keyDown" / "input.keyUp" ({ key = name }) for bound keys.
-- Keys typed into the chat also count, so prefer F-keys or modifiers.
function X.input.bind( key, fn )
	local vk = vkOf( key )
	if not vk then error( "SMX.input.bind: unknown key '" .. tostring( key ) .. "'", 2 ) end
	local b = binds[vk]
	if not b then b = { down = false, list = {}, name = tostring( key ):upper() } binds[vk] = b end
	local id = nextBind
	nextBind = nextBind + 1
	b.list[#b.list + 1] = { fn = fn, id = id }
	return id
end
function X.input.unbind( id )
	for _, b in pairs( binds ) do
		for i = #b.list, 1, -1 do if b.list[i].id == id then table.remove( b.list, i ) end end
	end
end
NET.onClientTick( function( game )
	runTimers( timers.client, game )
	for vk, b in pairs( binds ) do
		if #b.list > 0 then
			local down = NX.keyDown( vk )
			if down and not b.down then
				for _, l in ipairs( b.list ) do
					local ok, err = pcall( l.fn, b.name )
					if not ok then plog( "key bind failed: " .. tostring( err ) ) end
				end
				if SMEvents then SMEvents.emit( "input.keyDown", { key = b.name } ) end
			elseif b.down and not down then
				if SMEvents then SMEvents.emit( "input.keyUp", { key = b.name } ) end
			end
			b.down = down
		end
	end
end )

-- ---------------------------------------------------------------------------
-- Files: a private folder per mod (SMLoader\data\<mod>\), token protected
-- ---------------------------------------------------------------------------
-- local fs = SMX.files( "mymod", TOKEN )   -- TOKEN from api->ExtToken("mymod")
-- Paths are relative ("saves/slot1.json"); folders are created as needed.
function X.files( mod, token )
	if not NX.fsOk( mod, token ) then error( "SMX.files: wrong token for mod '" .. tostring( mod ) .. "'", 2 ) end
	local fs = {}
	local function op( name, path, data )
		local r, err = NX.fs( name, mod, token, path, data )
		if err then return nil, err end
		return r
	end
	function fs.read( path ) return op( "read", path ) end
	function fs.write( path, text ) return op( "write", path, tostring( text ) ) end
	function fs.append( path, text ) return op( "append", path, tostring( text ) ) end
	function fs.exists( path ) return op( "exists", path ) == true end
	function fs.remove( path ) return op( "remove", path ) end
	function fs.list( dir ) return op( "list", dir or "" ) or {} end
	function fs.readJson( path )
		local text, err = fs.read( path )
		if not text then return nil, err end
		local ok, v = pcall( NET.json.decode, text )
		if not ok then return nil, v end
		return v
	end
	function fs.writeJson( path, value ) return fs.write( path, X.pretty( value, true ) ) end
	-- Persistent per-user settings (config.json), merged over `defaults`.
	function fs.config( defaults )
		local cfg = X.table.deepCopy( defaults or {} )
		local saved = fs.readJson( "config.json" )
		if ty( saved ) == "table" then X.table.merge( cfg, saved, true ) end
		return cfg
	end
	function fs.saveConfig( cfg ) return fs.writeJson( "config.json", cfg ) end
	return fs
end

-- ---------------------------------------------------------------------------
-- Utilities (pure Lua)
-- ---------------------------------------------------------------------------
X.json = NET.json

X.table = {}
function X.table.copy( t ) local c = {} for k, v in pairs( t ) do c[k] = v end return c end
function X.table.deepCopy( t, seen )
	if ty( t ) ~= "table" then return t end
	seen = seen or {}
	if seen[t] then return seen[t] end
	local c = {}
	seen[t] = c
	for k, v in pairs( t ) do c[X.table.deepCopy( k, seen )] = X.table.deepCopy( v, seen ) end
	return c
end
-- Copy src's keys into dst (recursively for tables when deep). Returns dst.
function X.table.merge( dst, src, deep )
	for k, v in pairs( src ) do
		if deep and ty( v ) == "table" and ty( dst[k] ) == "table" then X.table.merge( dst[k], v, true ) else dst[k] = v end
	end
	return dst
end
function X.table.keys( t ) local r = {} for k in pairs( t ) do r[#r + 1] = k end return r end
function X.table.values( t ) local r = {} for _, v in pairs( t ) do r[#r + 1] = v end return r end
function X.table.count( t ) local n = 0 for _ in pairs( t ) do n = n + 1 end return n end
function X.table.find( t, value ) for k, v in pairs( t ) do if v == value then return k end end return nil end
function X.table.map( t, fn ) local r = {} for k, v in pairs( t ) do r[k] = fn( v, k ) end return r end
function X.table.filter( list, fn ) local r = {} for _, v in ipairs( list ) do if fn( v ) then r[#r + 1] = v end end return r end

-- Readable text for any value (tables nested, keys sorted). asJson = true
-- gives indented JSON instead.
function X.pretty( v, asJson, indent, seen )
	indent = indent or ""
	if asJson then
		if ty( v ) ~= "table" then return NET.json.encode( v ) end
		local isList = #v > 0
		local inner = indent .. "  "
		local parts = {}
		if isList then
			for _, x in ipairs( v ) do parts[#parts + 1] = inner .. X.pretty( x, true, inner ) end
			return "[\n" .. table.concat( parts, ",\n" ) .. "\n" .. indent .. "]"
		end
		local keys = X.table.keys( v )
		table.sort( keys, function( a, b ) return tostring( a ) < tostring( b ) end )
		for _, k in ipairs( keys ) do parts[#parts + 1] = inner .. NET.json.encode( tostring( k ) ) .. ": " .. X.pretty( v[k], true, inner ) end
		if #parts == 0 then return "{}" end
		return "{\n" .. table.concat( parts, ",\n" ) .. "\n" .. indent .. "}"
	end
	if ty( v ) == "string" then return string.format( "%q", v ) end
	if ty( v ) ~= "table" then return tostring( v ) end
	seen = seen or {}
	if seen[v] then return "<cycle>" end
	seen[v] = true
	local keys = X.table.keys( v )
	if #keys == 0 then return "{}" end
	table.sort( keys, function( a, b ) return tostring( a ) < tostring( b ) end )
	local inner = indent .. "  "
	local parts = {}
	for _, k in ipairs( keys ) do
		local ks = ty( k ) == "string" and k:match( "^[%a_][%w_]*$" ) and k or ( "[" .. X.pretty( k ) .. "]" )
		parts[#parts + 1] = inner .. ks .. " = " .. X.pretty( v[k], false, inner, seen )
	end
	return "{\n" .. table.concat( parts, ",\n" ) .. "\n" .. indent .. "}"
end

X.string = {}
function X.string.split( s, sep, plain )
	local out, start = {}, 1
	sep = sep or ","
	while true do
		local i, j = s:find( sep, start, plain ~= false )
		if not i then out[#out + 1] = s:sub( start ) break end
		out[#out + 1] = s:sub( start, i - 1 )
		start = j + 1
	end
	return out
end
function X.string.trim( s ) return ( s:gsub( "^%s+", "" ):gsub( "%s+$", "" ) ) end
function X.string.startsWith( s, p ) return s:sub( 1, #p ) == p end
function X.string.endsWith( s, p ) return p == "" or s:sub( -#p ) == p end
function X.string.pad( s, n, ch, left )
	s = tostring( s )
	local fill = string.rep( ch or " ", math.max( 0, n - #s ) )
	return left and ( fill .. s ) or ( s .. fill )
end

X.math = {}
function X.math.clamp( v, lo, hi ) return math.max( lo, math.min( hi, v ) ) end
function X.math.lerp( a, b, t ) return a + ( b - a ) * t end
function X.math.round( v, decimals ) local m = 10 ^ ( decimals or 0 ) return math.floor( v * m + 0.5 ) / m end
function X.math.map( v, inLo, inHi, outLo, outHi ) return outLo + ( v - inLo ) * ( outHi - outLo ) / ( inHi - inLo ) end
function X.math.sign( v ) return v > 0 and 1 or ( v < 0 and -1 or 0 ) end

-- CRC-32 of a string, as an unsigned number
local crcTable
function X.crc32( s )
	if not crcTable then
		crcTable = {}
		for i = 0, 255 do
			local c = i
			for _ = 1, 8 do c = bit.band( c, 1 ) == 1 and bit.bxor( 0xEDB88320, bit.rshift( c, 1 ) ) or bit.rshift( c, 1 ) end
			crcTable[i] = c
		end
	end
	local c = 0xFFFFFFFF
	for i = 1, #s do c = bit.bxor( crcTable[bit.band( bit.bxor( c, s:byte( i ) ), 0xFF )], bit.rshift( c, 8 ) ) end
	c = bit.bxor( c, 0xFFFFFFFF )
	if c < 0 then c = c + 4294967296 end
	return c
end

local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
X.base64 = {}
function X.base64.encode( s )
	local out = {}
	for i = 1, #s, 3 do
		local a, b, c = s:byte( i, i + 2 )
		local n = a * 65536 + ( b or 0 ) * 256 + ( c or 0 )
		local c1, c2, c3, c4 = math.floor( n / 262144 ) % 64, math.floor( n / 4096 ) % 64, math.floor( n / 64 ) % 64, n % 64
		out[#out + 1] = B64:sub( c1 + 1, c1 + 1 ) .. B64:sub( c2 + 1, c2 + 1 )
			.. ( b and B64:sub( c3 + 1, c3 + 1 ) or "=" ) .. ( c and B64:sub( c4 + 1, c4 + 1 ) or "=" )
	end
	return table.concat( out )
end
function X.base64.decode( s )
	s = s:gsub( "[^%w%+/=]", "" )
	local out = {}
	for i = 1, #s, 4 do
		local n, pad = 0, 0
		for j = 0, 3 do
			local ch = s:sub( i + j, i + j )
			local v = ch == "=" and 0 or ( ( B64:find( ch, 1, true ) or 1 ) - 1 )
			if ch == "=" or ch == "" then pad = pad + 1 end
			n = n * 64 + v
		end
		local bytes = string.char( math.floor( n / 65536 ) % 256, math.floor( n / 256 ) % 256, n % 256 )
		out[#out + 1] = bytes:sub( 1, 3 - pad )
	end
	return table.concat( out )
end

return "installed"
