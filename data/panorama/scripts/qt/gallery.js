// Qt theme gallery: theme switching and the focused text entry.
var kThemes = [ 'QtFusion', 'QtCurve' ];

function SetTheme( name )
{
	var win = $( '#Window' );
	for ( var i = 0; i < kThemes.length; ++i )
		win.SetHasClass( kThemes[i], kThemes[i] === name );
}

( function ()
{
	var focused = $( '#Focused' );
	if ( focused )
		focused.SetFocus();
} )();
