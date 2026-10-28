// hud-smoke.html: show the engine's getstats reply every 250 ms.
( function ()
{
	function refresh()
	{
		try
		{
			if ( typeof fte_query === "function" )
			{
				var s = fte_query( "getstats" );
				if ( s )
					$( "#Chip" ).text = "HUD · " + s;
			}
		}
		catch ( e ) {}
		$.Schedule( 0.25, refresh );
	}
	refresh();
} )();
