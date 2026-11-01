// Qt theme gallery: theme switching and the focused text entry.
// A theme is one or more classes: 'QtCurve Sokky' is a QtCurve variant.
var kClasses = [ 'QtFusion', 'QtCurve', 'Sokky', 'SokkyDark' ];

function SetTheme( name )
{
	var win = $( '#Window' );
	var want = name.split( ' ' );
	for ( var i = 0; i < kClasses.length; ++i )
		win.SetHasClass( kClasses[i], want.indexOf( kClasses[i] ) >= 0 );
}

( function ()
{
	var focused = $( '#Focused' );
	if ( focused )
		focused.SetFocus();
} )();
