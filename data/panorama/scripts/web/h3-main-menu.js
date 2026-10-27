// Panorama port of Stiletto's data/web/h3-main-menu (app.js): the Halo 3
// style main menu mock-up. The menu data is the page's data.js
// (window.__H3_MENU_DATA__), which the layout loads from the game before this
// script. Like the page, nothing here talks to the engine: confirming a row
// opens a lobby page or the stub overlay that names the screen it stands for.
//
// Keys come as Bulwark "key" events, the mouse through onmouseover and
// onactivate. Transitions are CSS transitions with delays standing in for the
// page's hold-then-move keyframes (see h3-main-menu.css).
var H3Menu = (function ()
{
	var data = window.__H3_MENU_DATA__;
	var ART = "file://{game}/data/web/h3-main-menu/";
	var TRANSITION_MS = 666;

	function art( path ) { return "url(\"" + ART + path + "\")"; }

	var screenEl = $( "#Screen" );
	var pageMain = $( "#PageMain" );
	var pageLobby = $( "#PageLobby" );
	var overlay = $( "#Overlay" );
	var debugEl = $( "#Debug" );

	// --- mouse: only real movement moves focus -------------------------------------

	var mouseStart = null;
	var mouseMoved = false;
	function mouseHasMoved()
	{
		if ( mouseMoved )
			return true;
		var at = [ Bulwark.MouseX(), Bulwark.MouseY() ];
		if ( !mouseStart )
			mouseStart = at;
		else if ( at[ 0 ] !== mouseStart[ 0 ] || at[ 1 ] !== mouseStart[ 1 ] )
			mouseMoved = true;
		return mouseMoved;
	}

	// --- ListController: rows built once, focus toggles a class ---------------------
	// The bar shows at once on focus and fades out over 120 ms on the row that
	// lost it (the "ghost trail"), as the page's asymmetric transition does.

	function ListController( opts )
	{
		var items = opts.items;
		var rowEls = [];
		var focus = 0;

		function makeRow( i )
		{
			var item = items[ i ];
			var row = $.CreatePanel( "Button", opts.parentFor( i ), "", { "class": opts.rowClass, hittest: "true" } );
			row.SetHasClass( "disabled", !!item.disabled );
			var bar = $.CreatePanel( "Panel", row, "", { "class": "hover-bar", hittest: "false" } );
			$.CreatePanel( "Panel", bar, "", { "class": "hover-fill", hittest: "false" } );
			$.CreatePanel( "Panel", bar, "", { "class": "hover-glow", hittest: "false" } );
			opts.buildRowContent( row, item );
			row.SetPanelEvent( "onactivate", function ()
			{
				if ( transitioning )
					return;
				if ( i === focus )
					confirm();
				else
					setFocus( i );
			} );
			row.SetPanelEvent( "onmouseover", function ()
			{
				if ( !transitioning && mouseHasMoved() && focus !== i )
					setFocus( i );
			} );
			return row;
		}

		function build()
		{
			opts.clear();
			rowEls.length = 0;
			for ( var i = 0; i < items.length; i++ )
				rowEls[ i ] = makeRow( i );
			applyFocus();
		}

		function applyFocus()
		{
			for ( var i = 0; i < rowEls.length; i++ )
				rowEls[ i ].SetHasClass( "focused", i === focus );
			$( "#DebugFocus" ).text = "focus: " + focus + " (" + items[ focus ].id + ")";
		}

		function setFocus( i )
		{
			if ( focus === i )
				return;
			focus = i;
			applyFocus();
		}

		function moveFocus( delta )
		{
			var next;
			if ( opts.wraps )
				next = ( focus + delta + items.length ) % items.length;
			else
				next = Math.max( 0, Math.min( items.length - 1, focus + delta ) );
			setFocus( next );
		}

		function confirm()
		{
			var item = items[ focus ];
			if ( item.disabled )
			{
				var row = rowEls[ focus ];
				row.RemoveClass( "shake" );
				$.Schedule( 0, function () { row.AddClass( "shake" ); } );
				$.Schedule( 0.25, function () { row.RemoveClass( "shake" ); } );
				return;
			}
			opts.onConfirm( item );
		}

		return {
			build: build,
			applyFocus: applyFocus,
			moveFocus: moveFocus,
			confirm: confirm,
			items: items
		};
	}

	// --- main menu: items[0] is the header slot, the rest the body --------------------
	// The engine prunes rows by game state; the page filters hidden_by_default.

	$( "#TitleLogo" ).style.backgroundImage = art( data.right_group_chrome.title_bitmap );
	$( "#BungieImage" ).style.backgroundImage = art( data.right_group_chrome.bungie_bitmap );
	$( "#BuildText" ).text = "build 12070.07.10.18.2128.delta";
	$( "#VersionText" ).text = "version 1.0";

	var primaryEl = $( "#MenuPrimary" );
	var mainListEl = $( "#MainList" );

	var mainList = ListController( {
		items: data.list.items.filter( function ( it ) { return !it.hidden_by_default; } ),
		wraps: data.list.wraps,
		rowClass: "mm-row",
		parentFor: function ( i ) { return i === 0 ? primaryEl : mainListEl; },
		clear: function ()
		{
			primaryEl.RemoveAndDeleteChildren();
			mainListEl.RemoveAndDeleteChildren();
		},
		buildRowContent: function ( row, item )
		{
			$.CreatePanel( "Label", row, "", { "class": "t mm-row-label", text: item.label, hittest: "false" } );
		},
		onConfirm: function ( item ) { navigateTo( item.destination ); }
	} );

	// --- pregame lobby: one template, filled per mode on entry ------------------------

	var lobbyList = null;
	var lobbyMode = null;

	function buildLobbyList( mode )
	{
		var cfg = data.pregame_lobbies.modes[ mode ];
		$( "#LobbyTitle" ).text = cfg.title;
		$( "#LobbyStatus" ).text = cfg.lobby_status;
		$( "#LobbyGameName" ).text = cfg.game_name;
		pageLobby.SetHasClass( "no-preview", !cfg.show_preview );

		// the preview tile names whatever picks the level/variant/map/film/hopper
		var previewTargets = [ "level", "variant", "map", "film", "hopper" ];
		var gameItem = null;
		for ( var i = 0; i < cfg.items.length && !gameItem; i++ )
			if ( previewTargets.indexOf( cfg.items[ i ].target ) >= 0 )
				gameItem = cfg.items[ i ];
		$( "#LobbyGametype" ).text = ( gameItem && gameItem.value ) || cfg.title;

		var listEl = $( "#LobbyList" );
		lobbyList = ListController( {
			items: cfg.items,
			wraps: true,
			rowClass: "lb-row",
			parentFor: function () { return listEl; },
			clear: function () { listEl.RemoveAndDeleteChildren(); },
			// "LABEL: VALUE" on one line; the primary action sits between hairlines
			buildRowContent: function ( row, item )
			{
				if ( item.id === "start_game" )
					row.AddClass( "separated" );
				var text = item.label + ( item.value ? ": " + item.value : "" );
				$.CreatePanel( "Label", row, "", { "class": "t lb-row-label", text: text.toUpperCase(), hittest: "false" } );
			},
			onConfirm: function ( item )
			{
				if ( item.id === "switch_lobby" )
				{
					back();
					return;
				}
				if ( item.id === "start_game" )
				{
					openOverlay( "STARTING GAME", "mode=" + mode + " -> " + data.pregame_lobbies.modes[ mode ].game_name );
					return;
				}
				openOverlay( ( item.label + ( item.target ? " (" + item.target + ")" : "" ) ).toUpperCase(),
					cfg.datasource + ".dsrc -> " + item.id );
			}
		} );
		lobbyList.build();
	}

	// --- roster: 16 slots, only the local player filled -------------------------------
	// Empty slots aren't drawn (retail paints only rows backed by a player);
	// the header still counts the seats.

	var ROSTER_SLOT_COUNT = 16;
	var initialRoster = [ { name: "Player 1", serviceTag: "PL1", focused: true } ];

	// base_color_hilite: a blue ramp (hsl 206 75% 55% to hsl 208 75% 32%, 95%)
	// through the white roster_unfocused_ui mask, 426.8 x 66 at x 40.6, y -18.
	// Panorama has no masks, but a wash tints the mask image, so the strip is
	// cut into bands, each washed with the ramp's colour at its middle.
	var BASE_TOP = [ 54, 152, 226 ], BASE_BOTTOM = [ 20, 86, 143 ], BASE_BANDS = 11;
	function buildRosterBase( slot )
	{
		var h = 66, band = h / BASE_BANDS;
		for ( var k = 0; k < BASE_BANDS; k++ )
		{
			var t = ( k + 0.5 ) / BASE_BANDS, rgb = [];
			for ( var c = 0; c < 3; c++ )
				rgb[ c ] = Math.round( BASE_TOP[ c ] + ( BASE_BOTTOM[ c ] - BASE_TOP[ c ] ) * t );
			var clip = $.CreatePanel( "Panel", slot, "", { "class": "roster-base-band", hittest: "false",
				style: "y:" + ( -18 + k * band ).toFixed( 2 ) + "px;height:" + band.toFixed( 2 ) + "px;" } );
			$.CreatePanel( "Panel", clip, "", { "class": "roster-base img", hittest: "false",
				style: "y:" + ( -k * band ).toFixed( 2 ) + "px;wash-color:rgb(" + rgb.join( "," ) + ");background-image:" +
					art( "assets/roster/roster_unfocused_ui.png" ) + ";" } );
		}
	}

	function buildRosterSlots()
	{
		var list = $( "#LobbyRoster" );
		list.RemoveAndDeleteChildren();
		var filled = 0;
		for ( var i = 0; i < initialRoster.length; i++ )
		{
			var seed = initialRoster[ i ];
			var slot = $.CreatePanel( "Panel", list, "", { "class": "roster-slot", hittest: "false",
				style: "y:" + ( i * 47.95 ).toFixed( 2 ) + "px;" } );
			buildRosterBase( slot );
			$.CreatePanel( "Label", slot, "", { "class": "t roster-name", text: seed.name, hittest: "false" } );
			$.CreatePanel( "Label", slot, "", { "class": "t roster-tag", text: seed.serviceTag || "", hittest: "false" } );
			filled++;
		}
		$( "#LobbyPlayersCount" ).text = String( filled );
		$( "#LobbyPlayersMax" ).text = String( ROSTER_SLOT_COUNT );
	}

	// --- button-key prompts -------------------------------------------------------------

	var online = false;

	function renderButtonKeys( bar, prompts )
	{
		bar.RemoveAndDeleteChildren();
		for ( var i = 0; i < prompts.length; i++ )
		{
			var p = prompts[ i ];
			var key = $.CreatePanel( "Panel", bar, "", { "class": "button-key", hittest: "false" } );
			var glyph = $.CreatePanel( "Panel", key, "", { "class": "button-key-glyph glyph-" + p.glyph, hittest: "false" } );
			$.CreatePanel( "Label", glyph, "", { "class": "t", text: p.glyph, hittest: "false" } );
			$.CreatePanel( "Label", key, "", { "class": "t button-key-label", text: p.label, hittest: "false" } );
		}
	}

	function renderMainButtonKeys()
	{
		renderButtonKeys( $( "#ButtonKeys" ), data.button_keys[ online ? "main_menu_online" : "main_menu_offline" ] );
	}

	// --- page router -----------------------------------------------------------------
	// The outgoing page plays transition-from and the incoming one transition-to,
	// together, over one 666 ms beat; input waits for it.

	var pageEls = { main_menu: pageMain, pregame_lobby: pageLobby };
	var currentPage = "main_menu";
	var activeList = mainList;
	var transitioning = false;

	function setPhase( el, phase )
	{
		el.SetHasClass( "entering", phase === "entering" );
		el.SetHasClass( "leaving", phase === "leaving" );
	}

	function showPage( targetPage, mode )
	{
		if ( transitioning || targetPage === currentPage )
			return;
		var fromEl = pageEls[ currentPage ];
		var toEl = pageEls[ targetPage ];
		transitioning = true;

		if ( targetPage === "pregame_lobby" )
		{
			lobbyMode = mode;
			buildLobbyList( mode );
			renderButtonKeys( $( "#LobbyButtonKeys" ), data.pregame_lobbies.template.button_keys );
		}
		else
			lobbyMode = null;

		toEl.SetHasClass( "hidden", false );
		setPhase( fromEl, "leaving" );
		setPhase( toEl, "entering" );
		fromEl.AddClass( "off" );
		toEl.RemoveClass( "off" );

		$.Schedule( ( TRANSITION_MS + 20 ) / 1000, function ()
		{
			fromEl.AddClass( "hidden" );
			setPhase( fromEl, "" );
			setPhase( toEl, "" );
			currentPage = targetPage;
			$( "#DebugScreen" ).text = "screen: " + targetPage;
			activeList = targetPage === "main_menu" ? mainList : lobbyList;
			activeList.applyFocus();
			transitioning = false;
		} );
	}

	// --- destinations -----------------------------------------------------------------

	function navigateTo( destination )
	{
		if ( destination === "_quit" )
		{
			// real Halo quits to the dashboard; the mock-up dims for a moment
			var stage = $( "#Stage" );
			stage.AddClass( "quitting" );
			$.Schedule( 1.2, function () { stage.RemoveClass( "quitting" ); } );
			return;
		}
		if ( destination.indexOf( "pregame_lobby_" ) === 0 )
		{
			var mode = destination.slice( "pregame_lobby_".length );
			if ( data.pregame_lobbies.modes[ mode ] )
			{
				showPage( "pregame_lobby", mode );
				$( "#DebugNav" ).text = "nav: " + destination;
				return;
			}
		}
		// a screen the mock-up doesn't model: name the tag it would open
		var tag = data.navigation_table[ destination ] || "(no widget tag - engine-side)";
		openOverlay( destination.replace( /_/g, " " ).toUpperCase(), tag + ".scn3" );
		$( "#DebugNav" ).text = "nav: " + destination;
	}

	function openOverlay( title, tag )
	{
		$( "#OverlayTitle" ).text = title;
		$( "#OverlayTag" ).text = tag;
		overlay.SetHasClass( "hidden", false );
	}

	function closeOverlay()
	{
		overlay.SetHasClass( "hidden", true );
		$( "#DebugNav" ).text = "nav: -";
	}

	function overlayOpen() { return !overlay.BHasClass( "hidden" ); }

	function back()
	{
		if ( overlayOpen() )
		{
			closeOverlay();
			return;
		}
		if ( currentPage === "pregame_lobby" )
			showPage( "main_menu" );
		// main_menu has "B-Back shouldn't dispose screen": nothing to do
	}

	function toggleOnline()
	{
		online = !online;
		$( "#DebugOnlineLabel" ).text = "online: [" + ( online ? "x" : " " ) + "]";
		renderMainButtonKeys();
	}

	// --- keys ------------------------------------------------------------------------------

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down )
			return;
		var key = e.key;
		if ( key === "`" )
		{
			debugEl.SetHasClass( "hidden", !debugEl.BHasClass( "hidden" ) );
			return;
		}
		if ( transitioning )
			return;
		switch ( key )
		{
			case "ArrowUp": case "w": case "W":
				activeList.moveFocus( -1 );
				break;
			case "ArrowDown": case "s": case "S":
				activeList.moveFocus( 1 );
				break;
			case "Enter": case "a": case "A": case " ":
				activeList.confirm();
				break;
			case "Escape": case "b": case "B":
				back();
				break;
			case "x": case "X":
				navigateTo( "start_menu_settings" );
				break;
			case "y": case "Y":
				if ( currentPage === "pregame_lobby" )
					openOverlay( "ROSTER", "ui\\halox\\common\\roster\\roster.grup" );
				else if ( online )
					navigateTo( "start_menu_hq" );
				break;
		}
	} );

	// --- letterbox: the 1920 x 1080 stage shrinks to fit a view narrower than 16:9 -----

	var builtScale = 0;
	function watchSize()
	{
		var w = Bulwark.Width(), h = Bulwark.Height();
		var k = w > 0 && h > 0 ? Math.min( 1, w / 1920, h / 1080 ) : 1;
		if ( Math.abs( k - builtScale ) > 0.0001 )
		{
			builtScale = k;
			$( "#Stage" ).style.transform = k < 1 ? "scale3d( " + k.toFixed( 4 ) + ", " + k.toFixed( 4 ) + ", 1 )" : "none";
		}
		$.Schedule( 0.5, watchSize );
	}

	// --- first paint -------------------------------------------------------------------
	// The main menu plays its enter transition once (the page's .is-initial),
	// the logos their delayed fade, and the black intro fade clears.

	mainList.build();
	renderMainButtonKeys();
	buildRosterSlots();
	watchSize();
	mouseHasMoved();
	$.Schedule( 0.05, function ()
	{
		screenEl.RemoveClass( "intro" );
		$( "#BlackFade" ).AddClass( "done" );
		setPhase( pageMain, "entering" );
		pageMain.RemoveClass( "off" );
		$.Schedule( TRANSITION_MS / 1000, function () { setPhase( pageMain, "" ); } );
	} );

	return {
		CloseOverlay: closeOverlay,
		ToggleOnline: toggleOnline,
		// for tests through ui_eval
		state: function () { return { page: currentPage, mode: lobbyMode, overlay: overlayOpen(), online: online }; },
		key: function ( k ) { Bulwark.__recv( "key", { key: k, down: true } ); }
	};
})();
