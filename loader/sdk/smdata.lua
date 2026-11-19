-- SMPlayerData - persistent player statistics and progression for SMLoader mods.
--
-- SMLoader compiles this file into winmm.dll and installs it right after
-- SMNetworking in every world. It defines the global table `SMData`.
-- Full guide: docs/PLAYERDATA.md.
--
--  Storage    The host owns everything. Each player's record is saved in the
--             world's own storage (sm.storage channel CHANNEL_BASE + player
--             id; the engine keys player data by that id too), so progress is
--             per world and travels with the save file. A small index
--             (CHANNEL_BASE + 0) keeps names and stats of everyone who ever
--             played, for all-time leaderboards.
--  Stats      Tracked automatically by the host: playtime, sessions, distance
--             on foot / in vehicles, and in Survival also deaths, damage taken,
--             robot kills (also per robot type), damage dealt to robots, crops
--             harvested and food eaten. Mods can add their own stats.
--  Mod data   Per player, per mod: key/value data, XP with level curves, and
--             unlocks (achievements). Private to the player unless published.
--  Sync       Each player gets their own full record (SMData.mine...). Stats
--             and published keys of everyone are shared with all players.
--  Hooks      Observation only (SMNet callback hooks + tick listeners); no game
--             function is wrapped or replaced.

local E = ...
if SMData and SMData.__env == E then return "already installed" end
if not SMNet then error( "SMPlayerData needs SMNetworking", 0 ) end
local NET = SMNet

local D = {}
SMData = D
D.__env = E
D.VERSION = "1.2.0"
D.MOD = "smdata"
D.CHANNEL_BASE = 1397555200           -- 0x534D0000 ("SM"): +0 index, +id player
D.SAVE_INTERVAL = 10                  -- seconds between saves of changed records
D.SYNC_INTERVAL = 2                   -- seconds between private syncs to each player
D.PUBLIC_INTERVAL = 3                 -- seconds between public (shared) updates
D.BUILTIN_STATS = { "playtime", "sessions", "firstSeen", "lastSeen", "distanceWalked", "distanceDriven",
                    "deaths", "damageTaken", "kills", "robotDamage", "cropsHarvested", "foodEaten" }

local function ty( v ) local ok, r = pcall( type, v ) return ok and r or "?" end
local function log( msg ) print( "[SMData] " .. tostring( msg ) ) end
local function safeCall( f, ... )
	if not f then return end
	local ok, err = pcall( f, ... )
	if not ok then log( err ) end
end
local function slot( t, k ) local s = t[k] if not s then s = {} t[k] = s end return s end
local function native( ... )
	local f = NET.__native
	if f then local ok, err = pcall( f, ... ) if not ok then log( err ) end end
end
local function jsonOf( v ) local ok, s = pcall( NET.json.encode, v ) return ok and s or "null" end
local function listJson( l ) if #l == 0 then return "[]" end return jsonOf( l ) end   -- {} would read as an object
local function pidOf( p )
	if ty( p ) == "number" then
		if p == 0 then return NET.localPlayerId() end
		return p
	end
	local ok, id = pcall( function() return p.id end )
	return ok and id or nil
end
local function nameOf( pl ) local ok, n = pcall( function() return pl.name end ) return ok and n or nil end
local function isHost() return NET.isHost() end
local function hostOnly( fname )
	if isHost() then return true end
	log( fname .. ": only the host can change player data" )
	return false
end

-- Listeners and settings (every machine registers its own)
D.curves = {}                          -- [mod] = { base, growth, max } or { fn = function( level ) -> xp, max }
D.public = {}                          -- [mod] = { [key] = true }: keys everyone can see
D.changeListeners = {}                 -- [mod] = { fn( player, key, value ) } (host)
D.levelListeners = {}                  -- [mod] = { fn( player, level, oldLevel ) } (host)
D.unlockListeners = {}                 -- [mod] = { fn( player, id ) } (host)
D.mineListeners = {}                   -- { fn( mod, key, value ) } (every machine, own data)

-- Host state
local H = { recs = {}, index = nil, indexDirty = false, saveAt = 0, syncAt = 0, publicAt = 0,
            lastTick = nil, hits = {}, scanAt = -100, scanned = false }
