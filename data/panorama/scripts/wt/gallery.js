// WolfTeam theme gallery: the ART switch (original sprites and fonts from
// the private pack, class WolfTeamArt) and the open combo box / focused
// entry shown on the page.
var WtGallery = ( function ()
{
	function ToggleArt()
	{
		var root = $( '#WtGallery' );
		var on = !root.BHasClass( 'WolfTeamArt' );
		root.SetHasClass( 'WolfTeamArt', on );
		var l = $( '#ArtLabel' );
		if ( l )
			l.text = on ? 'ART: on' : 'ART: off';
	}

	function SetArt( on )
	{
		var root = $( '#WtGallery' );
		if ( root.BHasClass( 'WolfTeamArt' ) !== on )
			ToggleArt();
	}

	return { ToggleArt: ToggleArt, SetArt: SetArt };
} )();

( function ()
{
	var focused = $( '#Focused' );
	if ( focused )
		focused.SetFocus();
	// Show one combo box open, so the popup rows are in the picture.
	var combo = $( '#OpenCombo' );
	if ( combo )
		combo.AddClass( 'DropDownMenuVisible' );
} )();
