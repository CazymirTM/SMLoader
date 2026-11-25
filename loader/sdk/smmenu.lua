-- SMModsMenu - the MODS page of the pause menu (global `SMModsMenu`).
--
-- SMLoader compiles this file into winmm.dll and installs it after the other
-- SDK modules in every world. Any mod can have a page of settings here: Lua mods
-- with SMModsMenu.page{...}, native mods with api->MenuAddPage / MenuAddOption.
-- Full guide: docs/MODSMENU.md.
--
-- The page is drawn with the game's own GUI system (sm.jsonGui), owned by the
-- game-mode script: its callbacks are added to that class, never wrapped.
-- Values are saved per PC in SMLoader\config\<page>.cfg. Options with host = true
-- follow the host: its values travel through SMNetworking's shared state.

local E, NX = ...                      -- game env, natives table from the loader
if SMModsMenu and SMModsMenu.__env == E then return "already installed" end
if not ( SMNet and SMX ) then error( "SMModsMenu needs SMNetworking and SMExtensions", 0 ) end
local NET, json = SMNet, SMNet.json

local M = {}
SMModsMenu = M
M.__env = E
M.VERSION = "1.6.0"

local NET_ID = "smmodsmenu"

local function ty( v ) local ok, r = pcall( type, v ) return ok and r or "?" end
local function log( msg ) SMX.log( "[SMModsMenu] " .. tostring( msg ) ) end
local function safe( what, f, ... )
	if not f then return nil end
	local ok, r = pcall( f, ... )
	if not ok then log( what .. " failed: " .. tostring( r ) ) return nil end
	return r
end
-- options may give text, visibility ... as a value or as a function
local function read( v, ... ) if ty( v ) == "function" then return safe( "a page's function", v, ... ) end return v end
local function now() return os.clock() end
local function clamp( v, lo, hi ) return math.max( lo, math.min( hi, v ) ) end
local function round( v ) return math.floor( v + 0.5 ) end

local pages, order = {}, {}            -- [id] = page; list
local listeners = {}                   -- { fn( page, option, value ) }
local overrides = {}                   -- ["page/option"] = the host's value
local published = {}                   -- host: ["page/option"] = JSON last put in the shared state
local ui = { open = false }            -- what is on screen
local dirty = false                    -- redraw on the next tick

-- ---------------------------------------------------------------------------
-- Values
-- ---------------------------------------------------------------------------
local VALUE = { toggle = true, slider = true, choice = true, text = true, key = true, color = true }

-- The value as this option stores it, or nil if it can't be used.
local function fit( o, v )
	local t = ty( v )
	if o.type == "toggle" then
		if t == "boolean" then return v end
	elseif o.type == "slider" then
		if t ~= "number" or v ~= v then return nil end
		if o.step and o.step > 0 then
			v = o.min + math.floor( ( v - o.min ) / o.step + 0.5 ) * o.step
			v = tonumber( string.format( "%.10g", v ) )      -- 0.30000000000000004 -> 0.3
		end
		return clamp( v, o.min, o.max )
	elseif o.type == "choice" then
		for _, c in ipairs( o.choices ) do if c.value == v then return v end end
	elseif o.type == "text" then
		if t == "string" and #v <= 4 * ( o.max or 64 ) then return v end
	elseif o.type == "key" then
		if t ~= "string" then return nil end
		v = v:upper()
		if v == "" or SMX.input.KEYS[v] then return v end
	elseif o.type == "color" then
		if t == "string" and v:match( "^#%x%x%x%x%x%x$" ) then return v:upper() end
	end
	return nil
end

local function hostRules( o ) return o.host and not NET.isHost() end

local function valueOf( p, o )
	local ov = overrides[p.id .. "/" .. o.id]
	if ov ~= nil and hostRules( o ) then return ov end
	local saved = NX.get( p.id, o.id )
	if saved then
		local ok, v = pcall( json.decode, saved )
		if ok then v = fit( o, v ) if v ~= nil then return v end end
	end
	return o.default
end

local function fire( p, o, value )
	if o.onChange then safe( p.id .. "/" .. o.id .. " onChange", o.onChange, value, p.handle ) end
	for _, fn in ipairs( p.listeners ) do safe( p.id .. " onChange", fn, o.id, value ) end
	for _, fn in ipairs( listeners ) do safe( "onChange", fn, p.id, o.id, value ) end
	if SMEvents then SMEvents.emit( "modsmenu.changed", { page = p.id, option = o.id, value = value } ) end
	dirty = true
end

-- Remember what a "needs a restart" option was when the game started.
local function markRestart( p, o, old )
	if not o.restart then return end
	local k = "boot:" .. p.id .. "/" .. o.id
	if not NX.session( k ) then NX.session( k, json.encode( old ) ) end
end
local function restartPending( p, o )
	if not o.restart then return false end
	local boot = NX.session( "boot:" .. p.id .. "/" .. o.id )
	return boot ~= nil and boot ~= json.encode( valueOf( p, o ) )
end

-- Change a value. keep = false while a slider is being dragged: saved later.
local function change( p, o, value, keep )
	if hostRules( o ) and overrides[p.id .. "/" .. o.id] ~= nil then return false end
	local v = fit( o, value )
	if v == nil then return false end
	local old = valueOf( p, o )
	if v == old then
		if keep ~= false then NX.flush() end
		return true
	end
	markRestart( p, o, old )
	NX.set( p.id, o.id, json.encode( v ), keep ~= false )
	if p.native then NX.event( p.id, o.id, "change" ) end
	fire( p, o, v )
	return true
end

-- ---------------------------------------------------------------------------
-- Pages (Lua API)
-- ---------------------------------------------------------------------------
local PAGE = {}                        -- methods of a page handle

