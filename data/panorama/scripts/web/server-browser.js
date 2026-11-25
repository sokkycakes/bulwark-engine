// The server browser (layout/web/server-browser.xml). Data from the engine's
// "sb_..." requests (docs/spec/serverbrowser.md); the page filters, sorts
// and scrolls. Keys, mouse and controller: every action has all three.
var Servers = ( function ()
{
	var SND_NAV = "misc/menu1", SND_OK = "misc/menu2", SND_BACK = "misc/menu3";
	var TABS = [ "internet", "lan", "favorites", "history" ];
	var TAB_NAMES = [ "Internet", "LAN", "Favorites", "History" ];
	var SORTS = [ "Ping", "Players", "Name", "Map" ];
	var VISIBLE = 11;
	var FILTERS = 5;
	var MODE_NAMES = { duel: "Duel", deathmatch: "Deathmatch", teamdm: "Team DM", domination: "Domination",
		lastmanstanding: "Last Man", invasion: "Invasion", singleplayer: "Co-op", dm_lobby: "Lobby" };
	var STATE_NAMES = { WARMUP: "Waiting for fighters", COUNTDOWN: "Starting", LIVE: "Fight in progress",
		ROUND_END: "Between rounds", MATCH_END: "Set over", INTERMISSION: "Between maps" };

	var tab = 0;
	var zone = "list";          // "list" or "filters"
	var filterSel = 0;
	var mode = "";              // "" = all
	var hideEmpty = false, hideFull = false, hideLocked = false;
	var sort = 0;
	var data = null;
	var shown = [];             // the filtered, sorted rows
	var selAddr = "";
	var sel = 0, top = 0;
	var rowPanels = [];
	var lastRowsKey = "";
	var details = null, detailsAddr = "", detailsAt = 0;
	var dialog = null;          // { kind: "password" | "address", addr, name, btn }
	var joining = null;
	var gamepad = false;
	var noticeUntil = 0;
	var history = [];
	var params = parseQuery();

	function q( req )
	{
		if ( typeof fte_query !== "function" )
			return null;
		try { return fte_query( req ); } catch ( e ) { return null; }
	}
	function cbuf( c ) { return q( "cbuf:" + c ) === "ok"; }
	function sound( s ) { q( "localsound:" + s ); }
	function show( id, on ) { $( id ).SetHasClass( "hidden", !on ); }
	function json( raw, def ) { try { var v = JSON.parse( raw || "" ); return v === null ? def : v; } catch ( e ) { return def; } }

	function parseQuery()
	{
		var out = {};
		var s = typeof location !== "undefined" && location.search ? String( location.search ) : "";
		s.replace( /^\?/, "" ).split( "&" ).forEach( function ( kv )
		{
			if ( !kv ) return;
			var i = kv.indexOf( "=" );
			var k = i < 0 ? kv : kv.slice( 0, i ), v = i < 0 ? "" : kv.slice( i + 1 );
			v = v.replace( /\+/g, " " );
			try { out[ k ] = decodeURIComponent( v ); } catch ( e ) { out[ k ] = v; }
		} );
		return out;
	}

	function modeName( m )
	{
		if ( !m ) return "-";
		if ( MODE_NAMES[ m ] ) return MODE_NAMES[ m ];
		return m.charAt( 0 ).toUpperCase() + m.slice( 1 );
	}

	// a server's refusal, in the browser's words
	function friendlyReason( r )
	{
		r = String( r || "" );
		if ( /password/i.test( r ) ) return "wrong or missing password";
		if ( /full/i.test( r ) ) return "the server is full";
		if ( /banned/i.test( r ) ) return "you're banned from that server";
		if ( /version|protocol/i.test( r ) ) return "it runs a different version";
		if ( /no response|no answer/i.test( r ) || !r ) return "no answer from the server";
		return r.toLowerCase();
	}

	function notice( text, secs )
	{
		$( "#Notice" ).text = text;
		noticeUntil = Date.now() + ( secs || 5 ) * 1000;
		show( "#Notice", true );
	}

	// --- the list ---------------------------------------------------------

	function inTab( s )
	{
		switch ( TABS[ tab ] )
		{
			case "internet": return s.internet;
			case "lan": return s.lan;
			case "favorites": return s.favorite;
			case "history": return s.history;
		}
		return true;
	}

	function passes( s )
	{
		if ( !inTab( s ) ) return false;
		if ( mode && s.gametype !== mode ) return false;
		if ( hideEmpty && s.players + s.bots === 0 ) return false;
		if ( hideFull && s.max && s.players + s.bots >= s.max ) return false;
		if ( hideLocked && s.password ) return false;
		return true;
	}

	function compare( a, b )
	{
		// servers that can't be joined go last
		if ( a.compatible !== b.compatible ) return a.compatible ? -1 : 1;
		if ( TABS[ tab ] === "history" && sort === 0 )
			return history.indexOf( a.addr ) - history.indexOf( b.addr );
		switch ( SORTS[ sort ] )
		{
			case "Players": return ( b.players - a.players ) || ( a.ping - b.ping );
			case "Name": return a.name.toLowerCase() < b.name.toLowerCase() ? -1 : a.name.toLowerCase() > b.name.toLowerCase() ? 1 : 0;
			case "Map": return a.map < b.map ? -1 : a.map > b.map ? 1 : a.ping - b.ping;
		}
		return a.ping - b.ping;
	}

	function rebuild()
	{
		var servers = data && data.servers ? data.servers : [];
		shown = servers.filter( passes );
		shown.sort( compare );
		// keep the selection on the same server
		var i = -1;
		for ( var k = 0; k < shown.length; k++ )
			if ( shown[ k ].addr === selAddr ) i = k;
		sel = i >= 0 ? i : Math.min( sel, Math.max( 0, shown.length - 1 ) );
		selAddr = shown[ sel ] ? shown[ sel ].addr : "";
		clampScroll();
	}

	function clampScroll()
	{
		if ( sel < top ) top = sel;
		if ( sel >= top + VISIBLE ) top = sel - VISIBLE + 1;
		top = Math.max( 0, Math.min( top, Math.max( 0, shown.length - VISIBLE ) ) );
	}

	function pingClass( p ) { return p < 60 ? "good" : p < 140 ? "ok" : "bad"; }

	function makeRows()
	{
		var parent = $( "#Rows" );
		for ( var i = 0; i < VISIBLE; i++ )
		{
			var r = $.CreatePanel( "Button", parent, "Row" + i, { class: "row" + ( i % 2 ? " odd" : "" ) } );
			var cells = {};
			[ "lock", "name", "mode", "map", "players", "ping" ].forEach( function ( c )
			{
				cells[ c ] = $.CreatePanel( "Label", r, "", { class: "c-" + c, text: "", hittest: "false" } );
			} );
			( function ( slot )
			{
				r.SetPanelEvent( "onactivate", function () { clickRow( slot ); } );
				r.SetPanelEvent( "onmouseover", function () { } );
			} )( i );
			rowPanels.push( { panel: r, cells: cells } );
		}
	}

	function renderRows()
	{
		var key = top + "|" + sel + "|" + zone + "|" + JSON.stringify( shown.slice( top, top + VISIBLE ) );
		if ( key === lastRowsKey )
			return;
		lastRowsKey = key;
		for ( var i = 0; i < VISIBLE; i++ )
		{
			var rp = rowPanels[ i ], s = shown[ top + i ];
			rp.panel.SetHasClass( "hidden", !s );
			if ( !s )
				continue;
			var c = rp.cells;
			c.lock.text = ( s.password ? "LOCK" : "" ) + ( s.favorite && !s.password ? "FAV" : "" );
			c.name.text = s.name;
			c.mode.text = modeName( s.gametype );
			c.map.text = s.map || "-";
			c.players.text = s.players + ( s.bots ? "+" + s.bots : "" ) + " / " + s.max;
			c.ping.text = String( s.ping );
			c.ping.SetHasClass( "good", pingClass( s.ping ) === "good" );
			c.ping.SetHasClass( "ok", pingClass( s.ping ) === "ok" );
			c.ping.SetHasClass( "bad", pingClass( s.ping ) === "bad" );
			rp.panel.SetHasClass( "full", s.max && s.players + s.bots >= s.max );
			rp.panel.SetHasClass( "bad-version", !s.compatible );
			rp.panel.SetHasClass( "is-selected", zone === "list" && top + i === sel );
		}
		// scroll bar
		var track = 550;
		var thumb = shown.length > VISIBLE ? Math.max( 40, track * VISIBLE / shown.length ) : track;
		var at = shown.length > VISIBLE ? ( track - thumb ) * top / ( shown.length - VISIBLE ) : 0;
		$( "#ScrollThumb" ).style.height = thumb.toFixed( 0 ) + "px";
		$( "#ScrollThumb" ).style.transform = "translateY( " + at.toFixed( 0 ) + "px )";
		show( "#ScrollTrack", shown.length > VISIBLE );
	}

	function emptyText()
	{
		var busy = data && data.refreshing;
		switch ( TABS[ tab ] )
		{
			case "internet":
				if ( busy ) return "Asking the master servers...";
				if ( data && data.masters && !data.masters_answered ) return "No master server answered";
				break;
			case "lan":
				if ( busy ) return "Looking on your network...";
				return "No servers on your network";
			case "favorites":
				if ( !busy && !countIn( "favorite" ) ) return "No favorites yet - " + ( gamepad ? "Y" : "F" ) + " on a server adds it";
				break;
			case "history":
				if ( !busy && !countIn( "history" ) ) return "Servers you join show up here";
				break;
		}
		if ( busy ) return "Refreshing...";
		return "No servers match the filters";
	}

	function countIn( flag )
	{
		return ( data && data.servers || [] ).filter( function ( s ) { return s[ flag ]; } ).length;
	}

	function renderStatus()
	{
		var st = $( "#Status" );
		var total = ( data && data.servers || [] ).filter( inTab ).length;
		var bits = [ TAB_NAMES[ tab ] ];
		if ( TABS[ tab ] === "internet" && data )
			bits.push( data.masters_answered + " of " + data.masters + " master server" + ( data.masters === 1 ? "" : "s" ) );
		bits.push( shown.length + ( shown.length !== total ? " of " + total : "" ) + " server" + ( total === 1 ? "" : "s" ) );
		if ( data && data.refreshing )
			bits.push( "refreshing" );
		st.text = bits.join( "  -  " ).toUpperCase();
		st.SetHasClass( "busy", !!( data && data.refreshing ) );
		$( "#Empty" ).text = shown.length ? "" : emptyText();
	}

	// --- filters ----------------------------------------------------------

	function modesSeen()
	{
		var seen = {};
		( data && data.servers || [] ).forEach( function ( s ) { if ( s.gametype ) seen[ s.gametype ] = 1; } );
		if ( mode ) seen[ mode ] = 1;
		return Object.keys( seen ).sort();
	}

	function renderFilters()
	{
		$( "#FilterText0" ).text = "Mode: " + ( mode ? modeName( mode ) : "All" );
		$( "#FilterText1" ).text = "Empty: " + ( hideEmpty ? "Hide" : "Show" );
		$( "#FilterText2" ).text = "Full: " + ( hideFull ? "Hide" : "Show" );
		$( "#FilterText3" ).text = "Locked: " + ( hideLocked ? "Hide" : "Show" );
		$( "#FilterText4" ).text = "Sort: " + ( TABS[ tab ] === "history" && sort === 0 ? "Recent" : SORTS[ sort ] );
		var on = [ !!mode, hideEmpty, hideFull, hideLocked, sort !== 0 ];
		for ( var i = 0; i < FILTERS; i++ )
		{
			$( "#Filter" + i ).SetHasClass( "is-on", on[ i ] );
			$( "#Filter" + i ).SetHasClass( "is-selected", zone === "filters" && i === filterSel );
		}
	}

	function changeFilter( i, dir )
	{
		dir = dir || 1;
		switch ( i )
		{
			case 0:
				var list = [ "" ].concat( modesSeen() );
				var at = list.indexOf( mode );
				mode = list[ ( at + dir + list.length ) % list.length ];
				break;
			case 1: hideEmpty = !hideEmpty; break;
			case 2: hideFull = !hideFull; break;
			case 3: hideLocked = !hideLocked; break;
			case 4: sort = ( sort + dir + SORTS.length ) % SORTS.length; break;
		}
		sound( SND_NAV );
		lastRowsKey = "";
		rebuild();
		render();
	}

	// --- the selected server ---------------------------------------------

	function selected() { return shown[ sel ] || null; }

	function renderDetail()
	{
		var s = selected();
		show( "#Detail", !!s );
		if ( !s )
			return;
		if ( detailsAddr !== s.addr || Date.now() - detailsAt > 2000 )
		{
			detailsAddr = s.addr;
			detailsAt = Date.now();
			details = json( q( "sb_details:" + s.addr ), null );
		}
		var d = details && details.ok && details.addr === s.addr ? details : null;
		$( "#DName" ).text = s.name;
		$( "#DAddr" ).text = s.addr + ( s.engine ? "  -  " + s.engine.toLowerCase() : "" );
		$( "#DMode" ).text = modeName( s.gametype );
		$( "#DMap" ).text = s.map || "-";
		$( "#DPlayers" ).text = s.players + ( s.bots ? "+" + s.bots : "" ) + " / " + s.max;
		$( "#DPing" ).text = s.ping + " ms";
		var rules = d && d.rules ? d.rules : {};
		var state = STATE_NAMES[ s.state ] || ( s.state ? s.state.toLowerCase() : "" );
		if ( rules.arena_p1 && rules.arena_p2 )
			state = rules.arena_p1 + "  vs  " + rules.arena_p2 + ( state ? "  -  " + state : "" );
		else if ( rules.arena_round )
			state = rules.arena_round + ( state ? "  -  " + state : "" );
		$( "#DState" ).text = state;
		var pl = $( "#DPlayerList" );
		pl.RemoveAndDeleteChildren();
		var players = d && d.player_list ? d.player_list : [];
		players.sort( function ( a, b ) { return b.frags - a.frags; } );
		if ( !players.length )
			$.CreatePanel( "Label", pl, "", { class: "pl-none", text: d ? "Nobody's here yet" : "Asking...", hittest: "false" } );
		players.slice( 0, 9 ).forEach( function ( p )
		{
			var row = $.CreatePanel( "Panel", pl, "", { class: "pl", hittest: "false" } );
			$.CreatePanel( "Label", row, "", { class: "pl-name", text: p.name || "?", hittest: "false" } );
			$.CreatePanel( "Label", row, "", { class: "pl-score", text: String( p.frags ), hittest: "false" } );
			$.CreatePanel( "Label", row, "", { class: "pl-ping", text: p.bot ? "BOT" : p.ping + " ms", hittest: "false" } );
		} );
		if ( players.length > 9 )
			$.CreatePanel( "Label", pl, "", { class: "pl-none", text: "and " + ( players.length - 9 ) + " more", hittest: "false" } );
		var flags = [];
		if ( s.password ) flags.push( "Password" );
		if ( s.favorite ) flags.push( "Favorite" );
		if ( s.lan ) flags.push( "LAN" );
		if ( !s.compatible ) flags.push( "Different version (" + s.protocol + ")" );
		if ( data && data.server === s.addr ) flags.push( "You're here" );
		$( "#DFlags" ).text = flags.join( "  -  " );
	}

	// --- hints ------------------------------------------------------------

	var hintKey = "";
	function renderHints()
	{
		var list;
		if ( dialog )
			list = [ [ "A", "Enter", "OK", dialogOk ], [ "B", "Esc", "Cancel", closeDialog ] ];
		else
			list = [
				[ "A", "Enter", "Join", join ],
				[ "B", "Esc", "Back", back ],
				[ "X", "R", "Refresh", refresh ],
				[ "Y", "F", selected() && selected().favorite ? "Unfavorite" : "Favorite", toggleFavorite ],
				[ "LB RB", "Q E", "Tab", null ],
				[ "LT RT", "PgUp PgDn", "Page", null ],
				[ "View", "O", "Sort", function () { changeFilter( 4, 1 ); } ],
				[ "Up", "Up", "Filters", null ],
				[ "Menu", "C", "Address", openAddress ]
			];
		var key = gamepad + JSON.stringify( list.map( function ( h ) { return h[ 2 ]; } ) );
		if ( key === hintKey )
			return;
		hintKey = key;
		var parent = $( "#Hints" );
		parent.RemoveAndDeleteChildren();
		list.forEach( function ( h )
		{
			var b = $.CreatePanel( "Button", parent, "", { class: "hint" } );
			var k = gamepad ? h[ 0 ] : h[ 1 ];
			var cls = "hint-key" + ( gamepad && /^[ABXY]$/.test( k ) ? " gp-" + k.toLowerCase() : "" );
			$.CreatePanel( "Label", b, "", { class: cls, text: k, hittest: "false" } );
			$.CreatePanel( "Label", b, "", { class: "hint-text", text: h[ 2 ], hittest: "false" } );
			if ( h[ 3 ] )
				b.SetPanelEvent( "onactivate", function () { $.GetContextPanel().SetFocus(); h[ 3 ](); } );
		} );
		$( "#TabPrev" ).text = gamepad ? "LB" : "Q";
		$( "#TabNext" ).text = gamepad ? "RB" : "E";
	}

	// --- actions ----------------------------------------------------------

	function setTab( t, snd )
	{
		t = ( t + TABS.length ) % TABS.length;
		if ( snd !== false ) sound( SND_NAV );
		tab = t;
		for ( var i = 0; i < TABS.length; i++ )
			$( "#Tab" + i ).SetHasClass( "is-selected", i === tab );
		sel = 0;
		top = 0;
		selAddr = "";
		refresh( true );
	}

	function refresh( quiet )
	{
		if ( quiet !== true ) sound( SND_OK );
		q( "sb_refresh:" + TABS[ tab ] );
		history = String( q( "cvar:sb_history" ) || "" ).split( " " ).filter( function ( x ) { return x; } );
		poll( true );
	}

	function move( d )
	{
		if ( !shown.length ) return;
		var n = Math.max( 0, Math.min( shown.length - 1, sel + d ) );
		if ( n !== sel ) sound( SND_NAV );
		sel = n;
		selAddr = shown[ sel ].addr;
		clampScroll();
		render();
	}

	function clickRow( slot )
	{
		$.GetContextPanel().SetFocus();
		if ( dialog || joining ) return;
		var i = top + slot;
		if ( !shown[ i ] ) return;
		zone = "list";
		// a click on the selected row joins it
		if ( i === sel && selAddr === shown[ i ].addr )
		{
			join();
			return;
		}
		sel = i;
		selAddr = shown[ i ].addr;
		sound( SND_NAV );
		render();
	}

	function join()
	{
		var s = selected();
		if ( !s || joining ) return;
		if ( !s.compatible )
		{
			sound( SND_BACK );
			notice( "That server runs a different version (" + s.protocol + ", you have " + ( data ? data.protocol : "?" ) + ")" );
			return;
		}
		if ( s.max && s.players + s.bots >= s.max )
		{
			sound( SND_BACK );
			notice( s.name + " is full" );
			return;
		}
		if ( s.password )
		{
			openDialog( "password", s );
			return;
		}
		connect( s.addr, s.name, "" );
	}

	function connect( addr, name, pass )
	{
		var r = json( q( "sb_connect:" + addr + ( pass ? " " + pass : "" ) ), {} );
		if ( !r.ok )
		{
			sound( SND_BACK );
			notice( "Can't join " + addr + ( r.error ? ": " + r.error : "" ) );
			return false;
		}
		sound( SND_OK );
		joining = { addr: addr, at: Date.now() };
		$( "#JoiningName" ).text = name || addr;
		$( "#JoiningSub" ).text = "Connecting to " + addr + "...";
		show( "#Joining", true );
		return true;
	}

	function toggleFavorite()
	{
		var s = selected();
		if ( !s ) return;
		var r = json( q( "sb_favorite:" + s.addr ), {} );
		if ( r.ok )
		{
			sound( SND_NAV );
			notice( r.favorite ? "Added " + s.name + " to favorites" : "Removed " + s.name + " from favorites", 3 );
			poll( true );
		}
	}

	function back()
	{
		sound( SND_BACK );
		if ( data && data.connected )
			cbuf( "menu_webcore" );            // the pause page
		else if ( params.from === "title" )
			cbuf( "menu_webcore_title" );
		else
			cbuf( "menu_webcore online" );
	}

	// --- the dialog -------------------------------------------------------

	function openDialog( kind, s )
	{
		dialog = { kind: kind, addr: s ? s.addr : "", name: s ? s.name : "", btn: 0 };
		$( "#DialogHead" ).text = kind === "password" ? "PASSWORD" : "CONNECT TO ADDRESS";
		$( "#DialogText" ).text = kind === "password" ? dialog.name + " asks for a password" :
			"An IP or host name, with :port if it isn't 27500. Join it, or add it to favorites.";
		$( "#DialogOkText" ).text = kind === "password" ? "JOIN" : "JOIN";
		$( "#DialogEntry" ).text = "";
		$( "#DialogEntry" ).SetHasClass( "masked", kind === "password" );
		$( "#DialogMask" ).text = "";
		hoverDialog( 0 );
		show( "#Dialog", true );
		sound( SND_OK );
		$( "#DialogEntry" ).SetFocus();
		renderHints();
	}

	function closeDialog()
	{
		if ( !dialog ) return;
		dialog = null;
		show( "#Dialog", false );
		sound( SND_BACK );
		$.GetContextPanel().SetFocus();
		renderHints();
	}

	function hoverDialog( i )
	{
		if ( !dialog ) return;
		dialog.btn = i;
		$( "#DialogOk" ).SetHasClass( "is-selected", i === 0 );
		$( "#DialogCancel" ).SetHasClass( "is-selected", i === 1 );
	}

	function dialogOk()
	{
		if ( !dialog ) return;
		if ( dialog.btn === 1 ) { closeDialog(); return; }
		var text = String( $( "#DialogEntry" ).text || "" ).replace( /^\s+|\s+$/g, "" );
		var d = dialog;
		if ( d.kind === "password" )
		{
			if ( !text ) { notice( "Type the password first", 3 ); return; }
			dialog = null;
			show( "#Dialog", false );
			$.GetContextPanel().SetFocus();
			connect( d.addr, d.name, text );
			return;
		}
		text = text.replace( /\s+/g, "" );
		if ( !text ) { notice( "Type an address first", 3 ); return; }
		dialog = null;
		show( "#Dialog", false );
		$.GetContextPanel().SetFocus();
		connect( text, text, "" );
	}

	function openAddress() { if ( !dialog && !joining ) openDialog( "address", null ); }

	// --- the loop ---------------------------------------------------------

	function render()
	{
		renderRows();
		renderFilters();
		renderStatus();
		renderDetail();
		renderHints();
		if ( noticeUntil && Date.now() > noticeUntil )
		{
			noticeUntil = 0;
			show( "#Notice", false );
		}
	}

	var lastPoll = 0;
	function poll( now )
	{
		data = json( q( "sb_list" ), data );
		rebuild();
		render();
		lastPoll = Date.now();
	}

	function tick()
	{
		if ( dialog && dialog.kind === "password" )
			$( "#DialogMask" ).text = new Array( String( $( "#DialogEntry" ).text || "" ).length + 1 ).join( "*" );
		if ( Date.now() - lastPoll > 300 )
			poll();
		else if ( noticeUntil && Date.now() > noticeUntil )
			render();
		// still here long after asking to join: it didn't take
		if ( joining && Date.now() - joining.at > 20000 )
		{
			joining = null;
			show( "#Joining", false );
			notice( "No answer from the server" );
		}
		$.Schedule( 0.1, tick );
	}

	function onKey( e )
	{
		if ( !e || !e.down )
			return;
		var k = e.key || "", b = String( e.bind || "" ).toUpperCase();
		var pad = /^GP_|^JOY|^AUX/.test( b );
		if ( pad !== gamepad && b && b !== "MOUSE1" )
		{
			gamepad = pad;
			renderHints();
			renderStatus();
		}
		var typing = !!dialog;
		var up = k === "ArrowUp" || b === "GP_DPAD_UP" || b === "MWHEELUP" || k === "WheelUp" || ( !typing && ( k === "w" || k === "W" ) );
		var down = k === "ArrowDown" || b === "GP_DPAD_DOWN" || b === "MWHEELDOWN" || k === "WheelDown" || ( !typing && ( k === "s" || k === "S" ) );
		var left = k === "ArrowLeft" || b === "GP_DPAD_LEFT";
		var right = k === "ArrowRight" || b === "GP_DPAD_RIGHT";
		var ok = k === "Enter" || b === "GP_A";
		var cancel = k === "Escape" || b === "GP_B";
		if ( joining )
		{
			if ( cancel )
			{
				joining = null;
				show( "#Joining", false );
				cbuf( "disconnect" );
				sound( SND_BACK );
			}
			return;
		}
		// Tab moves Panorama's focus to a button, which Enter would then
		// press as well: keys only go through this handler
		if ( !dialog )
			$.GetContextPanel().SetFocus();
		if ( dialog )
		{
			if ( cancel ) closeDialog();
			else if ( ok ) dialogOk();
			else if ( b === "GP_DPAD_LEFT" || b === "GP_DPAD_RIGHT" ) { hoverDialog( dialog.btn ? 0 : 1 ); sound( SND_NAV ); }
			return;
		}
		var lk = k.length === 1 ? k.toLowerCase() : "";
		if ( b === "GP_LSHOULDER" || lk === "q" ) { setTab( tab - 1 ); return; }
		if ( b === "GP_RSHOULDER" || lk === "e" ) { setTab( tab + 1 ); return; }
		if ( b === "GP_X" || lk === "r" || b === "F5" ) { refresh(); return; }
		if ( b === "GP_Y" || lk === "f" ) { toggleFavorite(); return; }
		if ( b === "GP_MENU" || b === "GP_START" || lk === "c" ) { openAddress(); return; }
		if ( b === "GP_VIEW" || b === "GP_BACK" || lk === "o" ) { changeFilter( 4, 1 ); return; }
		if ( cancel )
		{
			if ( zone === "filters" ) { zone = "list"; sound( SND_NAV ); render(); return; }
			back();
			return;
		}
		if ( zone === "filters" )
		{
			if ( left ) { filterSel = ( filterSel + FILTERS - 1 ) % FILTERS; sound( SND_NAV ); render(); }
			else if ( right ) { filterSel = ( filterSel + 1 ) % FILTERS; sound( SND_NAV ); render(); }
			else if ( down ) { zone = "list"; sound( SND_NAV ); lastRowsKey = ""; render(); }
			else if ( ok || k === " " ) changeFilter( filterSel, 1 );
			return;
		}
		if ( up )
		{
			if ( sel === 0 || !shown.length ) { zone = "filters"; sound( SND_NAV ); lastRowsKey = ""; render(); }
			else move( -1 );
		}
		else if ( down ) move( 1 );
		else if ( b === "GP_LTRIGGER" || b === "PGUP" ) move( -VISIBLE );
		else if ( b === "GP_RTRIGGER" || b === "PGDN" ) move( VISIBLE );
		else if ( b === "HOME" ) move( -shown.length );
		else if ( b === "END" ) move( shown.length );
		else if ( left ) setTab( tab - 1 );
		else if ( right ) setTab( tab + 1 );
		else if ( ok || k === " " ) join();
	}

	Bulwark.On( "key", onKey );

	makeRows();
	var startTab = TABS.indexOf( params.tab || "" );
	if ( startTab < 0 ) startTab = 0;
	tab = startTab;
	for ( var i = 0; i < TABS.length; i++ )
		$( "#Tab" + i ).SetHasClass( "is-selected", i === tab );
	refresh( true );
	if ( params.failed )
		notice( "Couldn't join " + params.failed + ": " + friendlyReason( params.reason ), 8 );
	tick();

	return {
		Tab: function ( i ) { $.GetContextPanel().SetFocus(); if ( !dialog && !joining ) setTab( i ); },
		Filter: function ( i ) { $.GetContextPanel().SetFocus(); if ( dialog || joining ) return; zone = "filters"; filterSel = i; changeFilter( i, 1 ); },
		HoverFilter: function ( i ) { if ( zone === "filters" ) { filterSel = i; renderFilters(); } },
		DialogOk: function () { dialogOk(); },
		CloseDialog: function () { closeDialog(); },
		HoverDialog: function ( i ) { hoverDialog( i ); }
	};
} )();
