// Co-op menu: the title menu and, in a game, the pause menu. Pages are
// built here from plain Panels, Buttons and Labels. The menu progs
// (coop/src/menu/main.qc) answers "maps", "binds", "servers" and "roster";
// cvars are read through the bridge and commands go out with Bulwark.Cmd.
// The Hero page asks the progs for a 3D preview ("preview"), drawn under
// this view in the see-through #HeroWindow.
var CoopMenu = ( function ()
{
	var stage = $( "#Stage" );
	var page = $( "#Page" );
	var body = $( "#PageBody" );
	var footer = $( "#PageFooter" );
	var inGame = false;
	var current = "";
	var keyHandler = null;	// the open page's own key handling
	var wheelHandler = null;

	// ---- helpers ---------------------------------------------------------

	function query( request )
	{
		try { return fte_query( request ); } catch ( e ) { return ""; }
	}
	function cvar( name ) { return query( "cvar:" + name ); }
	function cvarNum( name, fallback )
	{
		var v = parseFloat( cvar( name ) );
		return isNaN( v ) ? fallback : v;
	}
	function cmd( text ) { Bulwark.Cmd( text ); }
	function quote( s ) { return "\"" + String( s ).replace( /"/g, "'" ) + "\""; }

	function make( type, parent, cls, props )
	{
		props = props || {};
		if ( cls )
			props[ "class" ] = cls;
		return $.CreatePanel( type, parent, "", props );
	}
	function label( parent, cls, text )
	{
		return make( "Label", parent, cls, { text: text, hittest: "false" } );
	}
	function button( parent, cls, text, fn )
	{
		var b = make( "Button", parent, cls, { hittest: "true" } );
		label( b, "", text );
		b.SetPanelEvent( "onactivate", function ()
		{
			$.GetContextPanel().SetFocus();
			fn();
		} );
		return b;
	}
	function section( parent, text )
	{
		var s = make( "Panel", parent, "section", { hittest: "false" } );
		label( s, "", text );
		return s;
	}

	// ---- settings rows ---------------------------------------------------
	// Every row edits a pending value; Apply/OK writes them with seta.

	var pending = {};
	var restartVideo = false;

	function value( name, fallback )
	{
		if ( pending.hasOwnProperty( name ) )
			return pending[ name ];
		var v = cvar( name );
		return v === "" ? fallback : v;
	}
	function setPending( name, v, video )
	{
		pending[ name ] = String( v );
		if ( video )
			restartVideo = true;
	}

	function row( parent, name )
	{
		var r = make( "Panel", parent, "row", { hittest: "true" } );
		label( r, "row-name", name );
		return make( "Panel", r, "row-control", { hittest: "false" } );
	}

	// a value box with a fill bar and - / + steppers
	function sliderRow( parent, name, cvarName, min, max, step, fallback, format )
	{
		var ctl = row( parent, name );
		var minus = button( ctl, "btn small", "-", function () { change( -step ); } );
		var box = make( "Panel", ctl, "row-value", { hittest: "false" } );
		var bar = make( "Panel", box, "row-bar", { hittest: "false" } );
		var text = label( box, "", "" );
		var plus = button( ctl, "btn small", "+", function () { change( step ); } );

		function show()
		{
			var v = parseFloat( value( cvarName, fallback ) );
			if ( isNaN( v ) ) v = fallback;
			text.text = format ? format( v ) : String( Math.round( v * 100 ) / 100 );
			bar.style.width = Math.max( 0, Math.min( 100, ( v - min ) / ( max - min ) * 100 ) ) + "%";
		}
		function change( d )
		{
			var v = parseFloat( value( cvarName, fallback ) );
			if ( isNaN( v ) ) v = fallback;
			v = Math.round( ( v + d ) / step ) * step;
			v = Math.max( min, Math.min( max, v ) );
			setPending( cvarName, Math.round( v * 1000 ) / 1000 );
			show();
		}
		show();
	}

	// cycles through choices: [[value, "Label"], ...]
	function choiceRow( parent, name, cvarName, choices, fallback, video, onChange )
	{
		var ctl = row( parent, name );
		var box;
		var text;
		button( ctl, "btn small", "<", function () { step( -1 ); } );
		box = make( "Panel", ctl, "row-value", { hittest: "false" } );
		text = label( box, "", "" );
		button( ctl, "btn small", ">", function () { step( 1 ); } );

		function index()
		{
			var v = value( cvarName, fallback );
			for ( var i = 0; i < choices.length; i++ )
				if ( String( choices[ i ][ 0 ] ) === String( v ) )
					return i;
			return -1;
		}
		function show()
		{
			var i = index();
			text.text = i >= 0 ? choices[ i ][ 1 ] : String( value( cvarName, fallback ) );
		}
		function step( d )
		{
			var i = index();
			i = ( i < 0 ? 0 : i + d + choices.length ) % choices.length;
			setPending( cvarName, choices[ i ][ 0 ], video );
			if ( onChange )
				onChange( choices[ i ][ 0 ] );
			show();
		}
		show();
	}

	function toggleRow( parent, name, cvarName, fallback, onValue, offValue, video )
	{
		onValue = onValue === undefined ? "1" : onValue;
		offValue = offValue === undefined ? "0" : offValue;
		var ctl = row( parent, name );
		var box = make( "Button", ctl, "row-value", { hittest: "true" } );
		var text = label( box, "", "" );

		function isOn() { return String( value( cvarName, fallback ) ) === String( onValue ); }
		function show()
		{
			text.text = isOn() ? "On" : "Off";
			ctl.SetHasClass( "toggle-on", isOn() );
		}
		box.SetPanelEvent( "onactivate", function ()
		{
			$.GetContextPanel().SetFocus();
			setPending( cvarName, isOn() ? offValue : onValue, video );
			show();
		} );
		show();
	}

	function entryRow( parent, name, cvarName, maxchars )
	{
		var ctl = row( parent, name );
		var entry = make( "TextEntry", ctl, "entry", { text: value( cvarName, "" ), maxchars: String( maxchars || 32 ) } );
		return function () { return entry.text; };
	}

	function applyPending()
	{
		for ( var name in pending )
			cmd( "seta " + name + " " + quote( pending[ name ] ) );
		if ( restartVideo )
			cmd( "vid_restart" );
		cmd( "writeconfig" );
		pending = {};
		restartVideo = false;
	}

	// ---- lists with their own paging --------------------------------------

	function pagedList( parent, rowsShown, height )
	{
		var list = make( "Panel", parent, "list", { hittest: "false", style: "height: " + height + "px;" } );
		var pager = make( "Panel", parent, "list-pager", { hittest: "false" } );
		var info = label( pager, "", "" );
		var items = [];
		var first = 0;
		var draw = null;

		function render()
		{
			var count = items.filter( function ( it ) { return !it.divider; } ).length;
			list.RemoveAndDeleteChildren();
			if ( first > Math.max( 0, items.length - rowsShown ) )
				first = Math.max( 0, items.length - rowsShown );
			for ( var i = first; i < items.length && i < first + rowsShown; i++ )
				draw( list, items[ i ], i );
			info.text = items.length > rowsShown ?
				( first + 1 ) + "-" + Math.min( items.length, first + rowsShown ) + " of " + items.length + "   (mouse wheel scrolls)" :
				( count ? count + " shown" : "" );
		}
		return {
			Set: function ( newItems, drawFn ) { items = newItems; draw = drawFn; render(); },
			Scroll: function ( d ) { first = Math.max( 0, first + d ); render(); },
			Refresh: render,
			Items: function () { return items; }
		};
	}

	// ---- pages -------------------------------------------------------------

	function openPage( name, title )
	{
		heroPageOff();
		if ( name !== "lobby" )
		{
			mm = null;
			stage.SetHasClass( "lobby-open", false );
		}
		current = name;
		keyHandler = null;
		wheelHandler = null;
		body.RemoveAndDeleteChildren();
		footer.RemoveAndDeleteChildren();
		$( "#PageTitle" ).text = title;
		page.SetHasClass( "open", true );
		stage.SetHasClass( "paging", true );
		markButtons();
	}
	// Opened from the JTF title menu (layout/qt/jtf-menu.xml): this view
	// shows only the page it was asked for, and leaving the page goes back
	// to the title, which the progs opens again.
	var fromTitle = false;
	// Opened from a Left 4 Dead frame (coop/src/menu/l4d360.qc), in a game
	// too: the frame is the navigation, and leaving the page goes back to it.
	var l4d = false;
	function closePage()
	{
		clearPage();
		if ( l4d || ( fromTitle && !inGame ) )
			Bulwark.Send( "title", "" );
	}
	function clearPage()
	{
		heroPageOff();
		mm = null;
		stage.SetHasClass( "lobby-open", false );
		current = "";
		keyHandler = null;
		wheelHandler = null;
		body.RemoveAndDeleteChildren();
		footer.RemoveAndDeleteChildren();
		page.SetHasClass( "open", false );
		stage.SetHasClass( "paging", false );
		pending = {};
		restartVideo = false;
		Bulwark.Send( "stopservers", "" );
		markButtons();
	}

	// Play: pick a map and the server's settings
	var mapList = null;
	var selectedMap = "";

	function playPage()
	{
		openPage( "play", "Start a co-op game" );
		var cols = make( "Panel", body, "columns", { hittest: "false" } );
		var left = make( "Panel", cols, "column", { hittest: "false", style: "width: 40%; margin-right: 3%;" } );
		var right = make( "Panel", cols, "column", { hittest: "false", style: "width: 57%;" } );

		section( left, "Map" );
		mapList = pagedList( left, 13, 13 * 40 + 2 );
		wheelHandler = function ( d ) { mapList.Scroll( d ); };
		selectedMap = value( "coop_lastmap", "coop_arena" );
		Bulwark.Send( "maps", "" );

		section( right, "Server" );
		var hostname = entryRow( right, "Server name", "hostname", 48 );
		sliderRow( right, "Max players", "maxplayers", 1, 64, 1, 32 );
		choiceRow( right, "Difficulty", "skill", [ [ "1", "Easy" ], [ "2", "Medium" ], [ "3", "Hard" ] ], "2" );
		section( right, "Rules" );
		toggleRow( right, "Survival (no respawns, revive only)", "coop_survival", "0" );
		toggleRow( right, "Monster waves", "coop_waves", "1" );
		sliderRow( right, "Respawn delay", "coop_respawnDelay", 0, 30, 1, 5, function ( v ) { return v + " s"; } );
		toggleRow( right, "XP mod (levels and skills)", "coop_plugins", "xpm", "xpm", "" );
		toggleRow( right, "Allow votes", "mp_allowvote", "1" );

		button( footer, "btn primary", "Start", function ()
		{
			if ( !selectedMap )
				return;
			pending[ "hostname" ] = hostname();
			pending[ "coop_lastmap" ] = selectedMap;
			// the co-op defaults first, then what was picked here on top
			cmd( "exec default_coop.cfg" );
			applyPending();
			cmd( "map " + selectedMap );
		} );
		button( footer, "btn", "Matchmaking", lobbyPage );
		// the title menu has no servers button of its own
		if ( fromTitle )
			button( footer, "btn", "Find servers", serversPage );
		button( footer, "btn", "Back", closePage );
	}

	function showMaps( maps )
	{
		if ( !mapList || current !== "play" )
			return;
		var items = [];
		if ( maps.coop.length )
			items.push( { divider: "Co-op maps" } );
		maps.coop.forEach( function ( m ) { items.push( { map: m } ); } );
		if ( maps.other.length )
			items.push( { divider: "Other maps" } );
		maps.other.forEach( function ( m ) { items.push( { map: m } ); } );
		mapList.Set( items, function ( list, item )
		{
			if ( item.divider )
			{
				var d = make( "Panel", list, "list-divider", { hittest: "false" } );
				label( d, "", item.divider );
				return;
			}
			var r = make( "Button", list, "list-row" + ( item.map === selectedMap ? " selected" : "" ), { hittest: "true" } );
			label( r, "", item.map );
			r.SetPanelEvent( "onactivate", function ()
			{
				$.GetContextPanel().SetFocus();
				selectedMap = item.map;
				mapList.Refresh();
			} );
		} );
	}

	// Hero: the hero and kit to play, saved for every server with heroes on
	var roster = null;

	// #HeroWindow, as fractions of the screen (menu.css .hero-window)
	var HERO_WINDOW = "0.715 0.12 0.225 0.76";

	function heroPageOff()
	{
		if ( current !== "hero" )
			return;
		stage.SetHasClass( "hero-open", false );
		Bulwark.Send( "preview", "" );
	}

	function heroPage()
	{
		openPage( "hero", "Hero" );
		stage.SetHasClass( "hero-open", true );
		var list = make( "Panel", body, "heroes-list title-heroes", { hittest: "true" } );
		var status = label( body, "status", "" );
		body.heroList = list;
		body.heroStatus = status;
		button( footer, "btn", "Back", closePage );
		Bulwark.Send( "roster", "" );
	}

	function showRoster( r )
	{
		roster = r;
		if ( current !== "hero" || !body.heroList )
			return;
		var list = body.heroList;
		var kits = r.items.filter( function ( it ) { return it.type === "kit"; } );
		var shown = kits.filter( function ( k ) { return k.def === r.picked; } )[ 0 ] || kits[ 0 ];
		list.RemoveAndDeleteChildren();

		r.items.filter( function ( it ) { return it.type === "hero"; } ).forEach( function ( h )
		{
			var card = make( "Panel", list, "hero", { hittest: "false" } );
			var head = make( "Panel", card, "hero-head", { hittest: "false" } );
			label( head, "hero-name", h.name );
			label( head, "hero-stats", ( h.role && h.role !== h.name ? h.role + "  ·  " : "" ) +
				h.health + " health  ·  speed " + h.speed );
			if ( h.desc )
				label( card, "hero-desc", h.desc );
			kits.filter( function ( k ) { return k.hero === h.id; } ).forEach( function ( k )
			{
				var picked = k.def === r.picked;
				var row = make( "Button", card, "skill kit" + ( picked ? " current" : "" ), { hittest: "true" } );
				var text = make( "Panel", row, "skill-text", { hittest: "false" } );
				label( text, "skill-name", k.name );
				label( text, "skill-desc", k.desc || "" );
				label( row, "kit-state", picked ? "Picked" : "" );
				row.SetPanelEvent( "onactivate", function ()
				{
					$.GetContextPanel().SetFocus();
					Bulwark.Send( "hero", k.def );
				} );
			} );
		} );

		body.heroStatus.text = r.picked && kits.some( function ( k ) { return k.def === r.picked; } ) ?
			"Saved for every server with heroes on." :
			"No hero picked: servers will ask.";
		if ( shown )
			Bulwark.Send( "preview", shown.def + " screen " + HERO_WINDOW + ( l4d ? " none" : " backdrop" ) );
	}

	// Find servers: the LAN servers for this game, or an address
	var serverList = null;
	var selectedServer = "";

	function serversPage()
	{
		openPage( "servers", "Find servers" );
		section( body, "Servers on your network" );
		var head = make( "Panel", body, "list-head", { hittest: "false" } );
		label( head, "", "Name" ).style.width = "46%";
		label( head, "", "Map" ).style.width = "24%";
		label( head, "", "Players" ).style.width = "14%";
		label( head, "", "Ping" ).style.width = "10%";
		serverList = pagedList( body, 9, 9 * 40 + 2 );
		wheelHandler = function ( d ) { serverList.Scroll( d ); };
		serverList.Set( [], drawServer );
		var status = label( body, "status", "Looking for servers..." );
		section( body, "Join by address" );
		var address = entryRow( body, "Address (host:port)", "coop_lastaddress", 64 );

		button( footer, "btn primary", "Join", function ()
		{
			if ( selectedServer )
				cmd( "connect " + selectedServer );
		} );
		button( footer, "btn", "Join address", function ()
		{
			var a = address().trim();
			if ( !a )
				return;
			cmd( "seta coop_lastaddress " + quote( a ) );
			cmd( "connect " + a );
		} );
		button( footer, "btn", "Refresh", function ()
		{
			status.text = "Looking for servers...";
			Bulwark.Send( "servers", "" );
		} );
		button( footer, "btn", "Back", closePage );
		serverList.status = status;
		Bulwark.Send( "servers", "" );
	}

	function drawServer( list, s )
	{
		var r = make( "Button", list, "list-row" + ( s.address === selectedServer ? " selected" : "" ), { hittest: "true" } );
		label( r, "", s.name || s.address ).style.width = "46%";
		label( r, "", s.map ).style.width = "24%";
		label( r, "", s.players + " / " + s.max ).style.width = "14%";
		label( r, "", String( s.ping ) ).style.width = "10%";
		r.SetPanelEvent( "onactivate", function ()
		{
			$.GetContextPanel().SetFocus();
			if ( selectedServer === s.address )
				cmd( "connect " + s.address );	// a second click joins
			selectedServer = s.address;
			serverList.Refresh();
		} );
	}

	function showServers( servers )
	{
		if ( !serverList || current !== "servers" )
			return;
		serverList.Set( servers, drawServer );
		serverList.status.text = servers.length ? "" : "No servers found yet.";
	}

	// ---- button glyphs: Xelu's prompts (CC0, images/xelu) ---------------
	// an engine key name ("G", "MOUSE2", "GP_LSHOULDER") to its glyph image,
	// "" when there's none (the key's name is shown instead)
	var PAD_GLYPHS = {
		GP_A: "A", GP_B: "B", GP_X: "X", GP_Y: "Y", GP_LSHOULDER: "LB", GP_RSHOULDER: "RB",
		GP_LTRIGGER: "LT", GP_RTRIGGER: "RT", GP_LTHUMB: "Left_Stick_Click", GP_RTHUMB: "Right_Stick_Click",
		GP_START: "Menu", GP_MENU: "Menu", GP_BACK: "View", GP_VIEW: "View", GP_DPAD_UP: "Dpad_Up",
		GP_DPAD_DOWN: "Dpad_Down", GP_DPAD_LEFT: "Dpad_Left", GP_DPAD_RIGHT: "Dpad_Right"
	};
	var KEY_GLYPHS = {
		MOUSE1: "Mouse_Left", MOUSE2: "Mouse_Right", MOUSE3: "Mouse_Middle", SPACE: "Space", SHIFT: "Shift",
		LSHIFT: "Shift", RSHIFT: "Shift", CTRL: "Ctrl", LCTRL: "Ctrl", RCTRL: "Ctrl", ALT: "Alt", LALT: "Alt",
		RALT: "Alt", TAB: "Tab", ENTER: "Enter", ESCAPE: "Esc", BACKSPACE: "Backspace", UPARROW: "Arrow_Up",
		DOWNARROW: "Arrow_Down", LEFTARROW: "Arrow_Left", RIGHTARROW: "Arrow_Right", INS: "Insert", DEL: "Del",
		HOME: "Home", END: "End", PGUP: "Page_Up", PGDN: "Page_Down", CAPSLOCK: "Caps_Lock", TILDE: "Tilda",
		BACKQUOTE: "Tilda", SEMICOLON: "Semicolon", MINUS: "Minus", SLASH: "Slash", "[": "Bracket_Left",
		"]": "Bracket_Right", "'": "Quote", ";": "Semicolon", "-": "Minus", "/": "Slash", "`": "Tilda"
	};
	function glyph( key )
	{
		key = String( key || "" ).toUpperCase();
		if ( PAD_GLYPHS[ key ] )
			return "file://{images}/xelu/xbox/XboxSeriesX_" + PAD_GLYPHS[ key ] + ".png";
		var k = KEY_GLYPHS[ key ] || ( /^([A-Z0-9]|F[1-9]|F1[0-2])$/.test( key ) ? key : "" );
		return k ? "file://{images}/xelu/keyboard/" + k + "_Key_Dark.png" : "";
	}
	// glyphs for "KEY" or a chord "KEY+KEY" into parent, or the text when a
	// key has no glyph
	// Options: Player, Video, Audio, Mouse, Controller, Keys
	var optionsTab = "player";
	var bindsList = null;
	var capture = null;	// the command waiting for a key
	var captureRelease = false;	// still waiting for the click that started it to let go

	var KEYS = [
		[ "+forward", "Move forward" ], [ "+back", "Move back" ], [ "+moveleft", "Strafe left" ],
		[ "+moveright", "Strafe right" ], [ "+jump", "Jump" ], [ "+duck", "Duck" ], [ "+speed", "Walk" ],
		[ "+use", "Use / revive" ], [ "+attack", "Fire" ], [ "+attack2", "Secondary fire" ],
		[ "+reload", "Reload" ], [ "impulse 100", "Flashlight" ], [ "weaplast", "Last weapon (swap slot)" ],
		[ "invnext", "Next weapon" ], [ "invprev", "Previous weapon" ], [ "slot1", "Weapon slot 1" ],
		[ "slot2", "Weapon slot 2" ], [ "slot3", "Weapon slot 3" ], [ "slot4", "Weapon slot 4" ],
		[ "slot5", "Weapon slot 5" ], [ "+showscores", "Scores" ], [ "messagemode", "Chat" ],
		[ "messagemode2", "Team chat" ], [ "cmd selectskills", "Skills" ], [ "cmd shop", "Requisition" ], [ "vote yes", "Vote yes" ],
		[ "vote no", "Vote no" ], [ "impulse 201", "Spray" ],
		[ "impulse 21", "Hero tool" ], [ "impulse 22", "Hero ability 1" ], [ "impulse 23", "Hero ability 2" ],
		[ "impulse 25", "Engage move" ]
	];
	var binds = {};

	// the controller's actions, as Justice Task Force lays them out
	// (coop/src/menu/main.qc Menu_PadLayout); "+coop_lb"/"+coop_rb" are
	// the shoulders, whose release is an ability and both together engage
	var PAD = [
		[ "+attack", "Fire" ], [ "impulse 21", "Hero tool" ], [ "+coop_lb", "Ability 1 (on release)" ],
		[ "+coop_rb", "Ability 2 (on release)" ], [ "+coop_lb+coop_rb", "Engage (both shoulders)" ],
		[ "impulse 24", "Engage" ], [ "+jump", "Jump" ], [ "+duck", "Crouch" ], [ "+coop_x", "Use / reload" ], [ "weaplast", "Swap weapon" ],
		[ "impulse 25", "Engage move" ], [ "+attack2", "Secondary fire" ], [ "cmd shop", "Requisition" ], [ "slot2", "Weapon slot 2" ],
		[ "slot3", "Weapon slot 3" ], [ "slot4", "Weapon slot 4" ], [ "+showscores", "Scores" ], [ "togglemenu", "Menu" ]
	];
	var padList = null;

	// the pad buttons bound to a command, from the binds answer
	function padKeys( command )
	{
		return ( binds[ command ] || "" ).trim().split( /\s+/ ).filter( function ( k )
		{
			return /^GP_/i.test( k );
		} );
	}

	function drawPad()
	{
		if ( !padList )
			return;
		padList.RemoveAndDeleteChildren();
		PAD.forEach( function ( a )
		{
			var keys = a[ 0 ] === "+coop_lb+coop_rb" ?
				( padKeys( "+coop_lb" ).length && padKeys( "+coop_rb" ).length ? [ padKeys( "+coop_lb" )[ 0 ] + "+" + padKeys( "+coop_rb" )[ 0 ] ] : [] ) :
				padKeys( a[ 0 ] );
			if ( !keys.length )
				return;
			var r = make( "Panel", padList, "pad-row", { hittest: "false" } );
			var g = make( "Panel", r, "pad-glyphs", { hittest: "false" } );
			keys.forEach( function ( k )
			{
				k.split( "+" ).forEach( function ( one )
				{
					var src = glyph( one );
					if ( src )
						make( "Image", g, "pad-glyph", { src: src, hittest: "false" } );
					else
						label( g, "pad-key", one );
				} );
			} );
			label( r, "pad-action", a[ 1 ] );
		} );
	}

	function optionsPage( tab )
	{
		optionsTab = tab || optionsTab;
		openPage( "options", "Options" );
		var tabs = make( "Panel", body, "tabs", { hittest: "false" } );
		[ [ "player", "Player" ], [ "video", "Video" ], [ "audio", "Audio" ], [ "mouse", "Mouse" ], [ "pad", "Controller" ], [ "keys", "Keys" ] ]
			.forEach( function ( t )
			{
				var b = button( tabs, "tab" + ( t[ 0 ] === optionsTab ? " current" : "" ), t[ 1 ], function ()
				{
					optionsPage( t[ 0 ] );
				} );
			} );
		var area = make( "Panel", body, "column fill", { hittest: "false", style: "width: 100%;" } );

		if ( optionsTab === "player" )
		{
			var name = entryRow( area, "Name", "name", 32 );
			sliderRow( area, "Field of view", "fov", 70, 120, 1, 90 );
			toggleRow( area, "Fast weapon switch", "hud_fastswitch", "0" );
			toggleRow( area, "Names over teammates", "cg_nameplates", "1" );
			toggleRow( area, "Name and health of who you aim at", "cg_targetID", "1" );
			toggleRow( area, "XP readout", "xpm_hud", "1" );
			toggleRow( area, "Always run", "cl_run", "1" );
			area.readName = name;
		}
		else if ( optionsTab === "video" )
		{
			var modes = [ "1024x768", "1280x720", "1280x1024", "1366x768", "1600x900", "1920x1080", "2560x1440", "3840x2160" ];
			var current = cvar( "vid_width" ) + "x" + cvar( "vid_height" );
			if ( modes.indexOf( current ) < 0 )
				modes.unshift( current );
			pending[ "coop_videomode" ] = pending[ "coop_videomode" ] || current;
			choiceRow( area, "Resolution", "coop_videomode", modes.map( function ( m ) { return [ m, m.replace( "x", " x " ) ]; } ),
				current, true );
			choiceRow( area, "Display", "vid_fullscreen", [ [ "0", "Window" ], [ "1", "Full screen" ], [ "2", "Full screen (desktop)" ] ], "0", true );
			toggleRow( area, "Vertical sync", "vid_vsync", "1" );
			toggleRow( area, "Windows-style font drawing (GDI)", "r_gdifont", "1" );
			label( area, "note", "Resolution and display changes restart the video when you apply them." );
		}
		else if ( optionsTab === "audio" )
		{
			var pct = function ( v ) { return Math.round( v * 100 ) + "%"; };
			sliderRow( area, "Effects volume", "volume", 0, 1, 0.05, 0.7, pct );
			sliderRow( area, "Music volume", "bgmvolume", 0, 1, 0.05, 0.5, pct );
		}
		else if ( optionsTab === "mouse" )
		{
			sliderRow( area, "Sensitivity", "sensitivity", 0.5, 20, 0.5, 3 );
			var pitch = cvarNum( "m_pitch", 0.022 );
			pending[ "coop_invertmouse" ] = pending[ "coop_invertmouse" ] || ( pitch < 0 ? "1" : "0" );
			toggleRow( area, "Invert mouse", "coop_invertmouse", "0" );
			toggleRow( area, "Mouse smoothing", "m_filter", "0" );
		}
		else if ( optionsTab === "pad" )
		{
			toggleRow( area, "Use controller", "joy_enable", "1" );
			toggleRow( area, "Invert look", "joy_invert", "1" );
			sliderRow( area, "Turn speed", "joy_yawspeed", 60, 600, 10, 220 );
			sliderRow( area, "Look up and down speed", "joy_pitchspeed", 60, 600, 10, 160 );
			sliderRow( area, "Stick dead zone", "joy_deadzone", 0, 0.5, 0.05, 0.2 );
			sliderRow( area, "Stick response curve", "joy_curve", 1, 4, 0.25, 2 );
			section( area, "Buttons" );
			padList = make( "Panel", area, "pad-layout", { hittest: "false" } );
			drawPad();
			Bulwark.Send( "binds", PAD.map( function ( k ) { return "\"" + k[ 0 ] + "\""; } ).join( " " ) );
			button( footer, "btn", "Restore layout", function ()
			{
				cmd( "coop_padlayout" );
				Bulwark.Send( "binds", PAD.map( function ( k ) { return "\"" + k[ 0 ] + "\""; } ).join( " " ) );
			} );
		}
		else if ( optionsTab === "keys" )
		{
			label( area, "note", "Click an action, then press a key or mouse button. Escape cancels, Backspace clears." );
			bindsList = pagedList( area, 10, 10 * 50 + 2 );
			wheelHandler = function ( d ) { bindsList.Scroll( d ); };
			drawBinds();
			Bulwark.Send( "binds", KEYS.map( function ( k ) { return "\"" + k[ 0 ] + "\""; } ).join( " " ) );
		}

		button( footer, "btn primary", "OK", function () { applyOptions( area ); closePage(); } );
		button( footer, "btn", "Apply", function () { applyOptions( area ); optionsPage( optionsTab ); } );
		button( footer, "btn", "Cancel", closePage );
	}

	function applyOptions( area )
	{
		if ( area.readName )
			pending[ "name" ] = area.readName();
		if ( pending.hasOwnProperty( "coop_videomode" ) )
		{
			var m = pending[ "coop_videomode" ].split( "x" );
			if ( m.length === 2 && ( m[ 0 ] !== cvar( "vid_width" ) || m[ 1 ] !== cvar( "vid_height" ) ) )
			{
				pending[ "vid_width" ] = m[ 0 ];
				pending[ "vid_height" ] = m[ 1 ];
				restartVideo = true;
			}
			delete pending[ "coop_videomode" ];
		}
		if ( pending.hasOwnProperty( "coop_invertmouse" ) )
		{
			var p = Math.abs( cvarNum( "m_pitch", 0.022 ) );
			pending[ "m_pitch" ] = String( pending[ "coop_invertmouse" ] === "1" ? -p : p );
			delete pending[ "coop_invertmouse" ];
		}
		applyPending();
	}

	function drawBinds()
	{
		bindsList.Set( KEYS, function ( list, k )
		{
			var r = make( "Button", list, "row" + ( capture === k[ 0 ] ? " capture" : "" ), { hittest: "true" } );
			label( r, "row-name", k[ 1 ] );
			var ctl = make( "Panel", r, "row-control", { hittest: "false" } );
			var box = make( "Panel", ctl, "row-value", { hittest: "false" } );
			var keys = ( binds[ k[ 0 ] ] || "" ).trim();
			label( box, "", capture === k[ 0 ] ? "Press a key..." : ( keys ? keys.toUpperCase().split( " " ).join( ", " ) : "-" ) );
			r.SetPanelEvent( "onactivate", function ()
			{
				$.GetContextPanel().SetFocus();
				capture = k[ 0 ];
				captureRelease = true;
				drawBinds();
			} );
		} );
	}

	function captureKey( e )
	{
		if ( !capture )
			return false;
		if ( captureRelease )
		{
			// the button that activated the row is not the key wanted
			if ( !e.down )
				captureRelease = false;
			return true;
		}
		if ( !e.down )
			return true;
		var command = capture;
		if ( e.key === "Escape" )
		{
			capture = null;
		}
		else if ( e.key === "Backspace" )
		{
			( binds[ command ] || "" ).trim().split( " " ).forEach( function ( k ) { if ( k ) cmd( "unbind " + k ); } );
			capture = null;
		}
		else if ( e.bind )
		{
			cmd( "bind " + e.bind + " " + quote( command ) );
			capture = null;
		}
		else
		{
			return true;
		}
		drawBinds();
		cmd( "writeconfig" );
		Bulwark.Send( "binds", KEYS.map( function ( k ) { return "\"" + k[ 0 ] + "\""; } ).join( " " ) );
		return true;
	}

	// Vote: what Nuclide's callvote takes
	function votePage()
	{
		openPage( "vote", "Call a vote" );
		section( body, "Difficulty" );
		var row1 = make( "Panel", body, "row", { hittest: "false" } );
		[ [ "1", "Easy" ], [ "2", "Medium" ], [ "3", "Hard" ] ].forEach( function ( s )
		{
			button( row1, "btn inline", s[ 1 ], function () { cmd( "callvote skill " + s[ 0 ] ); resume(); } );
		} );
		section( body, "Map" );
		var row2 = make( "Panel", body, "row", { hittest: "false" } );
		button( row2, "btn inline", "Restart this map", function () { cmd( "callvote map_restart 0" ); resume(); } );
		button( row2, "btn inline", "Next map", function () { cmd( "callvote nextmap 0" ); resume(); } );
		section( body, "Current vote" );
		var row3 = make( "Panel", body, "row", { hittest: "false" } );
		button( row3, "btn inline", "Vote yes", function () { cmd( "vote yes" ); resume(); } );
		button( row3, "btn inline", "Vote no", function () { cmd( "vote no" ); resume(); } );
		label( body, "note", "Votes need mp_allowvote on the server. Half the players must agree." );
		button( footer, "btn", "Back", closePage );
	}

	// ---- main buttons ------------------------------------------------------

	var mainButtons = [];

	// ---- Matchmaking lobby (Bulwark Live) ---------------------------------
	// Laid out on Halo: Reach's matchmaking lobby (positions from its
	// widescreen overlay, 1152 wide centred in 1280x720, scaled to the
	// 1080-line design height): an image slot, the playlist's title and
	// population, its description, a status line with a timer on the right,
	// the option rows with help text under them, button hints, and the party
	// roster at the top right. Playlist, Map and Party privacy open ODST-style
	// flyouts: a title tab on the row that opened it, the choices starting
	// in that row, and a column at their right for help or an image slot.
	// Layout only: lobby.css has no visual styling.
	// Everything shown comes from the engine's live_state cvar (JSON,
	// src/net/live.c), read four times a second; rows run live_* commands.
	// The matcher, playlists and drop-in are on the server (docs/spec/live.md).

	var mm = null;	// the open lobby's panels, null when closed
	var mmFly = "";	// the open flyout: "playlist", "map", "privacy" or ""
	var mmErrorSeen = -1;
	var mmMadeParty = false;

	function liveState()
	{
		try { return JSON.parse( cvar( "live_state" ) || "{}" ) || {}; } catch ( e ) { return {}; }
	}
	function mmPlaylistId()
	{
		return cvar( "coop_mmPlaylist" ) || "horde";
	}
	function mmPlaylist( st )
	{
		var id = mmPlaylistId();
		var all = st.playlists || [];
		for ( var i = 0; i < all.length; i++ )
			if ( all[ i ].id === id )
				return all[ i ];
		return all[ 0 ] || null;
	}
	function mmClock( secs )
	{
		secs = Math.max( 0, Math.floor( secs ) );
		var m = Math.floor( secs / 60 ), s = secs % 60;
		return m + ":" + ( s < 10 ? "0" : "" ) + s;
	}
	// one list row: name on the left, value on the right, help under the list
	function mmRow( name, help, fn )
	{
		var b = make( "Button", mm.list, "mm-row", { hittest: "true" } );
		b.nameLabel = label( b, "mm-row-name", name );
		b.valueLabel = label( b, "mm-row-value", "" );
		b.help = help;
		b.SetPanelEvent( "onmouseover", function () { mm.hover = b; mmHelp(); } );
		b.SetPanelEvent( "onmouseout", function () { if ( mm.hover === b ) mm.hover = null; mmHelp(); } );
		b.SetPanelEvent( "onactivate", function ()
		{
			$.GetContextPanel().SetFocus();
			if ( !b.BHasClass( "off" ) )
				fn();
		} );
		return b;
	}
	function mmHelp()
	{
		var b = mm.hover || mm.defaultRow;
		mm.help.text = b ? ( typeof b.help === "function" ? b.help() : b.help ) : "";
	}
	function mmMapId()
	{
		return cvar( "coop_mmMap" ) || "";
	}
	// every map of a playlist's modes, once each, with the modes that play it
	function mmMaps( pl )
	{
		var out = [];
		( pl && pl.modes || [] ).forEach( function ( m )
		{
			( m.maps || [] ).forEach( function ( map )
			{
				var e = out.filter( function ( x ) { return x.id === map; } )[ 0 ];
				if ( !e )
					out.push( e = { id: map, modes: [] } );
				e.modes.push( m.name );
			} );
		} );
		return out;
	}

	function lobbyPage()
	{
		openPage( "lobby", "Matchmaking" );
		stage.SetHasClass( "lobby-open", true );
		mmFly = "";
		stage.SetHasClass( "fly-open", false );
		mm = {};

		var root = make( "Panel", body, "mm", { hittest: "false" } );
		make( "Panel", root, "mm-image", { hittest: "false" } );
		label( root, "mm-crumb", "Main Menu" );
		mm.title = label( root, "mm-title", "Matchmaking" );
		mm.population = label( root, "mm-population", "" );
		mm.desc = label( root, "mm-desc", "" );
		mm.status = label( root, "mm-status", "" );
		mm.timer = label( root, "mm-timer", "" );
		mm.list = make( "Panel", root, "mm-list", { hittest: "false" } );
		mm.help = label( root, "mm-help", "" );

		mm.code = label( root, "mm-code", "" );
		var head = make( "Panel", root, "mm-roster-head", { hittest: "false" } );
		mm.rosterTitle = label( head, "mm-roster-title", "My Party" );
		mm.count = label( head, "mm-count", "" );
		var right = make( "Panel", root, "mm-right", { hittest: "false" } );
		mm.roster = make( "Panel", right, "mm-roster", { hittest: "false" } );
		mm.invites = make( "Panel", right, "mm-invites", { hittest: "false" } );

		// behind an open flyout the lobby is dimmed; clicking the dim closes it
		mm.dim = make( "Button", root, "mm-dim", { hittest: "true" } );
		mm.dim.SetPanelEvent( "onactivate", function () { $.GetContextPanel().SetFocus(); if ( mmFly ) mmCloseFly(); } );
		mm.fly = make( "Panel", root, "mm-fly", { hittest: "false" } );

		button( footer, "btn", "Back", function ()
		{
			if ( mmFly )
				mmCloseFly();
			else
				closePage();
		} );
		button( footer, "btn", "Refresh playlists", function () { cmd( "live_playlists" ); } );

		mm.tick = 0;
		mm.listKey = "";
		cmd( "live_playlists" );
		mmDraw( true );
		mmSchedule();
	}

	function mmSchedule()
	{
		$.Schedule( 0.25, function ()
		{
			if ( !mm || current !== "lobby" )
				return;
			mm.tick++;
			mmDraw( false );
			mmSchedule();
		} );
	}

	function mmStart()
	{
		var st = liveState();
		if ( !st.party || st.party.leader !== st.id )
			return;
		if ( st.queue )
			cmd( "live_cancel" );
		else
		{
			var pl = mmPlaylist( st );
			if ( pl )
				cmd( "live_queue " + pl.id + " 1 " + quote( mmMapId() ) );
		}
	}

	function mmLeaderName( party )
	{
		var m = ( party.members || [] ).filter( function ( x ) { return x.id === party.leader; } )[ 0 ];
		return m ? m.name : "the party leader";
	}

	function mmDraw( rebuild )
	{
		if ( !mm )
			return;
		var st = liveState();
		var online = !!st.connected;
		var party = st.party || null;
		var leader = !!( party && party.leader === st.id );
		var pl = mmPlaylist( st );
		var server = cvar( "live_server" );
		var searching = !!st.queue;

		// no party yet: everyone is always in one, if only by themselves
		if ( online && !party && !mmMadeParty )
		{
			cmd( "live_party " + ( pl ? pl.players : 8 ) + " 1" );
			mmMadeParty = true;
		}
		if ( party )
			mmMadeParty = false;
		// not connected: connect if we know where to
		if ( !online && server && mm.tick % 20 === 1 )
			cmd( "live_connect" );
		// keep the population counts fresh
		if ( online && mm.tick % 32 === 16 )
			cmd( "live_playlists" );

		// the playlist: title, population, description
		mm.title.text = pl ? pl.name : "Matchmaking";
		mm.population.text = pl ? mmFacts( pl ) : "";
		mm.desc.text = pl ? pl.description || "" :
			online ? "Waiting for the server's playlists." : "Connect to Bulwark Live to see the playlists.";

		// the rows: options, the playlists, or the server question
		var key = !online && !server ? "server" : "options";
		if ( rebuild || key !== mm.listKey )
		{
			mm.listKey = key;
			mm.list.RemoveAndDeleteChildren();
			mm.hover = null;
			mm.defaultRow = null;
			mm.rows = null;
			if ( key === "options" )
				mmDrawOptions();
			else
				mmDrawServer();
		}
		if ( mm.rows )
		{
			var r = mm.rows;
			r.playlist.valueLabel.text = pl ? pl.name : online ? "Loading..." : "";
			r.playlist.SetHasClass( "off", !online || searching || !leader );
			var mapId = mmMapId();
			var maps = mmMaps( pl );
			if ( mapId && pl && maps.length && !maps.some( function ( x ) { return x.id === mapId; } ) )
			{
				cmd( "seta coop_mmMap \"\"" );	// not on this playlist: any
				mapId = "";
			}
			r.map.valueLabel.text = mapId || "Any";
			r.map.SetHasClass( "off", !online || searching || !leader || maps.length < 2 );
			r.privacy.valueLabel.text = party ? ( ( party.flags & 1 ) ? "Open" : "Invite only" ) : "";
			r.privacy.SetHasClass( "off", !leader || searching );
			r.start.nameLabel.text = searching ? "Stop searching" : "Start matchmaking";
			r.start.SetHasClass( "off", !online || !party || !leader || !pl );
		}
		// rows under an open flyout are covered by it
		if ( mm.rows )
			mm.rows.order.forEach( function ( b, i ) { b.SetHasClass( "covered", !!mmFly && i >= mm.rows.order.indexOf( mm.rows[ mmFly ] ) ); } );
		mm.help.SetHasClass( "covered", !!mmFly );
		if ( mmFly && ( searching || !leader ) )
			mmCloseFly();
		mmHelp();

		// errors from the server, once each, shown on the status line
		if ( st.error_seq !== undefined && st.error_seq !== mmErrorSeen )
		{
			if ( mmErrorSeen >= 0 && st.error )
			{
				mm.error = st.error;
				mm.errorUntil = mm.tick + 20;
			}
			mmErrorSeen = st.error_seq;
		}
		if ( mm.errorUntil && mm.tick > mm.errorUntil )
		{
			mm.error = "";
			mm.errorUntil = 0;
		}
		mmDrawStatus( st, pl, online, server, party, leader );
		mmDrawRoster( st, pl );
		mmDrawInvites( st );
	}

	function mmDrawOptions()
	{
		var playlist = mmRow( "Playlist", "Choose the playlist you want to play.", function () { mmOpenFly( "playlist" ); } );
		var map = mmRow( "Map", "Choose the map you'd like to play, or any.", function () { mmOpenFly( "map" ); } );
		var privacy = mmRow( "Party privacy", "Choose whether anyone can join your party or only players you invite.", function () { mmOpenFly( "privacy" ); } );
		var start = mmRow( "Start matchmaking", function ()
		{
			return liveState().queue ? "Stop searching and stay in the lobby." : "Search for a game in the selected playlist.";
		}, mmStart );
		mm.rows = { playlist: playlist, map: map, privacy: privacy, start: start, order: [ playlist, map, privacy, start ] };
		mm.defaultRow = start;
	}

	// ---- ODST-style flyouts ------------------------------------------------
	// The tab (the row's name) sits on the row that opened it; the choices
	// start in that same row, one per row, the current one first in focus;
	// help goes under the choices (privacy) or in the column at their right
	// (playlist; map, under an image slot). Choosing closes it; so do Back
	// and the tab.
	function mmCloseFly()
	{
		mmFly = "";
		mm.fly.RemoveAndDeleteChildren();
		mm.fly.SetHasClass( "on", false );
		mm.dim.SetHasClass( "on", false );
		stage.SetHasClass( "fly-open", false );
		mmDraw( false );
	}

	function mmOpenFly( kind )
	{
		var st = liveState();
		var pl = mmPlaylist( st );
		var opener = mm.rows[ kind ];
		var row = mm.rows.order.indexOf( opener );
		mmFly = kind;
		mm.fly.RemoveAndDeleteChildren();
		mm.fly.SetHasClass( "on", true );
		mm.dim.SetHasClass( "on", true );
		stage.SetHasClass( "fly-open", true );
		for ( var i = 0; i < 4; i++ )
			mm.fly.SetHasClass( "r" + i, i === row );	// the tab sits on the opening row

		var tab = make( "Button", mm.fly, "mm-fly-tab", { hittest: "true" } );
		label( tab, "mm-fly-title", opener.nameLabel.text + ":" );
		tab.SetPanelEvent( "onactivate", function () { $.GetContextPanel().SetFocus(); mmCloseFly(); } );
		// ODST's panels: the grey block of choices, a line under them and a
		// fade to the block's bottom, and the dark column at their right
		var block = make( "Panel", mm.fly, "mm-fly-block", { hittest: "false" } );
		var items = make( "Panel", block, "mm-fly-items", { hittest: "false" } );
		make( "Panel", block, "mm-fly-line", { hittest: "false" } );
		var fade = make( "Panel", block, "mm-fly-fade", { hittest: "false" } );
		var under = label( fade, "mm-fly-help", "" );
		var side = make( "Panel", mm.fly, "mm-fly-side", { hittest: "false" } );

		var choices, current, show;
		if ( kind === "playlist" )
		{
			choices = ( st.playlists || [] ).map( function ( p ) { return { id: p.id, name: p.name, p: p }; } );
			current = pl ? pl.id : "";
			var text = label( side, "mm-fly-text", "" );
			var facts = label( side, "mm-fly-text", "" );
			show = function ( c ) { text.text = c.p.description || ""; facts.text = mmFacts( c.p ); };
		}
		else if ( kind === "map" )
		{
			choices = [ { id: "", name: "Any", modes: [] } ].concat( mmMaps( pl ).map( function ( m ) { return { id: m.id, name: m.id, modes: m.modes }; } ) );
			current = mmMapId();
			make( "Panel", side, "mm-fly-image", { hittest: "false" } );
			var mtext = label( side, "mm-fly-text", "" );
			show = function ( c )
			{
				mtext.text = c.id ? "Played as " + c.modes.join( " or " ) + "." : "The matchmaker picks the map and the mode.";
			};
		}
		else
		{
			choices = [ { id: "1", name: "Open", help: "Anyone can join your party with its code, and matchmaking fills open seats." },
				{ id: "0", name: "Invite only", help: "Only players you invite can join your party." } ];
			current = st.party && ( st.party.flags & 1 ) ? "1" : "0";
			make( "Panel", side, "mm-fly-icon", { hittest: "false" } );
			show = function ( c ) { under.text = c.help; };
		}

		// block and column heights follow the number of choices (.n1 .. .n8)
		for ( var n = 1; n <= 8; n++ )
		{
			block.SetHasClass( "n" + n, n === Math.min( choices.length, 8 ) );
			side.SetHasClass( "n" + n, n === Math.min( choices.length, 8 ) );
		}
		// one focus bar: the current choice first, then wherever the cursor is
		var focus = null, buttons = [];
		var setFocus = function ( b ) { buttons.forEach( function ( x ) { x.SetHasClass( "focus", x === b ); } ); };
		choices.forEach( function ( c )
		{
			var b = make( "Button", items, "mm-fly-item" + ( c.id === current ? " current" : "" ), { hittest: "true" } );
			buttons.push( b );
			label( b, "mm-fly-name", c.name );
			b.SetPanelEvent( "onmouseover", function () { setFocus( b ); show( c ); } );
			b.SetPanelEvent( "onactivate", function ()
			{
				$.GetContextPanel().SetFocus();
				if ( kind === "playlist" )
				{
					cmd( "seta coop_mmPlaylist " + c.id );
					cmd( "seta coop_mmMap \"\"" );
				}
				else if ( kind === "map" )
					cmd( "seta coop_mmMap " + quote( c.id ) );
				else
					cmd( "live_open " + c.id );
				mmCloseFly();
			} );
			if ( c.id === current || !focus )
				focus = { c: c, b: b };
		} );
		if ( focus )
		{
			setFocus( focus.b );
			show( focus.c );
		}
		mmDraw( false );
	}

	function mmDrawServer()
	{
		label( mm.list, "mm-ask-text", "Matchmaking goes through a Bulwark Live server (bwlive-server). Type its address." );
		var r = make( "Panel", mm.list, "mm-ask-row", { hittest: "false" } );
		var e = make( "TextEntry", r, "mm-entry", { maxchars: "64", text: cvar( "live_server" ) } );
		var go = make( "Button", r, "mm-small", { hittest: "true" } );
		label( go, "", "Connect" );
		go.SetPanelEvent( "onactivate", function ()
		{
			var a = e.text.trim();
			if ( a )
			{
				cmd( "seta live_server " + quote( a ) );
				cmd( "live_connect " + a );
				mmDraw( true );
			}
		} );
	}

	function mmFacts( pl )
	{
		var min = pl.min_players || pl.players;
		var range = min < pl.players ? min + "-" + pl.players : "" + pl.players;
		var facts = range + ( pl.teams > 1 ? " players, " + pl.teams + " teams" : pl.players === 1 ? " player" : " players" );
		facts += "  ·  " + ( pl.online !== undefined ? pl.online : pl.searching || 0 ) + " online";
		return facts;
	}

	// the status line over the rows, and the timer at its right
	function mmDrawStatus( st, pl, online, server, party, leader )
	{
		var q = st.queue, match = st.match, text = "", timer = "";
		if ( mm.error )
			text = mm.error;
		else if ( !online )
			text = server ? "Connecting to " + server + "..." : "No Bulwark Live server set.";
		else if ( match && !q )
			text = match.host === st.id ? "Readying map..." : "Joining the game...";
		else if ( q )
		{
			text = q.stage === 0 ? "Searching for the closest players..." : "Widening the search (stage " + ( q.stage + 1 ) + ")...";
			timer = mmClock( ( q.waited || 0 ) + ( mm.tick % 4 ) * 0.25 );
		}
		else if ( party && !leader )
			text = "Waiting for " + mmLeaderName( party ) + " to start the game.";
		else if ( party )
			text = "Ready";
		mm.status.text = text;
		mm.timer.text = timer;
	}

	// one row per player: a console's own player, then its guests
	function mmDrawRoster( st, pl )
	{
		var rows = [];
		var match = st.match || null;
		if ( match && match.seats && match.seats.length )
		{
			mm.rosterTitle.text = "My Game";
			match.seats.forEach( function ( s )
			{
				rows.push( { name: s.name, guest: s.slot > 0, me: s.id === st.id, host: s.id === match.host && s.slot === 0 } );
			} );
		}
		else
		{
			mm.rosterTitle.text = "My Party";
			if ( st.party )
				( st.party.members || [] ).forEach( function ( m )
				{
					rows.push( { name: m.name, me: m.id === st.id, leader: m.id === st.party.leader } );
					( m.guests || [] ).forEach( function ( g ) { rows.push( { name: g, guest: true, me: m.id === st.id } ); } );
				} );
		}
		var size = pl ? pl.players : 8;
		mm.count.text = rows.length + "/" + Math.max( size, rows.length );
		mm.code.text = st.party ? "Party code " + st.party.code + ( ( st.party.flags & 1 ) ? "" : " (invite only)" ) : "";
		var sig = JSON.stringify( rows ) + size;
		if ( sig === mm.rosterSig )
			return;
		mm.rosterSig = sig;
		mm.roster.RemoveAndDeleteChildren();
		var shown = Math.min( Math.max( size, rows.length ), 8 );
		for ( var i = 0; i < shown; i++ )
		{
			var p = rows[ i ];
			var r = make( "Panel", mm.roster, "mm-player" + ( !p ? " empty" : p.me ? " me" : "" ), { hittest: "false" } );
			// Reach's row: rank slot, name, then the emblem slot at the right
			var rank = make( "Panel", r, "mm-rank", { hittest: "false" } );
			if ( !p )
			{
				label( r, "mm-pname", pl && pl.drop_in && match ? "Open: drop-in" : "Open" );
				continue;
			}
			label( rank, "mm-initial", ( p.name || "?" ).charAt( 0 ).toUpperCase() );
			label( r, "mm-pname", p.name );
			label( r, "mm-tag", p.leader ? "Leader" : p.host ? "Host" : p.guest ? "Guest" : "" );
			make( "Panel", r, "mm-emblem", { hittest: "false" } );
		}
	}

	function mmDrawInvites( st )
	{
		var inv = st.invites || [];
		var sig = JSON.stringify( inv );
		if ( sig === mm.invSig )
			return;
		mm.invSig = sig;
		mm.invites.RemoveAndDeleteChildren();
		inv.forEach( function ( i )
		{
			var r = make( "Panel", mm.invites, "mm-invite", { hittest: "false" } );
			label( r, "mm-invite-text", i.name + " invited you to their party" );
			var go = make( "Button", r, "mm-small", { hittest: "true" } );
			label( go, "", "Join" );
			go.SetPanelEvent( "onactivate", function () { cmd( "live_join " + i.code ); } );
		} );
	}

	function resume() { Bulwark.Send( "close", "" ); }

	function buildMain()
	{
		var list = $( "#MainButtons" );
		list.RemoveAndDeleteChildren();
		mainButtons = [];
		var items = inGame ?
			[ [ "", "Resume", resume ], [ "", "Requisition", function () { cmd( "cmd shop" ); } ], [ "vote", "Vote", votePage ], [ "options", "Options", function () { optionsPage(); } ],
			  [ "-" ], [ "", "Disconnect", function () { cmd( "disconnect" ); } ], [ "", "Quit", function () { cmd( "quit" ); } ] ] :
			[ [ "lobby", "Matchmaking", lobbyPage ], [ "play", "Play", playPage ], [ "hero", "Hero", heroPage ], [ "servers", "Find servers", serversPage ], [ "options", "Options", function () { optionsPage(); } ],
			  [ "-" ], [ "", "Quit", function () { cmd( "quit" ); } ] ];
		items.forEach( function ( it )
		{
			if ( it[ 0 ] === "-" )
			{
				make( "Panel", list, "main-gap", { hittest: "false" } );
				return;
			}
			var b = button( list, "main-button", it[ 1 ], it[ 2 ] );
			b.pageName = it[ 0 ];
			mainButtons.push( b );
		} );
		markButtons();
	}

	function markButtons()
	{
		mainButtons.forEach( function ( b ) { b.SetHasClass( "current", b.pageName !== "" && b.pageName === current ); } );
	}

	// ---- events --------------------------------------------------------------

	Bulwark.On( "context", function ( c )
	{
		inGame = !!( c && c.ingame );
		stage.SetHasClass( "ingame", inGame );
		l4d = !!( c && c.l4d );
		stage.SetHasClass( "l4d", l4d );
		fromTitle = ( !!( c && c.title ) && !inGame ) || l4d;
		stage.SetHasClass( "fromtitle", fromTitle );
		clearPage();
		buildMain();
	} );
	// the title menu's buttons: play, hero, settings
	Bulwark.On( "page", function ( name )
	{
		if ( name === "play" )
			playPage();
		else if ( name === "hero" )
			heroPage();
		else if ( name === "settings" )
			optionsPage();
		else if ( name === "servers" )
			serversPage();
		else if ( name === "controller" )
			optionsPage( "pad" );
		else if ( name === "vote" )
			votePage();
		else if ( name === "lobby" )
			lobbyPage();
	} );
	Bulwark.On( "maps", showMaps );
	Bulwark.On( "servers", showServers );
	Bulwark.On( "roster", showRoster );
	Bulwark.On( "binds", function ( b )
	{
		binds = b || {};
		if ( bindsList && current === "options" && optionsTab === "keys" )
			drawBinds();
		if ( current === "options" && optionsTab === "pad" )
			drawPad();
	} );
	Bulwark.On( "key", function ( e )
	{
		if ( !e )
			return;
		if ( captureKey( e ) )
			return;
		if ( !e.down )
			return;
		if ( e.key === "WheelUp" && wheelHandler )
			wheelHandler( -3 );
		else if ( e.key === "WheelDown" && wheelHandler )
			wheelHandler( 3 );
		else if ( e.key === "Escape" )
		{
			if ( current )
				closePage();
			else if ( inGame )
				resume();
		}
	} );

	buildMain();

	return {};
} )();
