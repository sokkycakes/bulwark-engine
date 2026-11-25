// Title menu v2 (port of data/web/title-menu-v2/app.js). Same requests as the
// page: cbuf:, localsound:, hostcache_refresh / gethostcache, lobby_join:.
// Keys come as Bulwark "key" events, mouse through onactivate / onmouseover.
var TitleMenu = (function ()
{
	var COUNT = 6;
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
		{
			$( "#JoinAddr" ).SetFocus();
			dialogFocus = lanRows.length + 2; // Refresh, the rows, Join Selected, then the field
		}
	}

	// the dialog's controls in screen order, for the d-pad / arrow keys
	// (a controller has no Tab); Enter (A) then activates the focused one
	var dialogFocus = -1;
	function dialogControls()
	{
		var out = [];
		if ( joinDialog.BHasClass( "join-create" ) )
			out = [ $( "#CreateOnline" ), $( "#CreateLan" ) ];
		else
		{
			out.push( $( "#JoinRefresh" ) );
			for ( var i = 0; i < lanRows.length; i++ )
				out.push( lanRows[ i ].panel );
			out.push( joinConnect, $( "#JoinAddr" ), $( "#JoinAddrGo" ), $( "#JoinCode" ), $( "#JoinCodeGo" ) );
		}
		out.push( $( "#JoinBack" ) );
		return out;
	}

	function moveDialogFocus( dir )
	{
		var all = dialogControls();
		var n = all.length;
		for ( var step = 0; step < n; step++ )
		{
			dialogFocus = dialogFocus < 0 ? ( dir > 0 ? 0 : n - 1 ) : ( dialogFocus + dir + n ) % n;
			var p = all[ dialogFocus ];
			if ( p && p.enabled !== false )
			{
				p.SetFocus();
				localsound( SND_NAV );
				return;
			}
		}
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
		dialogFocus = -1;
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
			leaveTitle( "menu_webcore online" );
			return;
		}
		if ( selected === 1 )
		{
			openJoin( true );
			return;
		}
		if ( selected === 2 )
		{
			localsound( SND_OK );
			openJoin( false );
			return;
		}
		if ( selected === 3 )
		{
			leaveTitle( "menu_webcore servers" );
			return;
		}
		leaveTitle( selected === 4 ? "menu_options" : "quit" );
	}

	function createLobby( online )
	{
		dropFocus();
		if ( cbuf( online ? "lobby_create_online" : "lobby_create_lan" ) )
			cbuf( "menu_webcore_lobby" );
	}

	// the backdrop: the page masks a #333 frame and its tiled "stiletto"
	// wordmark (assets/logotile.svg, the same file it inlines) with a radial
	// alpha mask (viewport-mask.svg). Panorama has no masks, so the frame is
	// rebuilt from rows whose gradients sample the mask, and the tiles are
	// drawn in slices whose opacity samples it.
	var FW = 1365, FH = 768;
	var TILE_W = 428.224, TILE_H = 70.272;
	var TILE_SLICES = 6;

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

	function viewSize()
	{
		var w = 0, h = 0;
		if ( typeof Bulwark.Width === "function" )
		{
			w = Bulwark.Width();
			h = Bulwark.Height();
		}
		return w > 0 && h > 0 ? [ w, h ] : [ 1920, 1080 ];
	}

	// the page's two masked layers, in view pixels: tiles tile_w x tile_h of
	// the stretched frame with the SVG kept to its aspect inside each
	// (preserveAspectRatio meet, centred); even bands at background-position
	// 0 0, odd bands at (tile_w / 2, tile_h) taken as CSS percentages, i.e.
	// of (frame - tile)
	function buildPattern( layer, vw, vh )
	{
		var fw = vw * FW / 1024, fh = vh;
		var tw = fw * TILE_W / FW, th = fh * TILE_H / FH;
		var sc = Math.min( tw / 429, th / 71 );
		var mw = 429 * sc, mh = 71 * sc;
		var mx = ( tw - mw ) / 2, my = ( th - mh ) / 2;
		var oddX = ( TILE_W / FW / 2 ) * ( fw - tw ), oddY = ( TILE_H / FH ) * ( fh - th );
		for ( var b = 0; b * th < fh; b++ )
		{
			var by0 = b * th, by1 = Math.min( fh, by0 + th );
			var ox = b % 2 ? oddX : 0, oy = b % 2 ? oddY : 0;
			for ( var k = Math.floor( ( by0 - oy ) / th ); k <= Math.floor( ( by1 - oy ) / th ); k++ )
			{
				var ty = oy + k * th;
				var y0 = Math.max( by0, ty + my ), y1 = Math.min( by1, ty + my + mh );
				if ( y1 - y0 < 0.5 )
					continue;
				for ( var tx = ox - tw * Math.ceil( ox / tw ); tx < fw; tx += tw )
				{
					for ( var s = 0; s < TILE_SLICES; s++ )
					{
						var x0 = Math.max( 0, tx + mx + mw * s / TILE_SLICES );
						var x1 = Math.min( fw, tx + mx + mw * ( s + 1 ) / TILE_SLICES );
						if ( x1 - x0 < 0.5 )
							continue;
						var a = 0.11 * maskAt( ( x0 + x1 ) / 2 * FW / fw, ( y0 + y1 ) / 2 * FH / fh );
						if ( a < 0.004 )
							continue;
						$.CreatePanel( "Panel", layer, "", { class: "bg-slice", style:
							"x: " + px( x0 ) + "; y: " + px( y0 ) + "; width: " + px( x1 - x0 ) + "; height: " + px( y1 - y0 ) + ";" +
							"background-size: " + px( mw ) + " " + px( mh ) + ";" +
							"background-position: " + px( tx + mx - x0 ) + " " + px( ty + my - y0 ) + ";" +
							"opacity: " + a.toFixed( 4 ) + ";" } );
					}
				}
			}
		}
	}

	var frame = $( "#BgFrame" );
	buildBase( frame );
	var patternLayer = $.CreatePanel( "Panel", frame, "BgPattern", { class: "bg-pattern" } );
	var builtSize = "";

	// the tiles depend on the view's aspect: rebuild when it changes
	function watchSize()
	{
		var size = viewSize();
		var key = size[ 0 ] + "x" + size[ 1 ];
		if ( key !== builtSize )
		{
			builtSize = key;
			patternLayer.RemoveAndDeleteChildren();
			buildPattern( patternLayer, size[ 0 ], size[ 1 ] );
		}
		$.Schedule( 0.5, watchSize );
	}
	watchSize();
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
			else if ( key === "ArrowUp" || key === "ArrowDown" )
				moveDialogFocus( key === "ArrowDown" ? 1 : -1 );
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
