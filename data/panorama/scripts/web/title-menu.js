// Title menu v1 (port of data/web/title-menu/app.js): Create Lobby / Join
// Lobby. Same requests as the page: cbuf:, hostcache_refresh / gethostcache,
// lobby_join:. No sounds and no transitions, unlike v2.
var TitleMenu = (function ()
{
	var COUNT = 4;
	var stage = $( "#Stage" );
	var joinDialog = $( "#JoinDialog" );
	var joinList = $( "#JoinList" );
	var joinConnect = $( "#JoinConnect" );
	var selected = 0;
	var selectedAddr = null;
	var joinOpen = false;
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

	// a click leaves the button focused, and Panorama activates a focused
	// button on Enter as well; park focus on the root so keys only go
	// through the key handler
	function dropFocus()
	{
		$.GetContextPanel().SetFocus();
	}

	function select( index )
	{
		if ( index < 0 ) index = 0;
		if ( index >= COUNT ) index = COUNT - 1;
		selected = index;
		for ( var i = 1; i < COUNT; i++ )
			stage.SetHasClass( "sel-" + i, i === selected );
	}

	function openJoin( create )
	{
		$( "#JoinTitle" ).text = create ? "Create Lobby" : "Join Lobby";
		joinDialog.SetHasClass( "join-create", !!create );
		joinDialog.SetHasClass( "join-join", !create );
		joinOpen = true;
		joinDialog.SetHasClass( "hidden", false );
		selectedAddr = null;
		joinConnect.enabled = false;
		if ( !create )
		{
			refreshLan();
			$( "#JoinAddr" ).SetFocus();
		}
	}

	function closeJoin()
	{
		joinOpen = false;
		joinDialog.SetHasClass( "hidden", true );
		dropFocus();
	}

	function selectLan( index )
	{
		var row = lanRows[ index ];
		if ( !row )
			return;
		selectedAddr = row.addr;
		for ( var i = 0; i < lanRows.length; i++ )
			lanRows[ i ].panel.SetHasClass( "is-selected", i === index );
		joinConnect.enabled = true;
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
		// prefer lobby-flagged rows; still show others if none are flagged
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
		if ( !addr )
			return;
		if ( q( "lobby_join:" + addr ) === "ok" )
		{
			closeJoin();
			cbuf( "menu_webcore_lobby" );
		}
	}

	function stripped( id )
	{
		var text = $( id ).text;
		return text ? String( text ).replace( /\s+/g, "" ) : "";
	}

	function activate()
	{
		if ( selected === 0 )
			openJoin( true );
		else if ( selected === 1 )
			openJoin( false );
		else
			cbuf( selected === 2 ? "menu_options" : "quit" );
	}

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down )
			return;
		var key = e.key;
		if ( joinOpen )
		{
			if ( key === "Escape" )
				closeJoin();
			return;
		}
		if ( key === "ArrowUp" || key === "w" || key === "W" || key === "WheelUp" )
			select( selected - 1 );
		else if ( key === "ArrowDown" || key === "s" || key === "S" || key === "WheelDown" )
			select( selected + 1 );
		else if ( key === "Enter" || key === " " )
			activate();
	} );

	joinConnect.enabled = false;
	select( 0 );

	return {
		Hover: function ( i ) { if ( !joinOpen ) select( i ); },
		Click: function ( i )
		{
			dropFocus();
			select( i );
			activate();
		},
		CloseJoin: closeJoin,
		CreateLobby: function ( online )
		{
			dropFocus();
			if ( cbuf( online ? "lobby_create_online" : "lobby_create_lan" ) )
				cbuf( "menu_webcore_lobby" );
		},
		Refresh: function () { dropFocus(); refreshLan(); },
		JoinSelected: function ()
		{
			dropFocus();
			if ( selectedAddr )
				joinAddr( selectedAddr );
		},
		JoinAddress: function () { dropFocus(); joinAddr( stripped( "#JoinAddr" ) ); },
		JoinCode: function ()
		{
			var code = stripped( "#JoinCode" );
			if ( code )
				joinAddr( code );
		}
	};
})();