local function findOption( p, id )
	for _, o in ipairs( p.options ) do if o.id == id and VALUE[o.type] then return o end end
	return nil
end

local function sortPages()
	table.sort( order, function( a, b )
		if ( a.id == "smloader" ) ~= ( b.id == "smloader" ) then return a.id == "smloader" end
		local na, nb = a.name:lower(), b.name:lower()
		if na ~= nb then return na < nb end
		return a.id < b.id
	end )
end

local function makePage( def, native )
	local p = { id = def.id, name = tostring( def.name or def.id ), version = def.version, author = def.author, desc = def.description or def.desc,
	            native = native, locked = def.locked, options = {}, listeners = {} }
	p.handle = SMX.setmetatable( { id = p.id }, { __index = PAGE, __metatable = false } )
	return p
end

local function addPage( p )
	local old = pages[p.id]
	pages[p.id] = p
	for i = #order, 1, -1 do if order[i] == old then table.remove( order, i ) end end
	order[#order + 1] = p
	sortPages()
	dirty = true
end

local function validId( id ) return ty( id ) == "string" and #id <= 64 and id:match( "^[%w_%-][%w_%-%.]*$" ) ~= nil and id:sub( -1 ) ~= "." end

-- Create a page: SMModsMenu.page{ id = "mymod", name = "My Mod", version = "1.0",
-- author = "me", description = "..." }. Creating it again starts it over (so a
-- script that is reloaded doesn't add its options twice). With a string: the
-- existing page of that id, or nil.
function M.page( def )
	if ty( def ) == "string" then local p = pages[def] return p and not p.locked and p.handle or nil end
	if ty( def ) ~= "table" or not validId( def.id ) then error( "SMModsMenu.page: id must be letters, digits, '_', '-' or '.'", 2 ) end
	local old = pages[def.id]
	if old and ( old.native or old.locked ) then error( "SMModsMenu.page: '" .. def.id .. "' is a native mod's page", 2 ) end
	local p = makePage( def, false )
	addPage( p )
	return p.handle
end

local function need( self, where )
	local p = pages[self.id]
	if not p or p.handle ~= self then error( "SMModsMenu: " .. where .. ": this page was replaced", 3 ) end
	return p
end

-- Check and complete an option; errors point at the mod's line.
local function checkOption( o, level )
	level = level + 1
	local function bad( msg ) error( "SMModsMenu: option '" .. tostring( o.id ) .. "': " .. msg, level ) end
	if o.type ~= "section" and o.type ~= "info" and not validId( o.id ) then bad( "id must be letters, digits, '_', '-' or '.'" ) end
	if o.label == nil then o.label = o.id or "" end
	if o.type == "slider" then
		if ty( o.min ) ~= "number" or ty( o.max ) ~= "number" or o.max <= o.min then bad( "a slider needs min < max" ) end
		if o.default == nil then o.default = o.min end
	elseif o.type == "choice" then
		if ty( o.choices ) ~= "table" or #o.choices == 0 then bad( "a choice needs choices" ) end
		local list = {}
		for i, c in ipairs( o.choices ) do
			if ty( c ) == "table" then list[i] = { value = c.value, label = tostring( c.label or c.value ) }
			else list[i] = { value = c, label = tostring( c ) } end
			local t = ty( list[i].value )
			if t ~= "string" and t ~= "number" and t ~= "boolean" then bad( "choice values must be strings, numbers or booleans" ) end
		end
		o.choices = list
		if o.default == nil then o.default = list[1].value end
	elseif o.type == "toggle" then
		if o.default == nil then o.default = false end
	elseif o.type == "text" or o.type == "key" then
		if o.default == nil then o.default = "" end
	elseif o.type == "color" then
		if o.default == nil then o.default = "#FFFFFF" end
	elseif o.type == "button" then
		if o.onClick ~= nil and ty( o.onClick ) ~= "function" then bad( "onClick must be a function" ) end
	end
	if VALUE[o.type] then
		local d = fit( o, o.default )
		if d == nil then bad( "the default doesn't fit the option" ) end
		o.default = d
		if o.onChange ~= nil and ty( o.onChange ) ~= "function" then bad( "onChange must be a function" ) end
	end
	return o
end

local function addOption( self, kind, def )
	local p = need( self, kind )
	if ty( def ) == "string" or ty( def ) == "function" then def = { label = def } end
	if ty( def ) ~= "table" then error( "SMModsMenu: " .. kind .. " needs a table", 3 ) end
	local o = {}
	for k, v in pairs( def ) do o[k] = v end
	o.type = kind
	if kind == "info" and o.text ~= nil then o.label = o.text end
	checkOption( o, 3 )
	if o.id then
		for i, x in ipairs( p.options ) do if x.id == o.id then p.options[i] = o dirty = true return self end end
	end
	p.options[#p.options + 1] = o
	dirty = true
	return self
end
for _, kind in ipairs( { "section", "info", "toggle", "slider", "choice", "text", "key", "color", "button" } ) do
	PAGE[kind] = function( self, def ) return addOption( self, kind, def ) end
end

-- The value that applies right now (the host's, for host options on other machines).
function PAGE.get( self, id )
	local p = need( self, "get" )
	local o = findOption( p, id )
	if not o then error( "SMModsMenu: page '" .. p.id .. "' has no setting '" .. tostring( id ) .. "'", 2 ) end
	return valueOf( p, o )
end
-- Change a value as if the player had. Returns false if it doesn't fit or the host decides.
function PAGE.set( self, id, value )
	local p = need( self, "set" )
	local o = findOption( p, id )
	if not o then error( "SMModsMenu: page '" .. p.id .. "' has no setting '" .. tostring( id ) .. "'", 2 ) end
	return change( p, o, value )
end
-- Back to the default: one setting, or all of them.
local function resetPage( p, id )
	for _, o in ipairs( p.options ) do
		if VALUE[o.type] and ( id == nil or o.id == id ) then change( p, o, o.default, false ) end
	end
	NX.flush()
end
function PAGE.reset( self, id ) resetPage( need( self, "reset" ), id ) end
-- fn( id, value ) for every change on this page
function PAGE.onChange( self, fn ) local p = need( self, "onChange" ) p.listeners[#p.listeners + 1] = fn return self end
-- All current values: { id = value }
function PAGE.values( self )
	local p, t = need( self, "values" ), {}
	for _, o in ipairs( p.options ) do if VALUE[o.type] then t[o.id] = valueOf( p, o ) end end
	return t
end
function PAGE.open( self ) return M.open( self.id ) end
-- Draw the page again (if a text or visible function would give something else now)
function PAGE.refresh() dirty = true end
function PAGE.remove( self )
	local p = need( self, "remove" )
	pages[p.id] = nil
	for i = #order, 1, -1 do if order[i] == p then table.remove( order, i ) end end
	dirty = true
end

-- Any page's value by ids (also native mods'); nil if there is no such setting.
function M.get( page, id )
	local p = pages[page]
	local o = p and findOption( p, id )
	if o then return valueOf( p, o ) end
	return nil
end
function M.set( page, id, value )
	local p = pages[page]
	local o = p and not p.locked and findOption( p, id )
	return o ~= nil and o ~= false and change( p, o, value ) or false
end
-- fn( page, option, value ) for every change anywhere
function M.onChange( fn ) listeners[#listeners + 1] = fn end
function M.isOpen() return ui.open end
-- Show the MODS page (opens on the next tick). page: an id, or nil for the last one shown.
function M.open( page ) ui.want = page or "" return true end
local closed
function M.close()
	if not ui.gui then return end
	safe( "close", function() ui.gui:close() end )
	if ui.open then closed() end
end
-- { { id, name, version, author }, ... } as listed
function M.pages()
	local t = {}
	for i, p in ipairs( order ) do t[i] = { id = p.id, name = p.name, version = p.version, author = p.author } end
	return t
end

-- ---------------------------------------------------------------------------
-- Native mods' pages
-- ---------------------------------------------------------------------------
local nativeDirty = true
local function pullNative()
	nativeDirty = false
	for _, def in ipairs( NX.pages() ) do
		local old = pages[def.id]
		local p = makePage( def, true )
		for _, o in ipairs( def.options ) do
			local ok, err = pcall( checkOption, o, 1 )
			if ok then p.options[#p.options + 1] = o else log( err ) end
		end
		if old then p.listeners = old.listeners end
		addPage( p )
	end
end
function M.__nativePages() nativeDirty = true end
function M.__wake() end
-- A native mod changed one of its values (already saved, its handler already ran).
function M.__fromNative( page, option )
	if nativeDirty then pullNative() end
	local p = pages[page]
	local o = p and findOption( p, option )
	if o then fire( p, o, valueOf( p, o ) ) end
end

-- ---------------------------------------------------------------------------
-- Host options
-- ---------------------------------------------------------------------------
NET.register( NET_ID, "1" )

-- Host: keep the shared state equal to our host options. Cheap: compares JSON.
local publishAt = -100
local function publish()
	if now() - publishAt < 0.5 or not NET.isHost() then return end
	publishAt = now()
	for _, p in ipairs( order ) do
		for _, o in ipairs( p.options ) do
			if o.host and VALUE[o.type] then
				local k, j = p.id .. "/" .. o.id, json.encode( valueOf( p, o ) )
				if published[k] ~= j then published[k] = j NET.setShared( NET_ID, k, j ) end
			end
		end
	end
end

-- Others: the host's value applies, whatever is saved here.
local function fromHost( key, j )
	local page, option = key:match( "^([^/]+)/(.+)$" )
	local p = page and pages[page]
	local o = p and findOption( p, option )
	if not o or not o.host then return end
	local old = valueOf( p, o )
	local v
	if ty( j ) == "string" then
		local ok, d = pcall( json.decode, j )
		if ok then v = fit( o, d ) end
	end
	overrides[key] = v
	NX.override( page, option, v ~= nil and json.encode( v ) or nil )
	local new = valueOf( p, o )
	if new ~= old then
		if p.native then NX.event( p.id, o.id, "change" ) end
		fire( p, o, new )
	end
end
local sharedDirty = false
NET.onShared( NET_ID, function( key, value )
	if NET.isHost() then return end
	if key == nil then sharedDirty = true else fromHost( key, value ) end
end )
local function pullShared()
	sharedDirty = false
	if NET.isHost() then return end
	for key, value in pairs( NET.getShared( NET_ID ) ) do fromHost( key, value ) end
end

-- ---------------------------------------------------------------------------
-- The page on screen: building blocks
-- ---------------------------------------------------------------------------
-- Positions are in the GUI's 1280x720 space.
local W_, H_ = 1100, 590
local HEAD, FOOT, SIDE = 76, 44, 268
local CONTENT_X, CONTENT_W = SIDE + 44, W_ - SIDE - 44 - 26
local ROW_W = CONTENT_W - 62                        -- room for the scroll bar
local LABEL_W, CTRL_X = 316, 336
local CTRL_W = ROW_W - CTRL_X - 8
local WHITE, GREY, AMBER, DIM = "1 1 1 1", "0.62 0.64 0.68 1", "1 0.831 0.290 1", "0.45 0.46 0.49 1"
local PALETTE = { "#FFFFFF", "#FF3B30", "#FF9500", "#FFD60A", "#34C759", "#00C7BE", "#0A84FF", "#BF5AF2" }

local CB = { click = "cl_smmenu_click", press = "cl_smmenu_press", drag = "cl_smmenu_drag", release = "cl_smmenu_release",
             edit = "cl_smmenu_edit", enter = "cl_smmenu_enter", close = "cl_smmenu_close" }

local function W( kind, skin, name, x, y, w, h, extra )
	local t = { Type = kind, Skin = skin, Name = name, x = x, y = y, width = w, height = h, Childs = {}, NeedKey = false, NeedMouse = false }
	-- a widget that neither takes nor passes on the mouse hides its children from it
	if skin == "PanelEmpty" then t.InheritsPick = true end
	for k, v in pairs( extra or {} ) do t[k] = v end
	return t
end
local function Text( name, x, y, w, h, caption, font, colour, align )
	return W( "TextBox", "TextBox", name, x, y, w, h,
		{ Caption = caption, FontName = font or "SM_Text", TextAlign = align or "Left VCenter", TextColour = colour or WHITE } )
end
local function Para( name, x, y, w, h, caption, font, colour )
	return W( "EditBox", "EditBoxEmpty", name, x, y, w, h, { Caption = caption, FontName = font, TextAlign = "Left Top", TextColour = colour,
		Static = true, MultiLine = true, WordWrap = true } )
end
local function Button( name, x, y, w, h, caption, on, enabled, skin )
	-- the disabled look has no "selected": a locked choice is shown as a faded normal one
	local locked = enabled == false
	return W( "Button", skin or "SettingsButton", name, x, y, w, h, { Caption = caption, FontName = "SM_ButtonSmall", TextAlign = "Center",
		NeedMouse = not locked, onClick = CB.click, StateSelected = on == true, Enabled = not locked or on == true, Alpha = locked and on and 0.45 or 1 } )
end
local function Edit( name, x, y, w, h, text, font, colour, align, max, enabled )
	local e = W( "EditBox", "EditBoxEmpty", name, 10, round( ( h - 24 ) / 2 ), w - 20, 24, { Caption = text, CaptionDisableReplacing = true,
		FontName = font, TextColour = colour, MaxTextLength = max, MultiLine = false, WordWrap = false, Static = not enabled, NeedKey = enabled,
		NeedMouse = enabled, TextAlign = align, onTextEdit = CB.edit, onTextEnter = CB.enter } )
	return W( "Widget", "SearchBarBackground", name .. "_bg", x, y, w, h, { NeedMouse = true, Childs = { e } } )
end
-- '#' starts a colour code in the game's texts
local function plain( s ) return ( tostring( s or "" ):gsub( "#", "##" ) ) end

local function textHeight( text, font, width, line )
	local ok, _, h = pcall( sm.gui.computeTextSize, text, font, "Left", width, true )
	if ok and ty( h ) == "number" and h > 0 then return math.ceil( h ) end
	return line * math.max( 1, math.ceil( #text * line * 0.42 / width ) )
end

local function colourOf( hex )
	local r, g, b = hex:match( "^#(%x%x)(%x%x)(%x%x)$" )
	if not r then return WHITE end
	return string.format( "%.4f %.4f %.4f 1", tonumber( r, 16 ) / 255, tonumber( g, 16 ) / 255, tonumber( b, 16 ) / 255 )
end

local function numberText( o, v )
	if o.format then local ok, s = pcall( string.format, o.format, v ) if ok then return s end end
	local d = 0
	local s = o.step and o.step > 0 and o.step or ( o.max - o.min ) / 100
	while d < 4 and math.abs( s * 10 ^ d - round( s * 10 ^ d ) ) > 1e-6 do d = d + 1 end
	return string.format( "%." .. d .. "f", v ) .. ( o.unit and tostring( o.unit ) or "" )
end

-- Can the player change this option right now? Otherwise also why not.
local function usable( p, o )
	if hostRules( o ) then return false, "host" end
	local e = o.enabledBy
	if ty( e ) == "function" then return read( e, p.handle ) ~= false and true or false end
	if ty( e ) == "string" and e ~= "" then
		local neg = e:sub( 1, 1 ) == "!"
		local dep = findOption( p, neg and e:sub( 2 ) or e )
		if dep then
			local on = valueOf( p, dep )
			if on == nil then on = false end
			if ( on ~= false ) == neg then return false, "off" end
		end
	end
	return true
end
local function shown( p, o )
	if o.visible == nil then return true end
	local v = read( o.visible, p.handle )
	return v ~= false and v ~= nil
end

-- ---------------------------------------------------------------------------
-- Option rows. Widget names: SmmO<index>_<part>; index into ui.rows.
-- ---------------------------------------------------------------------------
local SLIDER_W, HANDLE_W = CTRL_W - 92, 28

local control = {}
function control.toggle( p, o, n, y, on )
	local v = valueOf( p, o )
	local w = 92
	return { Button( n .. "_on", CTRL_X, y, w, 34, "#{ON_CAPS}", v == true, on ), Button( n .. "_off", CTRL_X + w + 8, y, w, 34, "#{OFF_CAPS}", v == false, on ) }
end
function control.slider( p, o, n, y, on )
	local v = valueOf( p, o )
	local f = clamp( ( v - o.min ) / ( o.max - o.min ), 0, 1 )
	local x0 = CTRL_X + 92
	local hx = round( f * ( SLIDER_W - HANDLE_W ) )
	local ev = on and { NeedMouse = true, onPressed = CB.press, onDrag = CB.drag, onReleased = CB.release } or {}
	local draft = ui.draft and ui.draft.o == o and ui.draft.text
	local c = { Edit( n .. "_num", CTRL_X, y, 84, 34, draft or numberText( o, v ), "SM_ListItem", on and AMBER or DIM, "Center", 16, on ) }
	local mid = y + 17
	c[2] = W( "Button", "LargeHorizontalSliderBackground", n .. "_track", x0, mid - 7, SLIDER_W, 13, ev )
	c[3] = W( "Widget", "LargeHorizontalSliderProgress", n .. "_fill", x0, mid - 7, math.max( 8, hx + round( HANDLE_W / 2 ) ), 12, { Alpha = on and 1 or 0.35, Visible = f > 0 } )
	c[4] = W( "Button", "LargeHorizontalSliderHandle", n .. "_handle", x0 + hx, mid - 17, HANDLE_W, 32, ev )
	c[4].Alpha = on and 1 or 0.35          -- always set: a missing property keeps its old value
	c[4].Cursor = on and "Draggable" or "Point"
	return c
end
function control.choice( p, o, n, y, on )
	local v = valueOf( p, o )
	local label = ""
	for _, c in ipairs( o.choices ) do if c.value == v then label = c.label end end
	if #o.choices == 2 and #o.choices[1].label <= 14 and #o.choices[2].label <= 14 then
		local w = round( ( CTRL_W - 8 ) / 2 )
		return { Button( n .. "_pick1", CTRL_X, y, w, 34, o.choices[1].label, v == o.choices[1].value, on ),
		         Button( n .. "_pick2", CTRL_X + w + 8, y, w, 34, o.choices[2].label, v == o.choices[2].value, on ) }
	end
	local box = W( "Button", "SettingsButton", n .. "_next", CTRL_X + 42, y, CTRL_W - 84, 34, { NeedMouse = true, onClick = CB.click, Enabled = on,
		Caption = label, FontName = "SM_ButtonSmall", TextAlign = "Center" } )
	return { Button( n .. "_prev", CTRL_X, y, 36, 34, "<", false, on ), box, Button( n .. "_next2", CTRL_X + CTRL_W - 36, y, 36, 34, ">", false, on ) }
end
function control.text( p, o, n, y, on )
	local draft = ui.draft and ui.draft.o == o and ui.draft.text
	return { Edit( n .. "_text", CTRL_X, y, CTRL_W, 34, draft or valueOf( p, o ), "SM_Text", on and WHITE or DIM, "Left VCenter", o.max or 64, on ) }
end
function control.key( p, o, n, y, on )
	local v = valueOf( p, o )
	local waiting = ui.capture and ui.capture.o == o
	local caption = waiting and "PRESS A KEY" or ( v ~= "" and v or "#{MENU_OPTIONS_CONTROLS_BINDING_NOT_SET}" )
	local c = { Button( n .. "_bind", CTRL_X, y, 192, 34, caption, waiting, on ) }
	if v ~= "" and not waiting then c[2] = Button( n .. "_clear", CTRL_X + 200, y, 80, 34, "CLEAR", false, on ) end
	return c
end
function control.color( p, o, n, y, on )
	local v = valueOf( p, o )
	local draft = ui.draft and ui.draft.o == o and ui.draft.text
	local c = { W( "Widget", "WhiteSkin", n .. "_show", CTRL_X, y, 34, 34, { Colour = colourOf( v ) } ),
	            Edit( n .. "_hex", CTRL_X + 42, y, 104, 34, draft or v, "SM_ListItem", on and WHITE or DIM, "Center", 7, on ) }
	for i, hex in ipairs( PALETTE ) do
		local x = CTRL_X + 154 + ( i - 1 ) * 25
		if x + 20 <= CTRL_X + CTRL_W then
			c[#c + 1] = W( "Widget", "WhiteSkin", n .. "_sw" .. i, x, y + 7, 20, 20, { Colour = colourOf( hex ), Alpha = on and 1 or 0.35 } )
			if on then c[#c + 1] = W( "Button", "ButtonEmptySkin", n .. "_pal" .. i, x, y + 7, 20, 20, { NeedMouse = true, onClick = CB.click } ) end
		end
	end
	return c
end
function control.button( p, o, n, y, on )
	local sure = ui.confirm and ui.confirm.o == o
	return { Button( n .. "_run", CTRL_X, y, 192, 34, sure and "ARE YOU SURE?" or tostring( read( o.caption, p.handle ) or "RUN" ), false, on, sure and "ActiveButton" or nil ) }
end

local function buildRow( p, o, i )
	local n = "SmmO" .. i
	local label = tostring( read( o.label, p.handle ) or "" )
	if o.type == "section" then
		return W( "Widget", "PanelEmpty", n, 0, 0, ROW_W, i == 1 and 34 or 52, { Childs = {
			Text( n .. "_label", 8, i == 1 and 2 or 20, ROW_W - 16, 26, label, "SM_Header", "1 1 1 0.5" ),
			W( "Widget", "WhiteSkin", n .. "_line", 0, i == 1 and 32 or 50, ROW_W - 6, 1, { Alpha = 0.15 } ) } } )
	end
	if o.type == "info" then
		local h = textHeight( label, "SM_TextTiny", ROW_W - 24, 18 ) + 4
		return W( "Widget", "PanelEmpty", n, 0, 0, ROW_W, h + 14, { Childs = { Para( n .. "_label", 8, 8, ROW_W - 24, h, label, "SM_TextTiny", GREY ) } } )
	end
	local on, why = usable( p, o )
	local desc = tostring( read( o.desc or o.description, p.handle ) or "" )
	local notes = {}
	if why == "host" then notes[1] = "Set by the host."
	elseif o.host then notes[1] = "For everyone in this world." end
	if restartPending( p, o ) then notes[#notes + 1] = "Applies after a game restart." end
	local note = table.concat( notes, " " )
	local dh = desc ~= "" and textHeight( desc, "SM_TextTiny", LABEL_W, 18 ) + 2 or 0
	local nh = note ~= "" and 20 or 0
	local h = math.max( 48, 34 + dh + nh + ( dh + nh > 0 and 8 or 0 ) )
	local c = { Text( n .. "_label", 8, dh + nh > 0 and 7 or 0, LABEL_W, dh + nh > 0 and 26 or 46, label, "SM_TextLabel", on and WHITE or DIM ) }
	if dh > 0 then c[#c + 1] = Para( n .. "_desc", 8, 33, LABEL_W, dh, desc, "SM_TextTiny", on and GREY or DIM ) end
	if nh > 0 then c[#c + 1] = Text( n .. "_note", 8, 31 + dh, LABEL_W, 18, note, "SM_TextTiny", AMBER ) end
	for _, w in ipairs( control[o.type]( p, o, n, round( ( h - 34 ) / 2 ), on ) ) do c[#c + 1] = w end
	c[#c + 1] = W( "Widget", "WhiteSkin", n .. "_line", 0, h - 1, ROW_W - 6, 1, { Alpha = 0.06 } )
	return W( "Widget", "PanelEmpty", n, 0, 0, ROW_W, h, { Childs = c } )
end

local function currentPage()
	local p = ui.page and pages[ui.page]
	if not p then p = order[1] ui.page = p and p.id end
	return p
end

local function build()
	local p = currentPage()
	local root = {}

	-- header
	local head = {
		W( "ImageBox", "ImageBox", "SmmIcon", 30, 26, 24, 24, { ImageTexture = "gui_mainmenu_icon_options.png", Colour = "1 0.780392 0.227451 1" } ),
		Text( "SmmTitle", 68, 12, 400, 52, "MODS", "SM_HeaderLarge_Wide" ),
		Button( "SmmReset", W_ - 290, 21, 130, 34, ui.confirm and ui.confirm.reset and "ARE YOU SURE?" or "#{MENU_OPTION_DEFAULT}", false, true,
			ui.confirm and ui.confirm.reset and "ActiveButton" or "SecondaryButton" ),
		Button( "SmmClose", W_ - 150, 21, 120, 34, "CLOSE", false, true, "PrimaryButton" ),
	}
	root[1] = W( "Widget", "BackgroundDarkRoundedUpperRight", "SmmHead", 0, 0, W_, HEAD, { NeedMouse = true, Childs = head } )

	-- list of mods
	local list = {}
	for i, q in ipairs( order ) do
		local sel = q == p
		local by = {}
		if q.version and q.version ~= "" then by[1] = "v" .. plain( q.version ) end
		if q.author and q.author ~= "" then by[#by + 1] = "by " .. plain( q.author ) end
		local c = { Text( "SmmP" .. i .. "_name", 18, #by > 0 and 6 or 0, SIDE - 50, #by > 0 and 24 or 50, plain( q.name ), "SM_ListItem", sel and AMBER or WHITE ) }
		if #by > 0 then c[2] = Text( "SmmP" .. i .. "_by", 18, 28, SIDE - 50, 16, table.concat( by, "  " ), "SM_TextTiny", GREY ) end
		if sel then c[#c + 1] = W( "Widget", "WhiteSkin", "SmmP" .. i .. "_bar", 0, 0, 4, 50, { Colour = AMBER } ) end
		local b = W( "Button", "SelectionFieldBox", "SmmP" .. i, 0, 0, SIDE - 18, 50, { NeedMouse = true, onClick = CB.click, StateSelected = sel, Childs = c } )
		list[i] = W( "Widget", "PanelEmpty", "SmmPr" .. i, 0, 0, SIDE - 18, 56, { Childs = { b } } )
	end
	local body = { W( "ScrollViewPanel", "LogScrollView", "SmmList", 20, 18, SIDE, H_ - HEAD - FOOT - 24, { LayoutRule = "Vertical", NeedMouse = true, Childs = list } ) }
	body[2] = W( "Widget", "WhiteSkin", "SmmSplit", SIDE + 30, 20, 1, H_ - HEAD - FOOT - 28, { Alpha = 0.1 } )

	-- the page
	ui.rows = {}
	local top = 14
	if p then
		local by = {}
		if p.version and p.version ~= "" then by[1] = "v" .. plain( p.version ) end
		if p.author and p.author ~= "" then by[#by + 1] = "by " .. plain( p.author ) end
		body[#body + 1] = Text( "SmmName", CONTENT_X + 8, top, CONTENT_W - 250, 30, plain( p.name ), "SM_GameName" )
		body[#body + 1] = Text( "SmmBy", CONTENT_X + CONTENT_W - 250, top, 214, 30, table.concat( by, "   " ), "SM_TextTiny", GREY, "Right VCenter" )
		top = top + 34
		local desc = tostring( read( p.desc, p.handle ) or "" )
		if desc ~= "" then
			local h = math.min( textHeight( desc, "SM_TextTiny", ROW_W - 16, 18 ) + 4, 58 )
			body[#body + 1] = Para( "SmmDesc", CONTENT_X + 8, top, ROW_W - 16, h, desc, "SM_TextTiny", GREY )
			top = top + h + 4
		end
		top = top + 6
		local rows = {}
		for _, o in ipairs( p.options ) do
			if shown( p, o ) then
				ui.rows[#ui.rows + 1] = o
				local ok, row = pcall( buildRow, p, o, #ui.rows )
				if ok then rows[#rows + 1] = row else log( p.id .. ": can't draw '" .. tostring( o.id ) .. "': " .. tostring( row ) ) end
			end
		end
		if #rows == 0 then
			rows[1] = W( "Widget", "PanelEmpty", "SmmNone", 0, 0, ROW_W, 60, { Childs = { Text( "SmmNone_t", 8, 0, ROW_W - 16, 60, "This mod has no settings.", "SM_Text", GREY ) } } )
		end
		-- named after the page, so every page starts at the top
		body[#body + 1] = W( "ScrollViewPanel", "LogScrollView", "SmmScroll_" .. p.id:gsub( "[^%w]", "_" ), CONTENT_X, top, CONTENT_W, H_ - HEAD - FOOT - top - 6,
			{ LayoutRule = "Vertical", NeedMouse = true, Childs = rows } )
	end

	-- footer
	local pending = false
	for _, q in ipairs( order ) do for _, o in ipairs( q.options ) do pending = pending or restartPending( q, o ) end end
	body[#body + 1] = W( "Widget", "WhiteSkin", "SmmFootLine", 20, H_ - HEAD - FOOT, W_ - 40, 1, { Alpha = 0.1 } )
	body[#body + 1] = Text( "SmmFoot", CONTENT_X + 8, H_ - HEAD - FOOT + 4, CONTENT_W - 40, FOOT - 10,
		pending and "Restart the game to apply your changes." or "Changes are saved as you make them.", "SM_TextTiny", pending and AMBER or GREY )
	root[2] = W( "Widget", "BackgroundLightRoundedLowerLeft", "SmmBody", 0, HEAD, W_, H_ - HEAD, { NeedMouse = true, Childs = body } )

	return W( "Widget", "PanelEmpty", "BackPanel", 0, 0, W_, H_, { Anchor = "Center", NeedMouse = true, Visible = true, LayoutRule = "Absolute",
		onClose = CB.close, Childs = root } )
end

local function render()
	dirty = false
	if not ui.gui then return end
	local ok, tree = pcall( build )
	if not ok then log( "can't build the page: " .. tostring( tree ) ) return end
	local done, err = pcall( function() ui.gui:render( tree ) end )
	if not done then log( "can't draw the page: " .. tostring( err ) ) end
end

-- ---------------------------------------------------------------------------
-- Player input
-- ---------------------------------------------------------------------------
local function commitDraft()
	local d = ui.draft
	ui.draft = nil
	if not d then return end
	dirty = true
	if not usable( d.p, d.o ) then return end
	if d.o.type == "slider" then
		local v = tonumber( ( d.text:gsub( ",", "." ):match( "%-?%d*%.?%d+" ) ) )
		if v then change( d.p, d.o, clamp( v, d.o.min, d.o.max ) ) end
	elseif d.o.type == "color" then
		local hex = d.text:match( "^%s*#?(%x%x%x%x%x%x)%s*$" )
		if hex then change( d.p, d.o, "#" .. hex ) end
	else
		change( d.p, d.o, d.text )
	end
end

local function rowOf( name )
	local i, part = name:match( "^SmmO(%d+)_(%w+)$" )
	local p = currentPage()
	local o = i and ui.rows and ui.rows[tonumber( i )]
	if not ( p and o ) then return nil end
	return p, o, part, "SmmO" .. i
end

local function step( p, o, dir )
	local v, at = valueOf( p, o ), 1
	for i, c in ipairs( o.choices ) do if c.value == v then at = i end end
	change( p, o, o.choices[( at - 1 + dir ) % #o.choices + 1].value )
end

-- Needs a second click within 3 seconds.
local function confirmed( key )
	local c = ui.confirm
	ui.confirm = nil
	if c and ( c.o == key or c.reset == key ) then return true end
	ui.confirm = key == true and { reset = true, at = now() } or { o = key, at = now() }
	return false
end

local function onClick( self, name )
	if ty( name ) ~= "string" then return end
	dirty = true
	ui.capture = nil
	if name ~= "SmmReset" and not name:match( "_run$" ) then ui.confirm = nil end
	if name == "SmmClose" then M.close() return end
	local idx = tonumber( name:match( "^SmmP(%d+)$" ) )
	if idx then
		commitDraft()
		if order[idx] then ui.page = order[idx].id NX.session( "page", ui.page ) end
		return
	end
	local cp = currentPage()
	if name == "SmmReset" then
		if cp and confirmed( true ) then ui.draft = nil resetPage( cp ) end
		return
	end
	local p, o, part = rowOf( name )
	if not p or not usable( p, o ) then return end
	if ui.draft and ui.draft.o == o then ui.draft = nil else commitDraft() end
	if part == "on" then change( p, o, true )
	elseif part == "off" then change( p, o, false )
	elseif part == "prev" then step( p, o, -1 )
	elseif part == "next" or part == "next2" then step( p, o, 1 )
	elseif part == "pick1" then change( p, o, o.choices[1].value )
	elseif part == "pick2" then change( p, o, o.choices[2].value )
	elseif part == "bind" then ui.capture = { p = p, o = o, at = now() }
	elseif part == "clear" then change( p, o, "" )
	elseif part == "run" then
		if not o.confirm or confirmed( o ) then
			if p.native then NX.event( p.id, o.id, "click" )
			else safe( p.id .. "/" .. o.id .. " onClick", o.onClick, p.handle ) end
			if SMEvents then SMEvents.emit( "modsmenu.clicked", { page = p.id, option = o.id } ) end
		end
	else
		local i = tonumber( part:match( "^pal(%d+)$" ) )
		if i and PALETTE[i] then change( p, o, PALETTE[i] ) end
	end
end

local function slideTo( x )
	local s = ui.slide
	if not s or not ui.gui then return end
	local ok, left = pcall( function() return ( ui.gui:getWidgetAbsolutePosition( s.n .. "_track" ) ) end )
	if not ok or ty( left ) ~= "number" then return end
	local f = clamp( ( x - s.grab - left - HANDLE_W / 2 ) / ( SLIDER_W - HANDLE_W ), 0, 1 )
	change( s.p, s.o, s.o.min + f * ( s.o.max - s.o.min ), false )
	dirty = true
end
local function onPress( self, name, x, y, button )
	local p, o, part, n = rowOf( ty( name ) == "string" and name or "" )
	if not p or o.type ~= "slider" or ( button or 0 ) ~= 0 or not usable( p, o ) then return end
	ui.draft, ui.confirm, ui.capture = nil, nil, nil
	ui.slide = { p = p, o = o, n = n, grab = 0 }
	if part == "handle" then
		local ok, left = pcall( function() return ( ui.gui:getWidgetAbsolutePosition( n .. "_track" ) ) end )
		local f = ( valueOf( p, o ) - o.min ) / ( o.max - o.min )
		if ok and ty( left ) == "number" then ui.slide.grab = x - left - HANDLE_W / 2 - f * ( SLIDER_W - HANDLE_W ) end
	else
		slideTo( x )
	end
end
local function onDrag( self, name, x ) slideTo( x ) end
local function onRelease( self, name, x )
	if not ui.slide then return end
	slideTo( x )
	ui.slide = nil
	NX.flush()
end

local function onEdit( self, name, text )
	local p, o = rowOf( ty( name ) == "string" and name or "" )
	if not p or not usable( p, o ) then return end
	if ui.draft and ui.draft.o ~= o then commitDraft() end
	ui.draft = { p = p, o = o, text = tostring( text or "" ), at = now() }
	-- text is shown by the box itself while typing: no redraw
end
local function onEnter( self, name, text )
	onEdit( self, name, text )
	commitDraft()
end

function closed()
	if not ui.open then return end
	commitDraft()
	ui.open, ui.gui, ui.slide, ui.capture, ui.confirm, ui.draft = false, nil, nil, nil, nil, nil
	ui.closedAt = now()
	NX.flush()
	if SMEvents then SMEvents.emit( "modsmenu.closed" ) end
end

local function show( page )
	if ui.gui then
		if page ~= "" and pages[page] then ui.page = page dirty = true end
		return
	end
	ui.page = page ~= "" and pages[page] and page or ui.page or NX.session( "page" )
	-- first time: the first mod that has something to set, not the loader itself
	if not ( ui.page and pages[ui.page] ) then ui.page = order[2] and order[2].id or ( order[1] and order[1].id ) end
	local ok, gui = pcall( sm.jsonGui.createGui, { isInteractive = true, needsCursor = true, bNeedsCursor = true, hidesHotbar = true, name = "SMModsMenu" } )
	if not ok or not gui then log( "can't create the GUI: " .. tostring( gui ) ) return end
	ui.gui, ui.open = gui, true
	render()
	if SMEvents then SMEvents.emit( "modsmenu.opened" ) end
end

-- ---------------------------------------------------------------------------
-- Every client tick (inside the game-mode script's callback sandbox)
-- ---------------------------------------------------------------------------
local held = {}                        -- keys of key options that are down
local function pollKeys()
	local blocked = ui.open or ( ui.closedAt and now() - ui.closedAt < 0.2 )
	local busy
	for _, p in ipairs( order ) do
		for _, o in ipairs( p.options ) do
			if o.type == "key" and ( p.native or o.onPress ) then
				local v = valueOf( p, o )
				local down = v ~= "" and SMX.input.isKeyDown( v )
				if down and not held[o] and not blocked then
					if busy == nil then local ok, r = pcall( sm.gui.hasActiveGui ) busy = ok and r == true end
					if not busy then
						if p.native then NX.event( p.id, o.id, "press" )
						else safe( p.id .. "/" .. o.id .. " onPress", o.onPress, p.handle ) end
					end
				end
				held[o] = down or nil
			end
		end
	end
end

local CAPTURE_SKIP = { MOUSE1 = true, MOUSE2 = true, ESC = true }
local function pollCapture()
	local c = ui.capture
	if not c then return end
	if not c.ready then                   -- wait until nothing is held
		for name in pairs( SMX.input.KEYS ) do if SMX.input.isKeyDown( name ) then return end end
		c.ready = true
		return
	end
	for name in pairs( SMX.input.KEYS ) do
		if not CAPTURE_SKIP[name] and SMX.input.isKeyDown( name ) then
			ui.capture = nil
			held[c.o] = true               -- the press that binds it doesn't trigger it
			change( c.p, c.o, name )
			dirty = true
			return
		end
	end
end

NET.onClientTick( function( game )
	if nativeDirty then pullNative() end
	if sharedDirty then pullShared() end
	publish()
	local want = NX.takeOpen() or ui.want
	ui.want = nil
	if want then show( want ) end
	if ui.open then
		pollCapture()
		if ui.confirm and now() - ui.confirm.at > 3 then ui.confirm = nil dirty = true end
		if dirty then render() end
	else
		dirty = false
	end
	pollKeys()
end )

-- ---------------------------------------------------------------------------
-- Callbacks on the game-mode class (called by SMLoader every second; cheap)
-- ---------------------------------------------------------------------------
local GAME_CLASSES = { "SurvivalGame", "CreativeGame", "CreativeTerrainGame", "CreativeFlatGame",
                       "ClassicCreativeGame", "CreativeCustomGame", "ChallengeGame", "Game" }
local function guard( f ) return function( ... ) safe( "input", f, ... ) end end
local handlers = { [CB.click] = guard( onClick ), [CB.press] = guard( onPress ), [CB.drag] = guard( onDrag ), [CB.release] = guard( onRelease ),
                   [CB.edit] = guard( onEdit ), [CB.enter] = guard( onEnter ), [CB.close] = guard( closed ) }
local scanAt = -100
function M.__scan()
	if now() - scanAt < 10 then return end
	scanAt = now()
	for _, name in ipairs( GAME_CLASSES ) do
		local cls = E[name]
		if ty( cls ) == "table" then
			for k, f in pairs( handlers ) do if cls[k] ~= f then cls[k] = f end end
		end
	end
end

return "installed"
