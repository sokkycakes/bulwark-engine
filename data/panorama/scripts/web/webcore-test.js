// Panorama port of webcore-test: the engine bridge answers synchronously.
var WebcoreTest = (function ()
{
	function Query()
	{
		var out = $( "#Result" );
		if ( typeof fte_query !== "function" )
		{
			out.text = "FTE bridge unavailable";
			return;
		}
		out.text = fte_query( "getstats" );
	}
	return { Query: Query };
})();
