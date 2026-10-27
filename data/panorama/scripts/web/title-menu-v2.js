// Title menu v2 (port of data/web/title-menu-v2/app.js). Same requests as the
// page: cbuf:, localsound:, hostcache_refresh / gethostcache, lobby_join:.
// Keys come as Bulwark "key" events, mouse through onactivate / onmouseover.
var TitleMenu = (function ()
{
	var COUNT = 4;
	// classic Quake menu sounds, as the page plays them
	var SND_NAV = "misc/menu1";
	var SND_OK = "misc/menu2";
	var SND_BACK = "misc/menu3";
	var TRANSITION_MS = 180;

	var stage = $( "#Stage" );
	var titleChrome = $( "#TitleChrome" );
	var joinDialog = $( "#JoinDialog" );
	var joinList = $( "#JoinList" );
	var joinConnect = $( "#JoinConnect" );
	var selected = 0;
	var selectedAddr = null;
	var joinOpen = false;
	var transitioning = true;
	var lanRows = [];

	function q( req )
	{
		if ( typeof fte_query !== "function" )
			return null;
		try { return fte_query( req ); } catch ( e ) { return null; }
	}
	function cbuf( cmd )
	{
		if ( !cmd )
			return false;
		return q( "cbuf:" + cmd ) === "ok";
	}
	function localsound( sample )
	{
		if ( sample )
			q( "localsound:" + sample );
	}

	function afterTransition( fn )
	{
		$.Schedule( TRANSITION_MS / 1000, fn );
	}

	// a click leaves the button focused, and Panorama activates a focused
	// button on Enter as well; park focus on the root so keys only go
	// through the key handler
	function dropFocus()
	{
		$.GetContextPanel().SetFocus();
	}

	function setTitleHidden( hidden )
	{
		titleChrome.SetHasClass( "is-hidden", hidden );
	}

	function setDialogAvailable( available )
	{
		joinDialog.SetHasClass( "is-hidden", !available );
	}

	function focusJoinField()
	{
		if ( joinOpen )
			$( "#JoinAddr" ).SetFocus();
	}

	function select( index, playNav )
	{
		if ( index < 0 ) index = 0;
		if ( index >= COUNT ) index = COUNT - 1;
		if ( index === selected )
			return;
		selected = index;
		for ( var i = 0; i < COUNT; i++ )
		{
			$( "#Item" + i ).SetHasClass( "is-selected", i === selected );
			if ( i > 0 )
				stage.SetHasClass( "sel-" + i, i === selected );
		}
		if ( playNav !== false )
			localsound( SND_NAV );
	}

	function openJoin( create )
	{
		$( "#JoinTitle" ).text = create ? "Create Lobby" : "Join Lobby";
		joinDialog.SetHasClass( "join-create", !!create );
		joinDialog.SetHasClass( "join-join", !create );
		if ( transitioning || joinOpen )
			return;
		transitioning = true;
		joinOpen = true;
		selectedAddr = null;
		joinConnect.enabled = false;
		if ( !create )
			refreshLan();
		setTitleHidden( true );
		afterTransition( function ()
		{
			joinDialog.SetHasClass( "hidden", false );
			// one frame visible at opacity 0, so the fade has a start
			$.Schedule( 0, function ()
			{
				setDialogAvailable( true );
				if ( !create )
					focusJoinField();
				afterTransition( function () { transitioning = false; } );
			} );
		} );
	}

	function closeJoin( playBack )
	{
		if ( transitioning || !joinOpen )
			return;
		transitioning = true;
		dropFocus();
		if ( playBack !== false )
			localsound( SND_BACK );
		setDialogAvailable( false );
		afterTransition( function ()
		{
			joinDialog.SetHasClass( "hidden", true );
			joinOpen = false;
			setTitleHidden( false );
			afterTransition( function () { transitioning = false; } );
		} );
	}

	function leaveTitle( cmd )
	{
		if ( transitioning || !cmd )
			return;
		transitioning = true;
		localsound( SND_OK );
		setTitleHidden( true );
		cbuf( cmd );
		if ( cmd === "menu_options" )
		{
			afterTransition( function ()
			{
				setTitleHidden( false );
				transitioning = false;
			} );
		}
	}

	function selectLan( index )
	{
		if ( transitioning || !joinOpen )
			return;
		var row = lanRows[ index ];
		if ( !row )
			return;
		selectedAddr = row.addr;
		for ( var i = 0; i < lanRows.length; i++ )
			lanRows[ i ].panel.SetHasClass( "is-selected", i === index );
		joinConnect.enabled = true;
		localsound( SND_NAV );
		dropFocus();
	}

	function addLanRow( r )
	{
		var index = lanRows.length;
		var panel = $.CreatePanel( "Button", joinList, "", { class: "lan-row" } );
		$.CreatePanel( "Label", panel, "", { class: "lan-name", text: String( r.name || r.addr ) } );
		$.CreatePanel( "Label", panel, "", { class: "lan-meta",
			text: String( r.map || "" ) + " · " + ( r.players || 0 ) + "/" + ( r.max || 0 ) + ( r.lobby ? " · LOBBY" : "" ) } );
		panel.SetPanelEvent( "onactivate", function () { selectLan( index ); } );
		lanRows.push( { addr: String( r.addr ), panel: panel } );
	}

	function renderLan( rows )
	{
		var anyLobby = rows.some( function ( x ) { return x && x.lobby; } );
		joinList.RemoveAndDeleteChildren();
		lanRows = [];
		for ( var i = 0; i < rows.length; i++ )
		{
			var r = rows[ i ];
			if ( !r || !r.addr )
				continue;
			if ( r.lobby === false && anyLobby )
				continue;
			addLanRow( r );
		}
		if ( !lanRows.length )
			$.CreatePanel( "Label", joinList, "", { class: "join-empty",
				text: "No LAN lobbies found — try Address join (127.0.0.1:27501)" } );
		selectedAddr = null;
		joinConnect.enabled = false;
	}

	function refreshLan()
	{
		q( "hostcache_refresh" );
		var raw = q( "gethostcache" );
		var rows = [];
		try { rows = JSON.parse( raw || "[]" ); } catch ( e ) { rows = []; }
		if ( !Array.isArray( rows ) )
			rows = [];
		renderLan( rows );
	}

	function joinAddr( addr )
	{
		if ( !addr || transitioning || !joinOpen )
			return;
		transitioning = true;
		if ( q( "lobby_join:" + addr ) !== "ok" )
		{
			transitioning = false;
			return;
		}
		localsound( SND_OK );
		setDialogAvailable( false );
		cbuf( "menu_webcore_lobby" );
	}

	function stripped( id )
	{
		var text = $( id ).text;
		return text ? String( text ).replace( /\s+/g, "" ) : "";
	}

	function joinCode()
	{
		var code = stripped( "#JoinCode" );
		if ( code )
			joinAddr( code );
	}

	function activate()
	{
		if ( transitioning || joinOpen )
			return;
		if ( selected === 0 )
		{
			openJoin( true );
			return;
		}
		if ( selected === 1 )
		{
			localsound( SND_OK );
			openJoin( false );
			return;
		}
		leaveTitle( selected === 2 ? "menu_options" : "quit" );
	}

	function createLobby( online )
	{
		dropFocus();
		if ( cbuf( online ? "lobby_create_online" : "lobby_create_lan" ) )
			cbuf( "menu_webcore_lobby" );
	}

	// the backdrop: the page masks a #333 frame and its tiled "stiletto"
	// wordmark with a radial alpha mask (viewport-mask.svg) and draws the
	// tile from an SVG. Panorama has neither masks nor SVG, so the frame is
	// rebuilt from rows whose gradients sample the mask, and the wordmark is
	// cut out of the game's raster logo (panorama/images/titlemenu/logo.png,
	// the same artwork) in slices whose opacity samples the mask.
	var FW = 1365, FH = 768;
	var TILE_W = 428.224, TILE_H = 70.272;
	// design pixels per artboard unit, vertically (the view is 1080 high)
	var PX = 1080 / FH;
	// logotile.svg is 429 x 71 and keeps its aspect in the stretched tile box
	// (meet): at 4:3 and wider it fills the tile's height and is centred, so
	// in design pixels its scale doesn't depend on the view's width
	var LS = TILE_H * PX / 71;
	// the tile's wordmark is the logo's first line, 1.824 units in
	var LOGO_X = 1.824, LOGO_W = 460, LOGO_H = 204;
	// where the logo's second line ("swift") reaches up into the first
	var SWIFT_X0 = 174, SWIFT_X1 = 256, SWIFT_TOP = 62.5;
	var SLICES = [ LOGO_X, 87, 174, 256, 343, LOGO_X + 429 ];
	// frame units per design pixel across, at 16:9 (only used to sample the mask)
	var FX_PER_PX = FW / ( 1920 * FW / 1024 );

	function maskAt( x, y )
	{
		var t = 61.1842 * Math.PI / 180;
		var px = x - 339.5, py = y - 9;
		var u = ( Math.cos( t ) * px + Math.sin( t ) * py ) / 745.857;
		var v = ( -Math.sin( t ) * px + Math.cos( t ) * py ) / 1325.64;
		var r = Math.sqrt( u * u + v * v );
		if ( r <= 0.4375 ) return 0;
		if ( r <= 0.774038 ) return 0.452632 * ( r - 0.4375 ) / ( 0.774038 - 0.4375 );
		if ( r <= 1 ) return 0.452632 + ( 1 - 0.452632 ) * ( r - 0.774038 ) / ( 1 - 0.774038 );
		return 1;
	}

	function pct( v, total ) { return ( v / total * 100 ).toFixed( 4 ) + "%"; }
	function px( v ) { return v.toFixed( 3 ) + "px"; }

	function hex2( a )
	{
		var n = Math.max( 0, Math.min( 255, Math.round( a * 255 ) ) );
		return ( n < 16 ? "0" : "" ) + n.toString( 16 );
	}

	function buildBase( frame )
	{
		var ROWS = 48, STOPS = 24;
		for ( var r = 0; r < ROWS; r++ )
		{
			var y0 = r * FH / ROWS, y1 = ( r + 1 ) * FH / ROWS, cy = ( y0 + y1 ) / 2;
			var stops = [];
			for ( var s = 0; s <= STOPS; s++ )
				stops.push( "color-stop( " + ( s / STOPS ).toFixed( 4 ) + ", #333333" + hex2( maskAt( s / STOPS * FW, cy ) ) + " )" );
			$.CreatePanel( "Panel", frame, "", { class: "bg-row", style:
				"y: " + pct( y0, FH ) + "; height: " + pct( y1 - y0, FH ) + ";" +
				"background-color: gradient( linear, 0% 0%, 100% 0%, " + stops.join( ", " ) + " );" } );
		}
	}

	// the visible rows [vy0, vy1] of the tile whose box starts at (tx, ty):
	// a panel over the tile box, the wordmark centred in it, cut into slices
	function addTile( frame, tx, ty, vy0, vy1 )
	{
		if ( vy1 - vy0 < 0.25 )
			return;
		var tile = $.CreatePanel( "Panel", frame, "", { class: "bg-tile", style:
			"x: " + pct( tx, FW ) + "; y: " + pct( vy0, FH ) + "; width: " + pct( TILE_W, FW ) + "; height: " + pct( vy1 - vy0, FH ) + ";" } );
		var mark = $.CreatePanel( "Panel", tile, "", { class: "bg-mark", style: "width: " + px( 429 * LS ) + ";" } );
		for ( var s = 0; s + 1 < SLICES.length; s++ )
		{
			var l0 = SLICES[ s ], l1 = SLICES[ s + 1 ];
			var bottom = Math.min( vy1, ty + ( l0 >= SWIFT_X0 && l1 <= SWIFT_X1 ? SWIFT_TOP : 71 ) * TILE_H / 71 );
			if ( bottom - vy0 < 0.25 )
				continue;
			var cx = tx + TILE_W / 2 + ( ( l0 + l1 ) / 2 - LOGO_X - 214.5 ) * LS * FX_PER_PX;
			var a = 0.11 * maskAt( cx, ( vy0 + bottom ) / 2 );
			if ( a < 0.004 )
				continue;
			$.CreatePanel( "Panel", mark, "", { class: "bg-slice", style:
				"x: " + px( ( l0 - LOGO_X ) * LS ) + "; width: " + px( ( l1 - l0 ) * LS ) + "; height: " + px( ( bottom - vy0 ) * PX ) + ";" +
				"background-size: " + px( LOGO_W * LS ) + " " + px( LOGO_H * LS ) + ";" +
				"background-position: " + px( -l0 * LS ) + " " + px( -( vy0 - ty ) * PX ) + ";" +
				"opacity: " + a.toFixed( 4 ) + ";" } );
		}
	}

	// HORIZONTAL_HEXAGONAL tiling, as the page's two masked layers draw it:
	// even bands at background-position 0 0, odd bands at
	// (tile_w / 2, tile_h) taken as CSS percentages, i.e. of (frame - tile)
	function buildPattern( frame )
	{
		var oddX = ( TILE_W / FW / 2 ) * ( FW - TILE_W );
		var oddY = ( TILE_H / FH ) * ( FH - TILE_H );
		for ( var b = 0; b * TILE_H < FH; b++ )
		{
			var by0 = b * TILE_H, by1 = Math.min( FH, by0 + TILE_H );
			var ox = b % 2 ? oddX : 0, oy = b % 2 ? oddY : 0;
			var kFirst = Math.floor( ( by0 - oy ) / TILE_H ), kLast = Math.floor( ( by1 - oy ) / TILE_H );
			for ( var k = kFirst; k <= kLast; k++ )
			{
				var ty = oy + k * TILE_H;
				for ( var tx = ox - TILE_W * Math.ceil( ox / TILE_W ); tx < FW; tx += TILE_W )
					addTile( frame, tx, ty, Math.max( by0, ty ), Math.min( by1, ty + TILE_H ) );
			}
		}
	}

	var frame = $( "#BgFrame" );
	buildBase( frame );
	buildPattern( frame );
	joinConnect.enabled = false;

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down )
			return;
		var key = e.key;
		if ( transitioning )
			return;
		if ( joinOpen )
		{
			if ( key === "Escape" )
				closeJoin();
			return;
		}
		if ( key === "ArrowUp" || key === "w" || key === "W" )
			select( selected - 1 );
		else if ( key === "ArrowDown" || key === "s" || key === "S" )
			select( selected + 1 );
		else if ( key === "Enter" || key === " " )
			activate();
		else if ( key === "WheelDown" )
			select( selected + 1 );
		else if ( key === "WheelUp" )
			select( selected - 1 );
	} );

	select( 0, false );
	$.Schedule( 0, function ()
	{
		setTitleHidden( false );
		afterTransition( function () { transitioning = false; } );
	} );

	return {
		Hover: function ( i ) { if ( !joinOpen && !transitioning ) select( i ); },
		Click: function ( i )
		{
			dropFocus();
			if ( transitioning || joinOpen )
				return;
			select( i, false );
			activate();
		},
		CloseJoin: function () { closeJoin(); },
		CreateLobby: createLobby,
		Refresh: function ()
		{
			dropFocus();
			if ( !transitioning && joinOpen )
				refreshLan();
		},
		JoinSelected: function ()
		{
			dropFocus();
			if ( selectedAddr )
				joinAddr( selectedAddr );
		},
		JoinAddress: function () { dropFocus(); joinAddr( stripped( "#JoinAddr" ) ); },
		JoinCode: function () { dropFocus(); joinCode(); },
		JoinCodeSubmit: function () { if ( !transitioning && joinOpen ) joinCode(); }
	};
})();
