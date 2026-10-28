// Pause menu (port of data/web/pause-menu/app.js). Every item is an
// allowlisted cbuf: command; Leave Game sends three, in order.
var PauseMenu = (function ()
{
	var COMMANDS = [
		[ "webcore_closemenu" ],
		[ "menu_hostmenu" ],
		[ "menu_changeteam" ],
		[ "menu_callvote" ],
		[ "menu_options" ],
		[ "lobby_close", "disconnect", "webcore_closemenu" ]
	];
	var stage = $( "#Stage" );
	var selected = 0;

	function cbuf( cmd )
	{
		if ( !cmd || typeof fte_query !== "function" )
			return false;
		try { return fte_query( "cbuf:" + cmd ) === "ok"; } catch ( e ) { return false; }
	}

	function select( index )
	{
		if ( index < 0 ) index = 0;
		if ( index >= COMMANDS.length ) index = COMMANDS.length - 1;
		selected = index;
		for ( var i = 1; i < COMMANDS.length; i++ )
			stage.SetHasClass( "sel-" + i, i === selected );
	}

	function activate()
	{
		COMMANDS[ selected ].forEach( function ( cmd ) { cbuf( cmd ); } );
	}

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down )
			return;
		var key = e.key;
		if ( key === "ArrowUp" || key === "w" || key === "W" || key === "WheelUp" )
			select( selected - 1 );
		else if ( key === "ArrowDown" || key === "s" || key === "S" || key === "WheelDown" )
			select( selected + 1 );
		else if ( key === "Enter" || key === " " )
			activate();
		else if ( key === "Escape" )
			cbuf( "webcore_closemenu" );
	} );

	select( 0 );

	return {
		Select: select,
		Click: function ( i )
		{
			// a clicked button keeps focus and Panorama would activate it on
			// Enter too; keys go through the handler above only
			$.GetContextPanel().SetFocus();
			select( i );
			activate();
		}
	};
})();
