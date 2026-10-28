// Panorama port of basemod-cef-test/menu.js: up/down (or W/S) and the mouse
// pick a row, Enter/Space/click runs its action. The actions call
// window.engine.*, which isn't there, so like the page they only log.
( function ()
{
	var ITEMS = [
		{ action: "map_browser", desc: "Browse and load available maps." },
		{ action: "start_server", desc: "Host a new multiplayer server." },
		{ action: "settings", desc: "Configure video, audio, and controls." },
		{ action: "legacy_menu", desc: "Open the classic Quake menu system." },
		{ action: "quit", desc: "Exit the game." }
	];
	var index = 0;

	function select( i )
	{
		index = ( i + ITEMS.length ) % ITEMS.length;
		for ( var n = 0; n < ITEMS.length; n++ )
			$( "#Item" + n ).SetHasClass( "selected", n === index );
		$( "#DescText" ).text = ITEMS[ index ].desc;
	}

	function activate()
	{
		var action = ITEMS[ index ].action;
		var engine = typeof window === "object" ? window.engine : undefined;
		var hooks = { map_browser: "openMapBrowser", start_server: "startServer", settings: "openSettings",
			legacy_menu: "openLegacyMenu", quit: "quit" };
		if ( engine && hooks[ action ] && typeof engine[ hooks[ action ] ] === "function" )
			engine[ hooks[ action ] ]();
		$.Msg( "[menu] action => " + action );
	}

	ITEMS.forEach( function ( item, i )
	{
		var row = $( "#Item" + i );
		row.SetPanelEvent( "onmouseover", function () { select( i ); } );
		row.SetPanelEvent( "onactivate", function () { activate(); $.GetContextPanel().SetFocus(); } );
	} );

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down )
			return;
		switch ( e.key )
		{
			case "ArrowUp": case "w": select( index - 1 ); break;
			case "ArrowDown": case "s": select( index + 1 ); break;
			case "Enter": case " ": activate(); break;
		}
	} );

	select( 0 );
} )();