-- Client state: this machine's own record (as sent by the host)
local C = { mine = nil, localId = nil }
D.__h, D.__c = H, C

-- ---------------------------------------------------------------------------
-- Records (host)
-- ---------------------------------------------------------------------------
local function newRec( id ) return { id = id, name = "Player " .. id, stats = {}, mods = {} } end

local function loadIndex()
	if H.index then return H.index end
	local ok, t = pcall( sm.storage.load, D.CHANNEL_BASE )
	if not ok then return { v = 1, players = {} } end   -- no server context: don't cache
	H.index = ( ty( t ) == "table" and ty( t.players ) == "table" ) and t or { v = 1, players = {} }
	return H.index
end

-- Returns the record for a player (Player or id). Loads it from the world on
-- first use. That needs a server callback context: outside one this returns
-- nil instead of an empty record, so a failed load can never overwrite saved
-- progress. (Online players are loaded by the host tick within a second.)
local function rec( player )
	local id = pidOf( player )
	if not id then return nil end
	local r = H.recs[id]
	if r then return r end
	local ok, saved = pcall( sm.storage.load, D.CHANNEL_BASE + id )
	if not ok then return nil end
	r = newRec( id )
	if ty( saved ) == "table" then
		r.name = saved.name or r.name
		r.stats = ty( saved.stats ) == "table" and saved.stats or {}
		r.mods = ty( saved.mods ) == "table" and saved.mods or {}
	end
	if ty( player ) ~= "number" then r.name = nameOf( player ) or r.name end
	H.recs[id] = r
	-- native mods read from a cache: give it everything that was just loaded
	for k, v in pairs( r.stats ) do native( "data", id, "stats", k, jsonOf( v ) ) end
	for mod, m in pairs( r.mods ) do
		for k, v in pairs( m.d or {} ) do native( "data", id, mod, k, jsonOf( v ) ) end
		native( "data", id, mod, "@xp", jsonOf( m.xp or 0 ) )
		native( "data", id, mod, "@level", jsonOf( m.lv or 1 ) )
		local ul = {}
		for u in pairs( m.u or {} ) do ul[#ul + 1] = u end
		table.sort( ul )
		native( "data", id, mod, "@unlocks", listJson( ul ) )
	end
	return r
end
D.__rec = rec

local function modRec( r, mod )
	local m = r.mods[mod]
	if not m then m = { d = {}, xp = 0, lv = 1, u = {} } r.mods[mod] = m end
	m.d = m.d or {} m.u = m.u or {} m.xp = m.xp or 0 m.lv = m.lv or 1
	return m
end

-- A value changed: save later, sync to the owner, update public data, mirror
-- to native mods. key may be "@xp", "@level" or "@unlocks" for progression.
local function changed( r, mod, key, value )
	r.dirty = true
	r.pending = r.pending or {}
	r.pending[#r.pending + 1] = { m = mod, k = key, v = value }
	if mod == "stats" or ( D.public[mod] and D.public[mod][key] ) then r.pubDirty = true end
	native( "data", r.id, mod, key, jsonOf( value ) )
end

local function fire( list, ... ) for _, fn in ipairs( list or {} ) do safeCall( fn, ... ) end end

-- ---------------------------------------------------------------------------
-- Public API: mod data (host writes; get works on the host for any player)
-- ---------------------------------------------------------------------------
function D.get( player, mod, key )
	if isHost() then
		local r = rec( player )
		local m = r and r.mods[mod]
		if key == nil then return m and m.d or {} end
		return m and m.d[key]
	end
	if pidOf( player ) == NET.localPlayerId() then return D.mine( mod, key ) end
	local pub = D.publicOf( pidOf( player ) )
	local pm = pub and pub.pub and pub.pub[mod]
	if key == nil then return pm or {} end
	return pm and pm[key]
end

function D.set( player, mod, key, value )
	if not hostOnly( "SMData.set" ) then return false end
	if ty( mod ) ~= "string" or ty( key ) ~= "string" then error( "SMData.set: mod and key must be strings", 2 ) end
	local r = rec( player )
	if not r then return false end
	local m = modRec( r, mod )
	if m.d[key] == value and ty( value ) ~= "table" then return true end
	m.d[key] = value
	changed( r, mod, key, value )
	fire( D.changeListeners[mod], player, key, value )
	return true
end

function D.add( player, mod, key, n )
	local v = ( tonumber( D.get( player, mod, key ) ) or 0 ) + ( n or 1 )
	if D.set( player, mod, key, v ) then return v end
	return nil
end

-- Make a mod's key visible to every player (stats are always public).
function D.publish( mod, key ) slot( D.public, mod )[key] = true end

-- ---------------------------------------------------------------------------
-- Stats
-- ---------------------------------------------------------------------------
function D.stat( player, name )
	if isHost() then local r = rec( player ) return r and r.stats[name] or 0 end
	if pidOf( player ) == NET.localPlayerId() then return D.myStat( name ) end
	local pub = D.publicOf( pidOf( player ) )
	return pub and pub.stats and pub.stats[name] or 0
end

function D.addStat( player, name, n )
	if not hostOnly( "SMData.addStat" ) then return nil end
	local r = rec( player )
	if not r then return nil end
	local v = ( r.stats[name] or 0 ) + ( n or 1 )
	r.stats[name] = v
	changed( r, "stats", name, v )
	return v
end

function D.setStat( player, name, value )
	if not hostOnly( "SMData.setStat" ) then return false end
	local r = rec( player )
	if not r then return false end
	r.stats[name] = value
	changed( r, "stats", name, value )
	return true
end

-- All-time leaderboard: { { id, name, value }, ... } highest first. On the host
-- it includes offline players (from the index); on clients, players online now.
function D.leaderboard( stat, count )
	local rows, seen = {}, {}
	local function add( id, name, stats )
		if seen[id] then return end
		seen[id] = true
		local v = stats and tonumber( stats[stat] )
		if v then rows[#rows + 1] = { id = id, name = name, value = v } end
	end
	if isHost() then
		for id, r in pairs( H.recs ) do add( id, r.name, r.stats ) end
		for id, e in pairs( loadIndex().players ) do add( tonumber( id ) or id, e.name, e.stats ) end
	else
		for k, e in pairs( NET.getShared( D.MOD ) ) do
			if ty( e ) == "table" then add( tonumber( k ) or k, e.name, e.stats ) end
		end
	end
	table.sort( rows, function( a, b ) return a.value > b.value end )
	if count then while #rows > count do rows[#rows] = nil end end
	return rows
end

-- ---------------------------------------------------------------------------
-- Progression: XP, levels, unlocks
-- ---------------------------------------------------------------------------
-- curve: { base = 100, growth = 1.5, max = 100 } -> XP for the next level is
-- floor(base * level ^ growth); or { fn = function( level ) return xp end, max = 50 }.
-- Register it on every machine (it's used to show levels locally too).
function D.setLevelCurve( mod, curve ) D.curves[mod] = curve end

local function needFor( mod, lv )
	local c = D.curves[mod] or {}
	if ty( c.fn ) == "function" then
		local ok, v = pcall( c.fn, lv )
		return ok and math.max( 1, math.floor( tonumber( v ) or 1 ) ) or 1
	end
	return math.max( 1, math.floor( ( c.base or 100 ) * lv ^ ( c.growth or 1.5 ) ) )
end
local function maxLevel( mod ) local c = D.curves[mod] return c and c.max or 100 end

-- level, xp into this level, xp needed for the next (0 at max level)
function D.levelOf( mod, xp )
	xp = math.max( 0, math.floor( tonumber( xp ) or 0 ) )
	local lv, cap = 1, maxLevel( mod )
	while lv < cap do
		local need = needFor( mod, lv )
		if xp < need then return lv, xp, need end
		xp = xp - need
		lv = lv + 1
	end
	return cap, 0, 0
end

function D.xp( player, mod )
	if not isHost() and pidOf( player ) == NET.localPlayerId() then return D.myXP( mod ) end
	local r = isHost() and rec( player )
	local m = r and r.mods[mod]
	return m and m.xp or 0
end
function D.level( player, mod ) return D.levelOf( mod, D.xp( player, mod ) ) end

-- Returns newLevel, levelsGained.
function D.addXP( player, mod, amount )
	if not hostOnly( "SMData.addXP" ) then return nil, 0 end
	amount = tonumber( amount ) or 0
	local r = rec( player )
	if not r or amount == 0 then return r and modRec( r, mod ).lv or 1, 0 end
	local m = modRec( r, mod )
	local old = m.lv
	m.xp = math.max( 0, m.xp + amount )
	m.lv = ( D.levelOf( mod, m.xp ) )
	changed( r, mod, "@xp", m.xp )
	changed( r, mod, "@level", m.lv )           -- always, so readers see level 1 too
	if m.lv > old then fire( D.levelListeners[mod], player, m.lv, old ) end
	return m.lv, m.lv - old
end

-- Returns true the first time; false if already unlocked.
function D.unlock( player, mod, id )
	if not hostOnly( "SMData.unlock" ) then return false end
	local r = rec( player )
	if not r then return false end
	local m = modRec( r, mod )
	if m.u[id] then return false end
	m.u[id] = os.time()
	local list = {}
	for k in pairs( m.u ) do list[#list + 1] = k end
	table.sort( list )
	changed( r, mod, "@unlocks", list )
	fire( D.unlockListeners[mod], player, id )
	return true
end
function D.isUnlocked( player, mod, id )
	if not isHost() and pidOf( player ) == NET.localPlayerId() then
		for _, u in ipairs( D.myUnlocks( mod ) ) do if u == id then return true end end
		return false
	end
	local r = isHost() and rec( player )
	local m = r and r.mods[mod]
	return m ~= nil and m.u[id] ~= nil
end
function D.unlocks( player, mod )
	local r = isHost() and rec( player )
	local m = r and r.mods[mod]
	local list = {}
	if m then for k in pairs( m.u ) do list[#list + 1] = k end end
	table.sort( list )
	return list
end

-- Host events
function D.onChange( mod, fn ) local s = slot( D.changeListeners, mod ) s[#s + 1] = fn end
function D.onLevelUp( mod, fn ) local s = slot( D.levelListeners, mod ) s[#s + 1] = fn end
function D.onUnlock( mod, fn ) local s = slot( D.unlockListeners, mod ) s[#s + 1] = fn end

-- ---------------------------------------------------------------------------
-- Own data on every machine (synced from the host)
-- ---------------------------------------------------------------------------
local function mineMod( mod ) local m = C.mine and C.mine.mods and C.mine.mods[mod] return m end
function D.mine( mod, key )
	local m = mineMod( mod )
	if key == nil then return m and m.d or {} end
	return m and m.d and m.d[key]
end
function D.myStat( name ) return C.mine and C.mine.stats and C.mine.stats[name] or 0 end
function D.myStats() return C.mine and C.mine.stats or {} end
function D.myXP( mod ) local m = mineMod( mod ) return m and m.xp or 0 end
function D.myLevel( mod ) return D.levelOf( mod, D.myXP( mod ) ) end
function D.myUnlocks( mod ) local m = mineMod( mod ) return m and m.ul or {} end
function D.onMine( fn ) D.mineListeners[#D.mineListeners + 1] = fn end
function D.isSynced() return C.mine ~= nil end
-- Public data of any player: { name, stats = {...}, pub = { [mod] = { key = value } } }
function D.publicOf( id ) local e = NET.getShared( D.MOD, tostring( id ) ) return ty( e ) == "table" and e or nil end

local function unlockList( u ) local l = {} for k in pairs( u or {} ) do l[#l + 1] = k end table.sort( l ) return l end

local function mirrorMine()
	if not C.mine then return end
	local id = C.localId or NET.localPlayerId() or 0
	native( "local", id )
	for k, v in pairs( C.mine.stats or {} ) do native( "data", id, "stats", k, jsonOf( v ) ) end
	for mod, m in pairs( C.mine.mods or {} ) do
		for k, v in pairs( m.d or {} ) do native( "data", id, mod, k, jsonOf( v ) ) end
		native( "data", id, mod, "@xp", jsonOf( m.xp or 0 ) )
		native( "data", id, mod, "@level", jsonOf( m.lv or 1 ) )
		native( "data", id, mod, "@unlocks", listJson( m.ul or {} ) )
	end
end

local function clientApply( mod, key, value )
	C.mine = C.mine or { stats = {}, mods = {} }
	if mod == "stats" then
		C.mine.stats[key] = value
	else
		local m = slot( C.mine.mods, mod )
		m.d = m.d or {}
		if key == "@xp" then m.xp = value
		elseif key == "@level" then m.lv = value
		elseif key == "@unlocks" then m.ul = value
		else m.d[key] = value end
	end
	if not isHost() then native( "data", C.localId or 0, mod, key, jsonOf( value ) ) end
	fire( D.mineListeners, mod, key, value )
end

NET.register( D.MOD, D.VERSION )
NET.onClient( D.MOD, "full", function( snap )
	if ty( snap ) ~= "table" then return end
	C.localId = snap.id
	local mods = {}
	for mod, m in pairs( snap.mods or {} ) do
		mods[mod] = { d = m.d or {}, xp = m.xp or 0, lv = m.lv or 1, ul = unlockList( m.u ) }
	end
	C.mine = { stats = snap.stats or {}, mods = mods }
	if not isHost() then mirrorMine() else native( "local", C.localId ) end
	fire( D.mineListeners, nil, nil, nil )
end )
NET.onClient( D.MOD, "delta", function( list )
	if ty( list ) ~= "table" then return end
	for _, e in ipairs( list ) do clientApply( e.m, e.k, e.v ) end
end )

-- ---------------------------------------------------------------------------
-- Automatic stats (host)
-- ---------------------------------------------------------------------------
-- Survival player tick (observation hook on SurvivalPlayer.server_onFixedUpdate)
function D.__playerTick( ps )
	local s = ps and ps.sv and ps.sv.saved
	if ty( s ) ~= "table" or ty( s.stats ) ~= "table" then return end
	local r = rec( ps.player )
	if not r then return end
	local hp, conscious = tonumber( s.stats.hp ), s.isConscious
	if hp and r.lastHp and r.lastConscious and hp < r.lastHp then D.addStat( ps.player, "damageTaken", r.lastHp - hp ) end
	if r.lastConscious == true and conscious == false then D.addStat( ps.player, "deaths", 1 ) end
	r.lastHp, r.lastConscious = hp, conscious
end

function D.__eat( ps ) if ps and ps.player then D.addStat( ps.player, "foodEaten", 1 ) end end
function D.__harvest( _, _, player ) if ty( player ) == "Player" then D.addStat( player, "cropsHarvested", 1 ) end end

-- Robot hits: remember the attacking player; the kill is confirmed on the next
-- host ticks when the robot's health reaches 0.
D.__hit = {}
local function recordHit( unit, attacker, damage, cls )
	if ty( attacker ) ~= "Player" or ty( unit ) ~= "table" then return end
	local h = H.hits[unit]
	if not h then h = {} H.hits[unit] = h end
	h.p, h.t, h.cls = attacker, os.clock(), cls
	if tonumber( damage ) and damage > 0 then D.addStat( attacker, "robotDamage", damage ) end
end

local function confirmKills()
	local t = os.clock()
	for unit, h in pairs( H.hits ) do
		local ok, hp = pcall( function() return unit.saved.stats.hp end )
		if ok and tonumber( hp ) and hp <= 0 then
			if not unit.__smdKilled then
				unit.__smdKilled = true
				D.addStat( h.p, "kills", 1 )
				D.addStat( h.p, "kills." .. h.cls, 1 )
			end
			H.hits[unit] = nil
		elseif t - h.t > 3 then
			H.hits[unit] = nil
		end
	end
end

-- Look for hookable classes (cheap; repeated every 10 s for late-loaded scripts)
function D.__scan()
	local t = os.clock()
	if H.scanned and t - H.scanAt < 10 then return end
	H.scanAt = t
	H.hooked = H.hooked or {}
	local seenFn = H.seenFn or {}
	H.seenFn = seenFn
	local function hook( target, handler, fn )
		if H.hooked[target] or ty( fn ) ~= "function" or seenFn[fn] then return end
		H.hooked[target], seenFn[fn] = true, true
		NET.addCallbackHook( target, handler )
	end
	local SP = E.SurvivalPlayer
	if ty( SP ) == "table" then
		hook( "SurvivalPlayer.server_onFixedUpdate", "SMData.__playerTick", SP.server_onFixedUpdate )
		hook( "SurvivalPlayer.sv_e_eat", "SMData.__eat", SP.sv_e_eat )
	end
	local MH = E.MatureHarvestable
	if ty( MH ) == "table" then hook( "MatureHarvestable.sv_n_harvest", "SMData.__harvest", MH.sv_n_harvest ) end
	for _, cls in ipairs( { "WocUnit", "BabyWocUnit", "GlobUnit", "LootbotUnit", "SeedbotUnit", "ScannerbotUnit",
	                        "TotebotGreenUnit", "TotebotLeafUnit", "TotebotRedUnit", "TotebotBlueUnit", "TotebotYellowUnit",
	                        "HaybotUnit", "TapebotUnit", "MinerbotUnit", "CablebotUnit", "FarmbotUnit",
	                        "TrashbotUnit", "TrashBallUnit" } ) do
		local U = E[cls]
		if ty( U ) == "table" then
			if not D.__hit[cls .. "_p"] then
				D.__hit[cls .. "_p"] = function( self, _, _, _, _, attacker, damage ) recordHit( self, attacker, damage, cls ) end
				D.__hit[cls .. "_m"] = function( self, _, attacker, damage ) recordHit( self, attacker, damage, cls ) end
			end
			hook( cls .. ".server_onProjectile", "SMData.__hit." .. cls .. "_p", U.server_onProjectile )
			hook( cls .. ".server_onMelee", "SMData.__hit." .. cls .. "_m", U.server_onMelee )
		end
	end
	H.scanned = true
end

-- ---------------------------------------------------------------------------
-- Host tick: presence, movement, saving, syncing
-- ---------------------------------------------------------------------------
local function snapshot( r )
	return { id = r.id, stats = r.stats, mods = r.mods }
end

local function publicEntry( r )
	local pub = {}
	for mod, keys in pairs( D.public ) do
		local m = r.mods[mod]
		if m then for k in pairs( keys ) do if m.d[k] ~= nil then slot( pub, mod )[k] = m.d[k] end end end
	end
	return { name = r.name, stats = r.stats, pub = pub }
end

local function save( r )
	local ok, err = pcall( sm.storage.save, D.CHANNEL_BASE + r.id, { v = 1, name = r.name, stats = r.stats, mods = r.mods } )
	if not ok then log( "saving player " .. r.id .. " failed: " .. tostring( err ) ) return end
	r.dirty = false
	local idx = loadIndex()
	idx.players[tostring( r.id )] = { name = r.name, stats = r.stats }
	H.indexDirty = true
end

local function svTick( game )
	local tick = sm.game.getCurrentTick()
	local t = os.clock()
	confirmKills()
	if H.lastTick and tick - H.lastTick < 40 then return end   -- the rest once per second
	local dt = H.lastTick and math.min( ( tick - H.lastTick ) / 40, 5 ) or 0
	H.lastTick = tick
	local now = os.time()
	local generic = ty( E.SurvivalPlayer ) ~= "table"   -- deaths from the character outside Survival

	local online = {}
	local ok, players = pcall( sm.player.getAllPlayers )
	for _, pl in ipairs( ok and players or {} ) do
		local r = rec( pl )
		if r then
			online[r.id] = true
			local name = nameOf( pl )
			if name and name ~= r.name then r.name = name r.dirty = true end
			if not r.online then
				r.online = true
				r.synced = false
				D.addStat( pl, "sessions", 1 )
				if not r.stats.firstSeen then D.setStat( pl, "firstSeen", now ) end
			end
			if dt > 0 then
				r.stats.playtime = ( r.stats.playtime or 0 ) + dt
				r.dirty = true
				r.playtimeSyncAt = r.playtimeSyncAt or 0
				if t - r.playtimeSyncAt >= 10 then
					r.playtimeSyncAt = t
					changed( r, "stats", "playtime", r.stats.playtime )
					D.setStat( pl, "lastSeen", now )
				end
			end
			local char = pl.character
			if char then
				local okp, pos = pcall( function() return char.worldPosition end )
				if okp and pos then
					if r.pos and dt > 0 then
						local d = ( pos - r.pos ):length()
						if d > 0.05 and d < 120 * dt + 5 then          -- ignore teleports and respawns
							local okS, seated = pcall( function() return char:isSeated() end )
							local stat = ( okS and seated ) and "distanceDriven" or "distanceWalked"
							r.stats[stat] = ( r.stats[stat] or 0 ) + d
							r.dirty = true
							r.moveDirty = true
						end
					end
					r.pos = pos
				end
				if generic then
					local okd, downed = pcall( function() return char:isDowned() end )
					if okd then
						if downed and not r.wasDowned then D.addStat( pl, "deaths", 1 ) end
						r.wasDowned = downed
					end
				end
			else
				r.pos = nil
			end
			if not r.synced and NET.hasMod( pl, D.MOD ) then
				r.synced = true
				r.pending = nil
				NET.sendToClient( pl, D.MOD, "full", snapshot( r ) )
			end
		end
	end
	for id, r in pairs( H.recs ) do
		if r.online and not online[id] then
			r.online, r.pos, r.synced, r.pending = false, nil, false, nil
			D.setStat( id, "lastSeen", now )
			save( r )
		end
	end

	-- private sync (owner) and public data (everyone)
	if t - H.syncAt >= D.SYNC_INTERVAL then
		H.syncAt = t
		for id, r in pairs( H.recs ) do
			if r.moveDirty then
				r.moveDirty = false
				changed( r, "stats", "distanceWalked", r.stats.distanceWalked or 0 )
				changed( r, "stats", "distanceDriven", r.stats.distanceDriven or 0 )
			end
			if r.online and r.synced and r.pending and #r.pending > 0 then
				NET.sendToClient( id, D.MOD, "delta", r.pending )
			end
			r.pending = nil
		end
	end
	if t - H.publicAt >= D.PUBLIC_INTERVAL then
		H.publicAt = t
		for id, r in pairs( H.recs ) do
			if r.online and r.pubDirty then
				r.pubDirty = false
				NET.setShared( D.MOD, tostring( id ), publicEntry( r ) )
			end
		end
	end
	if t - H.saveAt >= D.SAVE_INTERVAL then
		H.saveAt = t
		for _, r in pairs( H.recs ) do if r.dirty then save( r ) end end
		if H.indexDirty then
			H.indexDirty = false
			pcall( sm.storage.save, D.CHANNEL_BASE, loadIndex() )
		end
	end
end
NET.onServerTick( function( game ) if isHost() then svTick( game ) end end )

-- Save everything now (host). Normally automatic every SAVE_INTERVAL seconds.
-- Needs a server callback context (e.g. call it from a server tick).
function D.saveNow()
	if not isHost() then return false end
	for _, r in pairs( H.recs ) do if r.dirty then save( r ) end end
	pcall( sm.storage.save, D.CHANNEL_BASE, loadIndex() )
	H.indexDirty = false
	return true
end

-- ---------------------------------------------------------------------------
-- Native (C++) bridge - called by SMLoader
-- ---------------------------------------------------------------------------
local function decode( json )
	if json == nil or json == "" then return nil end
	local ok, v = pcall( NET.json.decode, json )
	if ok then return v end
	return json                           -- not JSON: keep the raw string
end
function D.__nativeSet( id, mod, key, json )
	if mod == "stats" then return D.setStat( id, key, decode( json ) ) end
	return D.set( id, mod, key, decode( json ) )
end
function D.__nativeAdd( id, mod, key, delta )
	if mod == "stats" then return D.addStat( id, key, delta ) end
	return D.add( id, mod, key, delta )
end
function D.__nativeAddXP( id, mod, amount ) return D.addXP( id, mod, amount ) end
function D.__nativeUnlock( id, mod, uid ) return D.unlock( id, mod, uid ) end

return "installed"
