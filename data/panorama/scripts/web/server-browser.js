// The server browser (layout/web/server-browser.xml). Data from the engine's
// "sb_..." requests (docs/spec/serverbrowser.md); the page searches, filters,
// sorts and scrolls. Keys, mouse and controller: every action has all three.
//
// Controller zones, top to bottom: the toolbar (filters, search, refresh),
// the sort row, the list, the bottom band (close, address, connect). The
// filter menu opens over the list from the filter button.
var Servers = ( function ()
{
	var SND_NAV = "misc/menu1", SND_OK = "misc/menu2", SND_BACK = "misc/menu3";
	var SOURCES = [ "internet", "lan", "favorites", "history" ];
	var SOURCE_NAMES = [ "Internet", "LAN", "Favorites", "History" ];
	var SORTS = [ "name", "map", "players", "latency", "played" ];
	var SORT_LATENCY = 3, SORT_PLAYED = 4;
	var VISIBLE = 8;
	var FROWS = 5;
	var ZONES = [ "toolbar", "sort", "list", "bottom" ];
	var ZONE_SIZE = { toolbar: 3, sort: 5, bottom: 3, popup: FROWS };
	var MODE_NAMES = { duel: "Duel", deathmatch: "Deathmatch", teamdm: "Team DM", domination: "Domination",
		lastmanstanding: "Last Man", invasion: "Invasion", singleplayer: "Co-op", dm_lobby: "Lobby" };
	var STATE_NAMES = { WARMUP: "Waiting for fighters", COUNTDOWN: "Starting", LIVE: "Fight in progress",
		ROUND_END: "Between rounds", MATCH_END: "Set over", INTERMISSION: "Between maps" };

	var source = 0;
	var zone = "list";          // a ZONES entry, or "popup" while the filter menu is open
	var zoneSel = { toolbar: 1, sort: SORT_LATENCY, bottom: 2, popup: 0 };
	var mode = "";              // "" = all
	var hideEmpty = false, hideFull = false, hideLocked = false;
	var sort = SORT_LATENCY;
	var search = "", typing = false;
	var data = null;
	var shown = [];             // the searched, filtered, sorted rows
	var selAddr = "";
	var sel = 0, top = 0;
	var rowPanels = [];
	var lastRowsKey = "";
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
	function popupOpen() { return zone === "popup"; }

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
		if ( !m ) return "";
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

	function inSource( s )
	{
		switch ( SOURCES[ source ] )
		{
			case "internet": return s.internet;
			case "lan": return s.lan;
			case "favorites": return s.favorite;
			case "history": return s.history;
		}
		return true;
	}

	function isFull( s ) { return s.max && s.players + s.bots >= s.max; }

	function passes( s )
	{
		if ( !inSource( s ) ) return false;
		if ( mode && s.gametype !== mode ) return false;
		if ( hideEmpty && s.players + s.bots === 0 ) return false;
		if ( hideFull && isFull( s ) ) return false;
		if ( hideLocked && s.password ) return false;
		if ( search )
		{
			var hay = ( s.name + " " + s.map + " " + modeName( s.gametype ) + " " + s.addr ).toLowerCase();
			var words = search.toLowerCase().split( /\s+/ );
			for ( var i = 0; i < words.length; i++ )
				if ( words[ i ] && hay.indexOf( words[ i ] ) < 0 ) return false;
		}
		return true;
	}

	function played( s ) { var i = history.indexOf( s.addr ); return i < 0 ? 1e6 : i; }
	function cmpText( a, b ) { a = a.toLowerCase(); b = b.toLowerCase(); return a < b ? -1 : a > b ? 1 : 0; }

	function compare( a, b )
	{
		// servers that can't be joined go last
		if ( a.compatible !== b.compatible ) return a.compatible ? -1 : 1;
		switch ( SORTS[ sort ] )
		{
			case "name": return cmpText( a.name, b.name ) || a.ping - b.ping;
			case "map": return cmpText( a.map, b.map ) || a.ping - b.ping;
			case "players": return ( b.players + b.bots - a.players - a.bots ) || ( a.ping - b.ping );
			case "played": return ( played( a ) - played( b ) ) || ( a.ping - b.ping );
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

	function el( type, parent, cls, extra )
	{
		var o = { class: cls, hittest: "false" };
		for ( var k in extra || {} ) o[ k ] = extra[ k ];
		return $.CreatePanel( type, parent, "", o );
	}

	function makeRows()
	{
		var parent = $( "#Rows" );
		for ( var i = 0; i < VISIBLE; i++ )
		{
			var r = $.CreatePanel( "Button", parent, "Row" + i, { class: "row" } );
			var c = {};
			c.shot = el( "Image", r, "r-shot" );
			c.fade = el( "Panel", r, "r-shot-fade" );
			c.name = el( "Label", r, "r-name", { text: "" } );
			c.tags = el( "Panel", r, "r-tags" );
			c.bots = el( "Panel", r, "r-bots" );
			var bot = el( "Panel", c.bots, "ico-bot" );
			el( "Panel", bot, "ico-bot-ant" );
			el( "Panel", bot, "ico-bot-head" );
			el( "Panel", bot, "ico-bot-eye e0" );
			el( "Panel", bot, "ico-bot-eye e1" );
			c.botsNum = el( "Label", c.bots, "r-bots-num", { text: "" } );
			c.map = el( "Label", r, "r-map", { text: "" } );
			c.net = el( "Panel", r, "r-net" );
			var bars = el( "Panel", c.net, "ico-bars" );
			c.bars = [ el( "Panel", bars, "bar b0" ), el( "Panel", bars, "bar b1" ), el( "Panel", bars, "bar b2" ) ];
			c.ping = el( "Label", c.net, "r-ping", { text: "" } );
			c.players = el( "Label", c.net, "r-players", { text: "" } );
			( function ( slot )
			{
				r.SetPanelEvent( "onactivate", function () { clickRow( slot ); } );
			} )( i );
			rowPanels.push( { panel: r, cells: c, shot: null, tagsKey: "" } );
		}
	}

	function tagsFor( s )
	{
		var t = [];
		if ( s.gametype ) t.push( [ "t-mode", modeName( s.gametype ) ] );
		if ( STATE_NAMES[ s.state ] ) t.push( [ "", STATE_NAMES[ s.state ] ] );
		if ( s.password ) t.push( [ "t-lock", "Password" ] );
		if ( s.favorite ) t.push( [ "t-fav", "Favorite" ] );
		if ( s.lan ) t.push( [ "t-lan", "LAN" ] );
		if ( data && data.server === s.addr ) t.push( [ "t-here", "You're here" ] );
		if ( !s.compatible ) t.push( [ "t-old", "Different version (" + s.protocol + ")" ] );
		return t;
	}

	function renderRows()
	{
		var key = top + "|" + sel + "|" + zone + "|" + ( data && data.server ) + "|" + JSON.stringify( shown.slice( top, top + VISIBLE ) );
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
			c.name.text = s.name;
			var tags = tagsFor( s ), tk = JSON.stringify( tags );
			if ( tk !== rp.tagsKey )
			{
				rp.tagsKey = tk;
				c.tags.RemoveAndDeleteChildren();
				tags.forEach( function ( t ) { el( "Label", c.tags, "tag " + t[ 0 ], { text: t[ 1 ] } ); } );
			}
			c.bots.SetHasClass( "hidden", !s.bots );
			c.botsNum.text = String( s.bots );
			c.map.text = s.map || "";
			var shot = s.shot ? "file://{game}/" + s.shot : "";
			if ( shot !== rp.shot )
			{
				rp.shot = shot;
				if ( shot ) c.shot.SetImage( shot );
				c.shot.SetHasClass( "hidden", !shot );
			}
			var pc = pingClass( s.ping ), lit = pc === "good" ? 3 : pc === "ok" ? 2 : 1;
			[ "good", "ok", "bad" ].forEach( function ( k ) { c.net.SetHasClass( k, k === pc ); } );
			for ( var b = 0; b < 3; b++ ) c.bars[ b ].SetHasClass( "lit", b < lit );
			c.ping.text = String( s.ping );
			c.players.text = ( s.players + s.bots ) + "/" + s.max;
			rp.panel.SetHasClass( "full", !!isFull( s ) );
			rp.panel.SetHasClass( "bad-version", !s.compatible );
			rp.panel.SetHasClass( "is-selected", ( zone === "list" || zone === "bottom" || zone === "popup" ) && top + i === sel );
		}
		// scroll bar
		var track = 680;
		var thumb = shown.length > VISIBLE ? Math.max( 40, track * VISIBLE / shown.length ) : track;
		var at = shown.length > VISIBLE ? ( track - thumb ) * top / ( shown.length - VISIBLE ) : 0;
		$( "#ScrollThumb" ).style.height = thumb.toFixed( 0 ) + "px";
		$( "#ScrollThumb" ).style.transform = "translateY( " + at.toFixed( 0 ) + "px )";
		show( "#ScrollTrack", shown.length > VISIBLE );
	}

	function emptyText()
	{
		var busy = data && data.refreshing;
		if ( search && countIn( null ) ) return "Nothing matches \"" + search + "\"";
		switch ( SOURCES[ source ] )
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
		return ( data && data.servers || [] ).filter( function ( s ) { return flag ? s[ flag ] : inSource( s ); } ).length;
	}

	function renderStatus()
	{
		var st = $( "#Status" );
		var total = countIn( null );
		var bits = [ SOURCE_NAMES[ source ] ];
		if ( SOURCES[ source ] === "internet" && data )
			bits.push( data.masters_answered + " of " + data.masters + " master server" + ( data.masters === 1 ? "" : "s" ) );
		bits.push( shown.length + ( shown.length !== total ? " of " + total : "" ) + " server" + ( total === 1 ? "" : "s" ) );
		if ( data && data.refreshing )
			bits.push( "refreshing" );
		st.text = bits.join( "  -  " ).toUpperCase();
		st.SetHasClass( "busy", !!( data && data.refreshing ) );
		$( "#Empty" ).text = shown.length ? "" : emptyText();
	}

	// --- toolbar, sort row, filter menu, bottom band -----------------------

	function modesSeen()
	{
		var seen = {};
		( data && data.servers || [] ).forEach( function ( s ) { if ( s.gametype ) seen[ s.gametype ] = 1; } );
		if ( mode ) seen[ mode ] = 1;
		return Object.keys( seen ).sort();
	}

	function filtersOn() { return !!mode || hideEmpty || hideFull || hideLocked; }

	function renderBars()
	{
		var i;
		for ( i = 0; i < 3; i++ )
			$( "#Tool" + i ).SetHasClass( "is-selected", zone === "toolbar" && zoneSel.toolbar === i );
		$( "#Tool0" ).SetHasClass( "is-on", filtersOn() || popupOpen() );
		$( "#Tool1" ).SetHasClass( "typing", typing );
		show( "#SearchHint", !typing && !String( $( "#Search" ).text || "" ) );
		for ( i = 0; i < SORTS.length; i++ )
		{
			$( "#Sort" + i ).SetHasClass( "is-on", sort === i );
			$( "#Sort" + i ).SetHasClass( "is-selected", zone === "sort" && zoneSel.sort === i );
		}
		var vals = [ SOURCE_NAMES[ source ], mode ? modeName( mode ) : "All", hideEmpty ? "Hide" : "Show",
			hideFull ? "Hide" : "Show", hideLocked ? "Hide" : "Show" ];
		var on = [ source !== 0, !!mode, hideEmpty, hideFull, hideLocked ];
		show( "#FilterMenu", popupOpen() );
		for ( i = 0; i < FROWS; i++ )
		{
			$( "#FVal" + i ).text = vals[ i ];
			$( "#FRow" + i ).SetHasClass( "is-on", on[ i ] );
			$( "#FRow" + i ).SetHasClass( "is-selected", popupOpen() && zoneSel.popup === i );
		}
		for ( i = 0; i < 3; i++ )
			$( "#Bottom" + i ).SetHasClass( "is-selected", zone === "bottom" && zoneSel.bottom === i );
		var s = selected();
		$( "#Bottom2" ).SetHasClass( "disabled", !s || !s.compatible || !!isFull( s ) );
	}

	function changeFilter( i, dir )
	{
		dir = dir || 1;
		switch ( i )
		{
			case 0: setSource( source + dir ); return;
			case 1:
				var list = [ "" ].concat( modesSeen() );
				var at = list.indexOf( mode );
				mode = list[ ( at + dir + list.length ) % list.length ];
				break;
			case 2: hideEmpty = !hideEmpty; break;
			case 3: hideFull = !hideFull; break;
			case 4: hideLocked = !hideLocked; break;
		}
		sound( SND_NAV );
		lastRowsKey = "";
		rebuild();
		render();
	}

	function setSort( i )
	{
		sort = ( i + SORTS.length ) % SORTS.length;
		sound( SND_NAV );
		lastRowsKey = "";
		rebuild();
		render();
	}

	function openPopup()
	{
		if ( popupOpen() ) { closePopup(); return; }
		stopTyping();
		zone = "popup";
		sound( SND_OK );
		lastRowsKey = "";
		render();
	}

	function closePopup()
	{
		if ( !popupOpen() ) return;
		zone = "toolbar";
		zoneSel.toolbar = 0;
		sound( SND_BACK );
		lastRowsKey = "";
		render();
	}

	function startTyping()
	{
		if ( popupOpen() ) closePopup();
		typing = true;
		zone = "toolbar";
		zoneSel.toolbar = 1;
		$( "#Search" ).SetFocus();
		sound( SND_NAV );
		render();
	}

	function stopTyping()
	{
		if ( !typing ) return;
		typing = false;
		$.GetContextPanel().SetFocus();
		render();
	}

	function readSearch()
	{
		var t = String( $( "#Search" ).text || "" ).replace( /^\s+|\s+$/g, "" );
		if ( t === search ) return false;
		search = t;
		sel = 0;
		top = 0;
		selAddr = "";
		lastRowsKey = "";
		rebuild();
		return true;
	}

	// --- the selected server ---------------------------------------------

	function selected() { return shown[ sel ] || null; }

	// --- hints: the smaller actions, between Address and Connect ------------

	var hintKey = "";
	function renderHints()
	{
		$( "#BKey0" ).text = gamepad ? "B" : "ESC";
		$( "#BKey0" ).SetHasClass( "gp-b", gamepad );
		$( "#BKey1" ).text = gamepad ? "MENU" : "C";
		$( "#BKey2" ).text = gamepad ? "A" : "ENTER";
		$( "#BKey2" ).SetHasClass( "gp-a", gamepad );
		var s = selected();
		var list = [
			[ "X", "R", "Refresh", function () { refresh(); } ],
			[ "Y", "F", s && s.favorite ? "Unfavorite" : "Favorite", toggleFavorite ],
			[ "VIEW", "O", "Filters", openPopup ],
			[ "LB RB", "Q E", SOURCE_NAMES[ source ], function () { setSource( source + 1 ); } ]
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
			var cls = "hint-key" + ( gamepad && /^[XY]$/.test( k ) ? " gp-" + k.toLowerCase() : "" );
			$.CreatePanel( "Label", b, "", { class: cls, text: k, hittest: "false" } );
			$.CreatePanel( "Label", b, "", { class: "hint-text", text: h[ 2 ], hittest: "false" } );
			b.SetPanelEvent( "onactivate", function () { $.GetContextPanel().SetFocus(); h[ 3 ](); } );
		} );
	}

	// --- actions ----------------------------------------------------------

	function setSource( t, snd )
	{
		source = ( t + SOURCES.length ) % SOURCES.length;
		if ( snd !== false ) sound( SND_NAV );
		// History lists newest first unless another sort was picked
		if ( SOURCES[ source ] === "history" && sort === SORT_LATENCY ) sort = SORT_PLAYED;
		else if ( SOURCES[ source ] !== "history" && sort === SORT_PLAYED ) sort = SORT_LATENCY;
		sel = 0;
		top = 0;
		selAddr = "";
		lastRowsKey = "";
		refresh( true );
	}

	function refresh( quiet )
	{
		if ( quiet !== true ) sound( SND_OK );
		q( "sb_refresh:" + SOURCES[ source ] );
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
		stopTyping();
		$.GetContextPanel().SetFocus();
		if ( dialog || joining ) return;
		if ( popupOpen() ) { closePopup(); return; }
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
		if ( isFull( s ) )
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
		else if ( params.from === "online" )
			cbuf( "menu_webcore online" );
		else
			cbuf( "menu_webcore_title" );
	}

	function activate( z, i )
	{
		switch ( z )
		{
			case "toolbar":
				if ( i === 0 ) openPopup();
				else if ( i === 1 ) startTyping();
				else refresh();
				break;
			case "sort": setSort( i ); break;
			case "popup": changeFilter( i, 1 ); break;
			case "bottom":
				if ( i === 0 ) back();
				else if ( i === 1 ) openAddress();
				else join();
				break;
		}
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
		render();
	}

	function closeDialog()
	{
		if ( !dialog ) return;
		dialog = null;
		show( "#Dialog", false );
		sound( SND_BACK );
		$.GetContextPanel().SetFocus();
		render();
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
		renderBars();
		renderStatus();
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
		if ( readSearch() )
			render();
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

	function setZone( z )
	{
		zone = z;
		sound( SND_NAV );
		lastRowsKey = "";
		render();
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
		var text = !!dialog || typing;
		var up = k === "ArrowUp" || b === "GP_DPAD_UP" || b === "MWHEELUP" || k === "WheelUp" || ( !text && ( k === "w" || k === "W" ) );
		var down = k === "ArrowDown" || b === "GP_DPAD_DOWN" || b === "MWHEELDOWN" || k === "WheelDown" || ( !text && ( k === "s" || k === "S" ) );
		var left = k === "ArrowLeft" || b === "GP_DPAD_LEFT";
		var right = k === "ArrowRight" || b === "GP_DPAD_RIGHT";
		var ok = k === "Enter" || b === "GP_A";
		var cancel = k === "Escape" || b === "GP_B";
		var wheel = b === "MWHEELUP" || b === "MWHEELDOWN" || k === "WheelUp" || k === "WheelDown";
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
		if ( dialog )
		{
			if ( cancel ) closeDialog();
			else if ( ok ) dialogOk();
			else if ( b === "GP_DPAD_LEFT" || b === "GP_DPAD_RIGHT" ) { hoverDialog( dialog.btn ? 0 : 1 ); sound( SND_NAV ); }
			return;
		}
		// typing in the search field: the entry has the keys; these leave it
		if ( typing )
		{
			if ( ok || cancel || down || b === "GP_DPAD_UP" )
			{
				stopTyping();
				zone = "list";
				lastRowsKey = "";
				render();
			}
			return;
		}
		// Tab moves Panorama's focus to a button, which Enter would then
		// press as well: keys only go through this handler
		$.GetContextPanel().SetFocus();
		// the wheel always scrolls the list
		if ( wheel ) { if ( zone !== "popup" ) zone = "list"; move( up ? -1 : 1 ); return; }
		var lk = k.length === 1 ? k.toLowerCase() : "";
		if ( b === "GP_LSHOULDER" || lk === "q" ) { setSource( source - 1 ); return; }
		if ( b === "GP_RSHOULDER" || lk === "e" ) { setSource( source + 1 ); return; }
		if ( b === "GP_X" || lk === "r" || b === "F5" ) { refresh(); return; }
		if ( b === "GP_Y" || lk === "f" ) { toggleFavorite(); return; }
		if ( b === "GP_MENU" || b === "GP_START" || lk === "c" ) { openAddress(); return; }
		if ( b === "GP_VIEW" || b === "GP_BACK" || lk === "o" ) { openPopup(); return; }
		if ( lk === "/" ) { startTyping(); return; }
		if ( zone === "popup" )
		{
			if ( cancel ) closePopup();
			else if ( up ) { zoneSel.popup = ( zoneSel.popup + FROWS - 1 ) % FROWS; sound( SND_NAV ); render(); }
			else if ( down ) { zoneSel.popup = ( zoneSel.popup + 1 ) % FROWS; sound( SND_NAV ); render(); }
			else if ( left ) changeFilter( zoneSel.popup, -1 );
			else if ( right || ok || k === " " ) changeFilter( zoneSel.popup, 1 );
			return;
		}
		if ( cancel )
		{
			if ( zone !== "list" ) { setZone( "list" ); return; }
			back();
			return;
		}
		if ( zone === "list" )
		{
			if ( up )
			{
				if ( sel === 0 || !shown.length ) { zoneSel.sort = sort; setZone( "sort" ); }
				else move( -1 );
			}
			else if ( down )
			{
				if ( sel >= shown.length - 1 ) setZone( "bottom" );
				else move( 1 );
			}
			else if ( b === "GP_LTRIGGER" || b === "PGUP" ) move( -VISIBLE );
			else if ( b === "GP_RTRIGGER" || b === "PGDN" ) move( VISIBLE );
			else if ( b === "HOME" ) move( -shown.length );
			else if ( b === "END" ) move( shown.length );
			else if ( ok || k === " " ) join();
			return;
		}
		// toolbar, sort row, bottom band: Left / Right along it, Up / Down between
		var n = ZONE_SIZE[ zone ];
		if ( left ) { zoneSel[ zone ] = ( zoneSel[ zone ] + n - 1 ) % n; sound( SND_NAV ); render(); }
		else if ( right ) { zoneSel[ zone ] = ( zoneSel[ zone ] + 1 ) % n; sound( SND_NAV ); render(); }
		else if ( up ) { if ( zone !== "toolbar" ) setZone( zone === "sort" ? "toolbar" : "list" ); }
		else if ( down ) { if ( zone !== "bottom" ) setZone( zone === "toolbar" ? "sort" : "list" ); }
		else if ( ok || k === " " ) activate( zone, zoneSel[ zone ] );
	}

	Bulwark.On( "key", onKey );

	makeRows();
	var startSource = SOURCES.indexOf( params.tab || params.show || "" );
	source = startSource < 0 ? 0 : startSource;
	if ( SOURCES[ source ] === "history" ) sort = SORT_PLAYED;
	refresh( true );
	if ( params.failed )
		notice( "Couldn't join " + params.failed + ": " + friendlyReason( params.reason ), 8 );
	tick();

	function mouse( z, i )
	{
		stopTyping();
		$.GetContextPanel().SetFocus();
		if ( dialog || joining ) return false;
		if ( z !== "popup" && z !== "toolbar" && popupOpen() ) zone = "list";
		zone = z;
		zoneSel[ z ] = i;
		return true;
	}

	return {
		Tool: function ( i )
		{
			if ( i === 1 ) { if ( !dialog && !joining ) startTyping(); return; }
			var wasPopup = popupOpen();
			if ( !mouse( "toolbar", i ) ) return;
			if ( i === 0 ) { if ( wasPopup ) zone = "popup"; openPopup(); }
			else refresh();
		},
		Sort: function ( i ) { if ( mouse( "sort", i ) ) setSort( i ); },
		FilterRow: function ( i ) { if ( mouse( "popup", i ) ) changeFilter( i, 1 ); },
		BottomBtn: function ( i ) { if ( mouse( "bottom", i ) ) activate( "bottom", i ); },
		Hover: function ( z, i )
		{
			// the mouse only moves the controller's highlight inside the zone it is in
			if ( z === zone && !typing ) { zoneSel[ z ] = i; renderBars(); }
		},
		SearchDone: function () { stopTyping(); zone = "list"; lastRowsKey = ""; render(); },
		DialogOk: function () { dialogOk(); },
		CloseDialog: function () { closeDialog(); },
		HoverDialog: function ( i ) { hoverDialog( i ); }
	};
} )();
