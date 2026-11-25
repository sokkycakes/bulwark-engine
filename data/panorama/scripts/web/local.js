// Local Play (layout/web/local.xml). The seats and settings come from
// fte_query("getlocal"); changes go back as fte_query("local:..."). Key
// events say which controller (device) and player (seat) they came from, so
// a controller nobody has joins with Start or A, and a guest's B leaves.
var Local = ( function ()
{
	var SND_NAV = "misc/menu1", SND_OK = "misc/menu2", SND_BACK = "misc/menu3";
	var ROWS = 5; // mode, map, set length or bots, screen, start
	var SPLIT2 = [ [ "horizontal", "Top and bottom" ], [ "vertical", "Side by side" ] ];
	var SPLIT3 = [ [ "top", "One wide on top" ], [ "bottom", "One wide below" ], [ "left", "One tall on the left" ],
		[ "right", "One tall on the right" ], [ "quad", "Quarters" ] ];

	var g = null;      // the last getlocal reply
	var selected = 0;
	var seatKey = "";
	var seatPanels = [];
	var leaving = false;

	function q( req )
	{
		if ( typeof fte_query !== "function" )
			return null;
		try { return fte_query( req ); } catch ( e ) { return null; }
	}
	function sound( s ) { q( "localsound:" + s ); }
	function ok( reply ) { try { return JSON.parse( reply || "{}" ).ok === true; } catch ( e ) { return false; } }
	function local( verb ) { return ok( q( "local:" + verb ) ); }
	function set( key, val ) { return local( "set:" + key + ":" + val ); }
	function mapName( m ) { return String( m || "" ).replace( /_/g, " " ); }

	// --- the players ------------------------------------------------------------------

	function buildSeats()
	{
		var parent = $( "#Seats" );
		parent.RemoveAndDeleteChildren();
		seatPanels = [];
		for ( var k = 0; k < 4; k++ )
		{
			var p = $.CreatePanel( "Panel", parent, "Seat" + k, { class: "seat" + ( k === 3 ? " is-last" : "" ), hittest: "false" } );
			var tag = $.CreatePanel( "Panel", p, "", { class: "seat-tag", hittest: "false" } );
			$.CreatePanel( "Label", tag, "", { text: "P" + ( k + 1 ), hittest: "false" } );
			seatPanels.push( {
				panel: p,
				state: $.CreatePanel( "Label", p, "", { class: "seat-state", text: "", hittest: "false" } ),
				name: $.CreatePanel( "Label", p, "", { class: "seat-name", text: "", hittest: "false" } ),
				dev: $.CreatePanel( "Label", p, "", { class: "seat-dev", text: "", hittest: "false" } ),
				join: $.CreatePanel( "Label", p, "", { class: "seat-join", text: "", hittest: "false" } ),
				hint: $.CreatePanel( "Label", p, "", { class: "seat-hint", text: "", hittest: "false" } )
			} );
		}
	}

	function paintSeats()
	{
		var seats = g.seats || [];
		for ( var k = 0; k < seatPanels.length; k++ )
		{
			var sp = seatPanels[ k ], s = seats[ k ] || { on: false };
			var joining = s.on && s.state !== "ready" && k > 0;
			sp.panel.SetHasClass( "is-on", !!s.on );
			sp.panel.SetHasClass( "is-joining", joining );
			sp.state.text = !s.on ? "" : k === 0 ? "Host" : "Guest";
			sp.name.text = s.on ? s.name : "";
			sp.dev.text = s.on ? ( s.devices || "No controller" ) : "";
			sp.join.text = s.on ? "" : "Press START\nto join";
			sp.hint.text = !s.on ? "" : k === 0 ? "" : "B on their controller: leave";
		}
	}

	// --- settings ---------------------------------------------------------------------

	function versus() { return g && g.mode === "versus"; }

	function splitChoices()
	{
		var n = g ? g.players : 1;
		return n === 2 ? SPLIT2 : n === 3 ? SPLIT3 : null;
	}

	function splitIndex( choices )
	{
		var cur = g ? ( choices === SPLIT3 ? g.split3 : g.split ) : "";
		for ( var i = 0; i < choices.length; i++ )
			if ( choices[ i ][ 0 ] === cur || ( choices === SPLIT2 && cur === "1" && i === 1 ) )
				return i;
		return 0;
	}

	function paintRows()
	{
		var vs = versus();
		$( "#Val0" ).text = vs ? "Versus" : "Free for all";
		$( "#Desc0" ).text = vs ? ( g.players < 2 ? "You against a bot, first to " + g.firstto + " games" :
			"Two players, first to " + g.firstto + " games, rematch after" ) : "Everyone against everyone";
		$( "#Val1" ).text = mapName( g.map );
		$( "#Desc1" ).text = ( g.maps || [] ).length + " maps";
		$( "#Name2" ).text = vs ? "Set" : "Bots";
		$( "#Val2" ).text = vs ? "First to " + g.firstto : String( g.bots );
		$( "#Desc2" ).text = vs ? "Each game is best of 3 rounds" : g.bots ? "Bots join when the game starts" : "No bots";
		var choices = splitChoices();
		$( "#Row3" ).SetHasClass( "is-off", !choices );
		$( "#Val3" ).text = choices ? choices[ splitIndex( choices ) ][ 1 ] : g.players >= 4 ? "Quarters" : "Full screen";
		$( "#Desc3" ).text = choices ? "How the screen is shared" : g.players >= 4 ? "Four players get a quarter each" : "One player";
		$( "#Problem" ).text = g.problem || "";
		$( "#Row4" ).enabled = !g.problem;
		for ( var i = 0; i < ROWS; i++ )
			$( "#Row" + i ).SetHasClass( "is-selected", i === selected );
	}

	function change( row, dir )
	{
		if ( !g )
			return;
		var done = false;
		if ( row === 0 )
			done = set( "mode", versus() ? "ffa" : "versus" );
		else if ( row === 1 )
		{
			var maps = g.maps || [];
			if ( maps.length )
			{
				var at = maps.indexOf( g.map );
				done = set( "map", maps[ ( at + dir + maps.length ) % maps.length ] );
			}
		}
		else if ( row === 2 )
		{
			if ( versus() )
				done = set( "firstto", ( ( g.firstto - 1 + dir + 5 ) % 5 ) + 1 );
			else
				done = set( "bots", ( g.bots + dir + 7 ) % 7 );
		}
		else if ( row === 3 )
		{
			var choices = splitChoices();
			if ( choices )
			{
				var i = ( splitIndex( choices ) + dir + choices.length ) % choices.length;
				done = set( choices === SPLIT3 ? "split3" : "split", choices[ i ][ 0 ] );
			}
		}
		if ( done )
		{
			sound( SND_NAV );
			poll( true );
		}
	}

	function select( i )
	{
		i = Math.max( 0, Math.min( ROWS - 1, i ) );
		if ( i === selected )
			return;
		selected = i;
		sound( SND_NAV );
		if ( g )
			paintRows();
	}

	function start()
	{
		if ( leaving || !g || g.problem )
			return;
		if ( local( "start" ) )
		{
			leaving = true;
			sound( SND_OK );
		}
	}

	function back()
	{
		if ( leaving )
			return;
		leaving = true;
		sound( SND_BACK );
		local( "back" );
	}

	function activate()
	{
		if ( selected === 4 )
			start();
		else
			change( selected, 1 );
	}

	// --- keys --------------------------------------------------------------------------

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down || !g || leaving )
			return;
		var k = e.key || "", b = String( e.bind || "" ).toUpperCase();
		var dev = typeof e.device === "number" ? e.device : 0;
		var seat = typeof e.seat === "number" ? e.seat : 0;
		var pad = dev > 0;
		// a controller nobody has: Start or A joins. The first player's
		// controller (shared with the keyboard) leaves them for a seat of
		// its own with Start.
		if ( pad && seat < 0 )
		{
			if ( b === "GP_A" || b === "GP_MENU" || b === "GP_START" )
			{
				if ( local( "join:" + dev ) )
					sound( SND_OK );
				poll( true );
			}
			return;
		}
		if ( pad && seat === 0 && ( b === "GP_MENU" || b === "GP_START" ) )
		{
			if ( local( "join:" + dev ) )
			{
				sound( SND_OK );
				poll( true );
			}
			return;
		}
		// a guest: B leaves; their other buttons drive the menu too
		if ( seat > 0 && b === "GP_B" )
		{
			if ( local( "leave:" + seat ) )
				sound( SND_BACK );
			poll( true );
			return;
		}
		if ( seat > 0 && ( b === "GP_MENU" || b === "GP_START" ) )
			return; // the Start that just seated them
		var up = k === "ArrowUp" || k === "w" || k === "W" || b === "GP_DPAD_UP" || k === "WheelUp";
		var down = k === "ArrowDown" || k === "s" || k === "S" || b === "GP_DPAD_DOWN" || k === "WheelDown";
		var left = k === "ArrowLeft" || k === "a" || k === "A" || b === "GP_DPAD_LEFT";
		var right = k === "ArrowRight" || k === "d" || k === "D" || b === "GP_DPAD_RIGHT";
		if ( up ) select( selected - 1 );
		else if ( down ) select( selected + 1 );
		else if ( left && selected < 4 ) change( selected, -1 );
		else if ( right && selected < 4 ) change( selected, 1 );
		else if ( k === "Enter" || k === " " || b === "GP_A" ) activate();
		else if ( k === "Escape" || b === "GP_B" ) back();
	} );

	// --- frame -------------------------------------------------------------------------

	function poll( once )
	{
		var raw = q( "getlocal" );
		try { g = JSON.parse( raw || "null" ); } catch ( e ) { g = null; }
		if ( g )
		{
			var key = JSON.stringify( g.seats );
			if ( key !== seatKey )
			{
				seatKey = key;
				paintSeats();
			}
			paintRows();
		}
		if ( !once )
			$.Schedule( 0.2, function () { poll( false ); } );
	}

	buildSeats();
	poll( false );

	return {
		Hover: function ( i ) { select( i ); },
		Click: function ( i )
		{
			$.GetContextPanel().SetFocus();
			selected = i;
			activate();
		},
		Back: function () { $.GetContextPanel().SetFocus(); back(); }
	};
} )();
