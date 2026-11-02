// Murrine theme gallery: theme switching and the focused text entry.
// A theme is one or more classes: 'Murrine Bluebird' is a Murrine variant
// written by tools/gtkref/gtkrc2panorama.py --name Bluebird.
var current = [ 'Murrine' ];

function SetTheme( name )
{
	var win = $( '#Window' );
	for ( var i = 0; i < current.length; ++i )
		win.SetHasClass( current[i], false );
	current = name.split( ' ' );
	for ( var i = 0; i < current.length; ++i )
		win.SetHasClass( current[i], true );
}

( function ()
{
	var focused = $( '#Focused' );
	if ( focused )
		focused.SetFocus();
} )();
