// Panorama port of Stiletto's data/web/scratch.html: builds the parts the page
// writes out by hand (equip slots, backpack rows, the button legend and the
// range graph) and keeps the viewport fitted to the view, as the page's
// updateScale() does. The page is a static mock-up: nothing is interactive.

var Scratch = ( function ()
{
	var VIEWPORT_W = 1849.1, VIEWPORT_H = 1080;

	// The page's inline 24x24 icons. Their shapes say fill="currentColor",
	// which beats the stylesheet's cyan/red svg fill, so they draw in the
	// text colour; only the drop-shadow glow is cyan or red.
	var INK = "#d8d2a8", GLOW_CYAN = "#4a9ab0", GLOW_RED = "#a04a3a";

	function svgUrl( svg )
	{
		return "url(\"data:image/svg+xml," + encodeURIComponent( svg ) + "\")";
	}

	function icon( body, fill )
	{
		return svgUrl( "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>" + body.replace( /currentColor/g, fill ) + "</svg>" );
	}

	// drop-shadow( 0 0 3px glow ) as three soft strokes under the shapes, in a
	// view box 4 units bigger each side (the icon panel grows to match)
	function glowIcon( body, glow )
	{
		var halo = body.replace( /currentColor|#000/g, glow ), layers = "";
		[ [ 10, 0.08 ], [ 6, 0.12 ], [ 3, 0.2 ] ].forEach( function ( l )
		{
			layers += "<g stroke='" + glow + "' stroke-width='" + l[ 0 ] + "' stroke-linejoin='round' opacity='" + l[ 1 ] + "'>" + halo + "</g>";
		} );
		return svgUrl( "<svg xmlns='http://www.w3.org/2000/svg' viewBox='-4 -4 32 32'>" + layers + body.replace( /currentColor/g, INK ) + "</svg>" );
	}

	var GRENADE = "<ellipse cx='12' cy='15' rx='6' ry='7' fill='currentColor'/><rect x='10' y='4' width='4' height='4' fill='currentColor'/>";
	var CANISTER = "<rect x='8' y='6' width='8' height='14' fill='currentColor'/><rect x='9' y='3' width='6' height='3' fill='currentColor'/>";

	var SLOTS = [
		{ weight: "3.2kg", icon: "<path d='M3 14 L14 14 L14 11 L18 11 L18 9 L21 9 L21 14 L20 16 L14 16 L14 18 L11 18 L11 16 L7 16 L7 18 L3 18 Z' fill='currentColor'/>" },
		{ weight: "2.4kg", icon: "<path d='M2 13 L16 13 L16 11 L20 11 L20 15 L16 15 L16 17 L4 17 Z' fill='currentColor'/>" },
		{ weight: "1.8kg", icon: "<rect x='4' y='10' width='16' height='6' fill='currentColor'/><rect x='8' y='7' width='4' height='3' fill='currentColor'/>" },
		{ label: "M-CUSTOM", selected: true },
		{ weight: "1.5kg", red: true, icon: "<circle cx='12' cy='13' r='6' fill='currentColor'/><rect x='11' y='5' width='2' height='4' fill='currentColor'/>" }
	];

	var ITEMS = [
		{ weight: "0.3kg", name: "GRENADE", count: "10", red: true, icon: GRENADE + "<rect x='14' y='5' width='3' height='2' fill='currentColor'/>" },
		{ weight: "0.8kg", name: "PETRO BOMB", count: "9", red: true, icon: "<path d='M9 4 L15 4 L15 8 L17 12 L17 20 L7 20 L7 12 L9 8 Z' fill='currentColor'/>" },
		{ weight: "0.8kg", name: "WP G.", count: "10", icon: GRENADE },
		{ weight: "0.8kg", name: "STUN G.", count: "10", icon: CANISTER },
		{ weight: "0.8kg", name: "CHAFF G.", count: "2", selected: true,
			icon: CANISTER + "<line x1='10' y1='10' x2='14' y2='10' stroke='#000' stroke-width='1'/><line x1='10' y1='13' x2='14' y2='13' stroke='#000' stroke-width='1'/>" },
		{ weight: "0.8kg", name: "SMOKE G.", count: "5", icon: CANISTER + "<circle cx='12' cy='13' r='2' fill='#000'/>" },
		{ weight: "1.5kg", name: "CLAYMORE", count: "10", red: true,
			icon: "<path d='M4 18 L20 18 L18 10 L6 10 Z' fill='currentColor'/><rect x='6' y='18' width='2' height='3' fill='currentColor'/><rect x='16' y='18' width='2' height='3' fill='currentColor'/>" },
		{ weight: "1.5kg", name: "S.G. MINE", count: "8", icon: "<circle cx='12' cy='14' r='6' fill='currentColor'/><rect x='11' y='5' width='2' height='4' fill='currentColor'/>" },
		{ weight: "0.8kg", name: "C4", count: "9", red: true, icon: "<rect x='4' y='9' width='16' height='7' fill='currentColor'/><rect x='9' y='6' width='6' height='3' fill='currentColor'/>" },
		{ weight: "1.5kg", name: "S.G.SATCHEL", count: "0",
			icon: "<rect x='5' y='8' width='14' height='11' fill='currentColor'/><path d='M7 8 Q12 3 17 8' fill='none' stroke='currentColor' stroke-width='1.5'/>" }
	];

	// Inter has no U+25A6, and its U+25CF is far bigger than the page's
	// fallback glyph, so those two keys draw icons
	var LEGEND = [
		{ label: "RTN", glyph: "\u21bb" },
		{ label: "B", glyph: "B" },
		{ label: "FRT", icon: "<rect x='1' y='1' width='22' height='22' fill='none' stroke='#f4eed0' stroke-width='2'/>" +
			"<path d='M7.5 1 V23 M12 1 V23 M16.5 1 V23 M1 7.5 H23 M1 12 H23 M1 16.5 H23' stroke='#f4eed0' stroke-width='1.6'/>" },
		{ label: "STR", glyph: "\u2197" },
		{ label: "LED", icon: "<circle cx='13' cy='14' r='6' fill='#f4eed0'/>" }
	];

	// the range curve: the page's 100x50 polyline stretched over the graph,
	// with its 1px non-scaling stroke
	var GRAPH_W = 589.6, GRAPH_H = 635.5;
	var CURVE = [ [ 0, 40 ], [ 15, 38 ], [ 30, 30 ], [ 50, 15 ], [ 70, 8 ], [ 85, 5 ], [ 100, 4 ] ];

	function glassRow( parent, cls, y, selected )
	{
		var row = $.CreatePanel( "Panel", parent, "", { "class": "glass-row " + cls + ( selected ? " selected" : "" ), hittest: "false",
			style: "y: " + y.toFixed( 2 ) + "px;" } );
		$.CreatePanel( "Panel", row, "", { "class": "fringe", hittest: "false" } );
		$.CreatePanel( "Panel", row, "", { "class": "row-hi", hittest: "false" } );
		$.CreatePanel( "Panel", row, "", { "class": "row-lo", hittest: "false" } );
		return row;
	}

	function label( parent, cls, text )
	{
		return $.CreatePanel( "Label", parent, "", { "class": "t " + cls, text: text, hittest: "false" } );
	}

	function buildSlots()
	{
		var parent = $( "#EquipSlots" );
		SLOTS.forEach( function ( slot, i )
		{
			var row = glassRow( parent, "slot", i * 130.75, slot.selected );
			if ( slot.icon )
				$.CreatePanel( "Panel", row, "", { "class": "ghost-icon", hittest: "false" } ).style.backgroundImage = icon( slot.icon, INK );
			if ( slot.weight )
				label( row, "ghost-weight", slot.weight );
			if ( slot.label )
				label( row, "slot-label", slot.label );
		} );
	}

	function buildItems()
	{
		var parent = $( "#ItemList" );
		ITEMS.forEach( function ( item, i )
		{
			var row = glassRow( parent, "item", i * 70.32, item.selected );
			$.CreatePanel( "Panel", row, "", { "class": "item-icon", hittest: "false" } ).style.backgroundImage = glowIcon( item.icon, item.red ? GLOW_RED : GLOW_CYAN );
			label( row, "item-weight", item.weight );
			label( row, "item-name", item.name );
			label( row, "item-count", item.count );
		} );
	}

	function buildLegend()
	{
		var parent = $( "#LegendPanel" );
		LEGEND.forEach( function ( entry, i )
		{
			var y = i * 31.1;
			$.CreatePanel( "Label", parent, "", { "class": "t legend-label", text: entry.label, hittest: "false",
				style: "y: " + ( 2.37 + y ).toFixed( 2 ) + "px;" } );
			var key = $.CreatePanel( "Panel", parent, "", { "class": "legend-key", hittest: "false",
				style: "y: " + ( 9.02 + y ).toFixed( 2 ) + "px;" } );
			if ( entry.glyph )
				label( key, "legend-glyph", entry.glyph );
			else
				$.CreatePanel( "Panel", key, "", { "class": "legend-icon", hittest: "false" } ).style.backgroundImage = icon( entry.icon, "#f4eed0" );
		} );
	}

	function buildGraph()
	{
		var points = CURVE.map( function ( p )
		{
			return ( p[ 0 ] * GRAPH_W / 100 ).toFixed( 1 ) + "," + ( p[ 1 ] * GRAPH_H / 50 ).toFixed( 1 );
		} ).join( " " );
		var svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " + GRAPH_W + " " + GRAPH_H + "'>" +
			"<polyline points='" + points + "' fill='none' stroke='#f4eed0' stroke-width='2.68'/></svg>";
		$( "#StatsLine" ).style.backgroundImage = svgUrl( svg );
	}

	// the page scales its viewport to fit; at 1080 lines that only matters
	// when the view is narrower than the viewport
	var builtScale = -1;
	function watchSize()
	{
		var w = Bulwark.Width(), h = Bulwark.Height();
		var k = w > 0 && h > 0 ? Math.min( 1, w / VIEWPORT_W, h / VIEWPORT_H ) : 1;
		if ( Math.abs( k - builtScale ) > 0.0001 )
		{
			builtScale = k;
			$( "#Viewport" ).style.transform = k < 1 ? "scale3d( " + k.toFixed( 4 ) + ", " + k.toFixed( 4 ) + ", 1 )" : "none";
		}
		$.Schedule( 0.5, watchSize );
	}

	buildSlots();
	buildItems();
	buildLegend();
	buildGraph();
	watchSize();

	return {};
} )();
