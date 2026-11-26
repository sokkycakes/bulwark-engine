// Panorama port of Stiletto's data/web/ql-menu (app.js): the Quake Live style
// in-game menu. With ?mode=arena it is the duel/arena selector (polls getarena,
// sends arena_action:<...>); without it, the warmup and Esc menu demo views.
//
// The original paints a 640x480 canvas every animation frame. Here each draw
// call of that code becomes an op (image, text, rect or hit area) and a frame's
// ops are reconciled against the panels already on screen: a panel whose op is
// unchanged is kept, so only what changed is rebuilt. Hit areas sit in their
// own layer above the art, the way the canvas kept a separate hit list.
var QlMenu = (function ()
{
	var W = 640, H = 480;
	var FOCUS = "rgb(255, 191, 0)";
	var ART = "file://{game}/data/web/ql-menu/assets/";

	var A = {
		logo: "backscreenlogo.png",
		btnBack: "button_back.png",
		frametl: "score/frametl.png", framet: "score/framet.png", frametr: "score/frametr.png",
		framel: "score/framel.png", framem: "score/framem.png", framer: "score/framer.png",
		framebl: "score/framebl.png", frameb: "score/frameb.png", framebr: "score/framebr.png",
		scoretl: "score/scoretl3.png", scoretm: "score/scoretm3.png", scoretr: "score/scoretr3.png",
		scorel: "score/scorel.png", scorem: "score/scorem.png", scorer: "score/scorer.png",
		scorebl: "score/scorebl.png", scoreb: "score/scoreb.png", scorebr: "score/scorebr.png",
		btn: "score/btn.png", btno: "score/btno.png", arrow: "score/arrow.png",
		navbarl: "score/navbarl.png", navbarm: "score/navbarm.png", navbarr: "score/navbarr.png",
		navleft: "score/navleft.png", navlefty: "score/navlefty.png",
		navright: "score/navright.png", navrighty: "score/navrighty.png", navrightb: "score/navrightb.png",
		bigred: "score/bigred.png",
		boxtl: "menu/boxtl.png", boxtr: "menu/boxtr.png", boxbl: "menu/boxbl.png", boxbr: "menu/boxbr.png",
		fade: "menu/fade.png",
		logo2: "score/logo2.png", gtbox: "score/gtbox.png",
		bgfill: "score/bgfill2.png", bgfillRed: "score/bgfill2_red.png",
		bgfillO1: "score/bgfill2o1.jpg", bgfillO2: "score/bgfill2o2.jpg",
		bgfillO6: "score/bgfill2o6.jpg", bgfillO7: "score/bgfill2o7.jpg",
		adtl: "score/adtl.png", adtm: "score/adtm.png", adtr: "score/adtr.png", adbr: "score/adbr.png",
		ad2x1: "ad2x1.jpg",
		scoretlA: "score/scoretl.png", scoretl2: "score/scoretl2.png", scoretmA: "score/scoretm.png",
		scoretr2: "score/scoretr2.png",
		specl: "score/specl.png", specm: "score/specm.png", specflip: "score/specflip.png"
	};
	for ( var name in A )
		A[name] = ART + A[name];

	var TABS = [
		{ id: "about", label: "CURRENT MATCH", x: 72 },
		{ id: "controls", label: "CONTROLS", x: 167 },
		{ id: "options", label: "GAME SETTINGS", x: 262 },
		{ id: "vote", label: "CALL VOTE", x: 357 }
	];

	var PLAYERS = [
		{ name: "You", ready: false, team: "spec" },
		{ name: "Bob", ready: true, team: "red" },
		{ name: "Alice", ready: true, team: "blue" },
		{ name: "Kane", ready: false, team: "red" },
		{ name: "Nova", ready: true, team: "blue" },
		{ name: "Rex", ready: false, team: "spec" }
	];

	var state = {
		view: "warmup", // "warmup" | "esc"
		open: false,
		tab: "about",
		popup: null, // null | "join" | "leave" | "quitConfirm" | "restartConfirm"
		hover: null,
		teamMode: false,
		status: "Warmup view · Esc menu · 1/2 switch views",
		flashUntil: 0,
		myReady: false,
		myTeam: "spec"
	};

	// Without the bridge (never the case in Bulwark) the original ran its
	// browser demo; every view here is the engine one.
	var demoMode = typeof fte_query !== "function";
	var search = ( typeof location === "object" && location && location.search ) || "";
	var arenaMode = /[?&]mode=arena(&|$)/.test( search );
	var arenaSnapshot = null;
	var arenaLastPoll = 0;

	function now() { return Date.now(); }

	function cbuf( cmd )
	{
		if ( demoMode )
			return false;
		try { return fte_query( "cbuf:" + cmd ) === "ok"; }
		catch ( e ) { return false; }
	}

	function toast( msg )
	{
		state.status = msg;
		state.flashUntil = now() + 1650; // the canvas faded its flash over ~100 frames
	}

	function arenaAction( action )
	{
		if ( !arenaMode )
			return false;
		try
		{
			var reply = fte_query( "arena_action:" + action );
			var result = JSON.parse( reply || "" );
			if ( result && result.ok === true )
			{
				// the engine closes on spectate; this mirrors the original's extra close
				if ( action === "spectate" )
				{
					try { fte_query( "arena_action:close" ); } catch ( e ) {}
				}
				toast( action.replace( "hero:", "SELECT " ) );
				return true;
			}
		}
		catch ( e ) {} // the bridge may go away while the menu closes
		toast( "Action unavailable" );
		return false;
	}

	function syncArena( t )
	{
		if ( !arenaMode || t < arenaLastPoll )
			return;
		arenaLastPoll = t + 120;
		try
		{
			var raw = fte_query( "getarena" );
			var snapshot = raw ? JSON.parse( raw ) : null;
			arenaSnapshot = snapshot && typeof snapshot === "object" && !Array.isArray( snapshot ) ? snapshot : null;
		}
		catch ( e )
		{
			arenaSnapshot = null;
		}
	}

	// --- the 640x480 stage, letterboxed into the view ---------------------------

	var K = 2.25; // design pixels per virtual pixel
	var root = null, stage = null, artLayer = null, hitLayer = null, cursor = null;

	// the stage keeps its aspect and is centred in the view (layout pixels)
	var viewW = 1920, viewH = 1080;
	function computeScale()
	{
		viewW = Bulwark.Width() || viewW;
		viewH = Bulwark.Height() || viewH;
		return Math.min( viewW / W, viewH / H );
	}

	function px( v ) { return ( Math.round( v * K * 100 ) / 100 ) + "px"; }
	function box( x, y, w, h )
	{
		return "position:" + px( x ) + " " + px( y ) + " 0px;width:" + px( Math.max( 0, w ) ) + ";height:" + px( Math.max( 0, h ) ) + ";";
	}

	// --- ops ------------------------------------------------------------------------

	var ops = [];
	var hits = [];
	var hitById = {};

	function drawImage( img, x, y, w, h, alpha, extra )
	{
		var a = alpha === undefined ? 1 : alpha;
		var style = box( x, y, w, h ) + "background-image:url(\"" + img + "\");" + ( a < 1 ? "opacity:" + a + ";" : "" ) + ( extra || "" );
		ops.push( { type: "Panel", cls: "img", style: style } );
	}

	function fillRect( x, y, w, h, color )
	{
		ops.push( { type: "Panel", cls: "rect", style: box( x, y, w, h ) + "background-color:" + color + ";" } );
	}

	function strokeRect( x, y, w, h, color )
	{
		ops.push( { type: "Panel", cls: "rect", style: box( x - 0.5, y - 0.5, w + 1, h + 1 ) + "border:" + px( 1 ) + " solid " + color + ";" } );
	}

	function normalizeGlyphs( str )
	{
		return String( str )
			.replace( /[●]/g, "*" )
			.replace( /[○]/g, "o" )
			.replace( /[»]/g, ">" )
			.replace( /[«]/g, "<" )
			.replace( /[✓]/g, "v" )
			.replace( /[·]/g, "-" )
			.replace( /[—–]/g, "-" )
			.replace( /[…]/g, "..." )
			.replace( /[“”]/g, "\"" )
			.replace( /[‘’]/g, "'" );
	}

	// the canvas mapped Q3 textscale to px through the old bitmap glyph scale
	var QL_GLYPH_SCALE = { 16: 3, 24: 2, 48: 1 };
	function resolveTextPx( o )
	{
		var size = o.size === undefined ? 12 : o.size;
		var fontSize = o.fontSize === undefined ? 24 : o.fontSize;
		if ( o.scale !== undefined && o.scale !== null )
			return Math.max( 8, Math.round( fontSize * o.scale * ( QL_GLYPH_SCALE[fontSize] || 2 ) ) );
		return Math.max( 8, Math.round( size ) );
	}

	// Font ascents (hhea, per em): a label puts its baseline one ascent below
	// its top, the canvas drew at the baseline.
	var ASCENT = { standard_07_57: 1.125, hooge_05_57: 0.625 };

	// text(str, x, y, {font, color, align, baseline, alpha, scale, fontSize, size, cls})
	function text( str, x, y, o )
	{
		o = o || {};
		var font = o.font || "standard_07_57";
		var size = resolveTextPx( o );
		var asc = ASCENT[font] || 1;
		var top;
		if ( o.baseline === "top" )
			top = y;
		else if ( o.baseline === "middle" )
			top = y - asc * size + 0.2 * size;
		else
			top = y - asc * size;
		// a label box that stays inside the stage, aligned the way the canvas was
		var align = o.align || "left";
		var span, left;
		if ( align === "center" )
		{
			span = 2 * Math.max( 1, Math.min( x, W - x ) );
			left = x - span / 2;
		}
		else if ( align === "right" )
		{
			span = Math.max( 1, x );
			left = 0;
		}
		else
		{
			span = Math.max( 1, W - x );
			left = x;
		}
		var style = "position:" + px( left ) + " " + px( top ) + " 0px;width:" + px( span ) + ";" +
			"font-size:" + px( size ) + ";text-align:" + align + ";";
		if ( o.color !== null )
			style += "color:" + ( o.color || "#fff" ) + ";";
		if ( o.alpha !== undefined && o.alpha < 1 )
			style += "opacity:" + o.alpha + ";";
		ops.push( { type: "Label", cls: "t" + ( font === "hooge_05_57" ? " hooge" : "" ) + ( o.cls ? " " + o.cls : "" ),
			style: style, text: normalizeGlyphs( str ) } );
	}

	// a clickable area; returns whether it is the hovered one, as the canvas did
	function hit( id, x, y, w, h, onClick )
	{
		hits.push( { id: id, x: x, y: y, w: w, h: h } );
		hitById[id] = onClick;
		return state.hover === id;
	}

	// --- reconciling ops with panels -------------------------------------------------

	function Layer( parent ) { this.parent = parent; this.items = []; }

	// list: [{key, make()}]; panels with the same key are kept
	Layer.prototype.commit = function ( list )
	{
		var pool = {}, i, item;
		for ( i = 0; i < this.items.length; i++ )
		{
			item = this.items[i];
			( pool[item.key] = pool[item.key] || [] ).push( item.panel );
		}
		var next = [];
		for ( i = 0; i < list.length; i++ )
		{
			var bucket = pool[list[i].key];
			var panel = bucket && bucket.length ? bucket.shift() : null;
			next.push( { key: list[i].key, panel: panel, make: list[i].make } );
		}
		for ( var k in pool )
			for ( i = 0; i < pool[k].length; i++ )
				pool[k][i].DeleteAsync( 0 );
		for ( i = 0; i < next.length; i++ )
		{
			if ( !next[i].panel )
				next[i].panel = next[i].make( this.parent );
			var at = this.parent.GetChild( i );
			if ( at !== next[i].panel )
				this.parent.MoveChildBefore( next[i].panel, at );
		}
		this.items = next.map( function ( n ) { return { key: n.key, panel: n.panel }; } );
	};

	function onHitOver( id ) { state.hover = id; }
	function onHitOut( id ) { if ( state.hover === id ) state.hover = null; }
	function onHitClick( id )
	{
		if ( id === "__outside" )
		{
			// out-of-bounds click closes popups (QL outOfBoundsClick)
			if ( state.popup )
			{
				state.popup = null;
				toast( "Popup closed" );
			}
			return;
		}
		var fn = hitById[id];
		if ( fn )
			fn();
	}

	function hitList()
	{
		var list = [ { id: "__outside", x: 0, y: 0, w: W, h: H } ].concat( hits );
		return list.map( function ( h )
		{
			return {
				key: h.id + "|" + K + "|" + h.x + "," + h.y + "," + h.w + "," + h.h,
				make: function ( parent )
				{
					var p = $.CreatePanel( "Panel", parent, "", { "class": "hit", style: box( h.x, h.y, h.w, h.h ), hittest: "true" } );
					p.SetPanelEvent( "onactivate", function () { onHitClick( h.id ); } );
					if ( h.id !== "__outside" )
					{
						p.SetPanelEvent( "onmouseover", function () { onHitOver( h.id ); } );
						p.SetPanelEvent( "onmouseout", function () { onHitOut( h.id ); } );
					}
					return p;
				}
			};
		} );
	}

	// --- shared chrome ----------------------------------------------------------------

	function scoreInset( x, y, w, h )
	{
		var tl = 20, side = 10, bot = 10;
		drawImage( A.scoretl, x, y, tl, tl );
		drawImage( A.scoretm, x + tl, y, w - tl * 2, tl );
		drawImage( A.scoretr, x + w - tl, y, tl, tl );
		drawImage( A.scorel, x, y + tl, side, h - tl - bot );
		drawImage( A.scorem, x + side, y + tl, w - side * 2, h - tl - bot );
		drawImage( A.scorer, x + w - side, y + tl, side, h - tl - bot );
		drawImage( A.scorebl, x, y + h - bot, side, bot );
		drawImage( A.scoreb, x + side, y + h - bot, w - side * 2, bot );
		drawImage( A.scorebr, x + w - side, y + h - bot, side, bot );
	}

	// --- Esc menu view (ingame*.menu) ---------------------------------------------------

	function drawClosedHint()
	{
		text( "Press ESC for GAME MENU", 320, 450, { scale: 0.22, fontSize: 16, align: "center", color: "#fff", cls: "pulse-hint" } );
	}

	function drawShell()
	{
		fillRect( 0, 0, W, H, "rgba(0,0,0,0.25)" );
		drawImage( A.logo, 200, 18, 240, 60 );
		drawImage( A.frametl, 62, 90, 40, 40, 0.9 );
		drawImage( A.framet, 102, 90, 436, 40, 0.9 );
		drawImage( A.frametr, 538, 90, 40, 40, 0.9 );
		drawImage( A.arrow, 69, 95, 10, 10 );
		text( "GAME MENU", 82, 104, { scale: 0.2, fontSize: 16, color: "#ccc" } );

		TABS.forEach( function ( tab )
		{
			var hovered = hit( "tab:" + tab.id, tab.x, 108, 94, 25, function ()
			{
				state.tab = tab.id;
				state.popup = null;
				if ( !demoMode )
				{
					if ( tab.id === "controls" || tab.id === "options" ) { cbuf( "menu_options" ); return; }
					if ( tab.id === "vote" ) { cbuf( "menu_callvote" ); return; }
				}
				toast( "Opened " + tab.label );
			} );
			var active = state.tab === tab.id;
			drawImage( A.btn, tab.x, 108, 94, 25, 0.25 );
			if ( active )
				drawImage( A.btno, tab.x, 108, 94, 25, 1 );
			else if ( hovered )
				drawImage( A.btno, tab.x, 108, 94, 25, 0.35 );
			text( tab.label, tab.x + 7 + 40, 108 + 14, { scale: 0.18, fontSize: 16, align: "center", color: hovered && !active ? FOCUS : "#fff" } );
		} );
	}

	function drawBottomNav()
	{
		drawImage( A.navbarl, 33, 382, 20, 40, 1 );
		drawImage( A.navbarm, 53, 382, 532, 40, 1 );
		drawImage( A.navbarr, 585, 382, 20, 40, 1 );

		// ad placeholder
		fillRect( 218, 373, 204, 54, "rgba(0,0,0,0.75)" );
		strokeRect( 220.5, 375.5, 199, 49, "rgba(255,255,255,0.12)" );
		text( "AD 4x1", 320, 400, { scale: 0.2, fontSize: 16, align: "center", color: "#666" } );

		var leaveHot = hit( "leave", 38, 391, 80, 20, function ()
		{
			state.popup = "leave";
			toast( "Leave Match popup" );
		} );
		drawImage( leaveHot ? A.navlefty : A.navleft, 38, 391, 80, 20 );
		text( "LEAVE MATCH", 62, 404, { scale: 0.2, fontSize: 16, color: leaveHot ? FOCUS : "#fff" } );

		var retHot = hit( "return", 520, 391, 80, 20, function ()
		{
			state.open = false;
			state.popup = null;
			if ( demoMode ) { toast( "Returned to match" ); return; }
			cbuf( "webcore_closemenu" );
		} );
		drawImage( retHot ? A.navrighty : A.navright, 520, 391, 80, 20 );
		text( "RETURN TO MATCH", 574, 404, { scale: 0.2, fontSize: 16, color: retHot ? FOCUS : "#fff", align: "right" } );
	}

	function panelFrame( bodyH )
	{
		var y = 129;
		drawImage( A.framel, 62, y, 40, bodyH, 0.9 );
		drawImage( A.framem, 102, y, 436, bodyH, 0.9 );
		drawImage( A.framer, 538, y, 40, bodyH, 0.9 );
		drawImage( A.framebl, 62, y + bodyH, 40, 20, 0.9 );
		drawImage( A.frameb, 102, y + bodyH, 436, 20, 0.9 );
		drawImage( A.framebr, 538, y + bodyH, 40, 20, 0.9 );
		scoreInset( 71, y, 387, Math.min( 118, bodyH + 10 ) );
		return y;
	}

	function drawAbout()
	{
		var y0 = 129;
		panelFrame( 107 );
		text( "ABOUT THIS MATCH", 78, y0 + 12, { scale: 0.18, fontSize: 16 } );
		text( "Campgrounds  -  Clan Arena", 86, y0 + 34, { scale: 0.2, fontSize: 24 } );
		text( "Timelimit 10  -  Roundlimit 8", 95, y0 + 52, { scale: 0.2, fontSize: 24, color: "#999" } );
		text( "Players 6/16  -  Warmup", 95, y0 + 70, { scale: 0.2, fontSize: 24, color: "#999" } );
		text( "Server: localdemo  -  Rules: stock", 95, y0 + 88, { scale: 0.2, fontSize: 24, color: "#999" } );
		text( "Map: dm_campgrounds", 95, y0 + 106, { scale: 0.2, fontSize: 24, color: "#999" } );

		if ( state.teamMode )
		{
			var rows = [
				{ id: "joinRed", label: "JOIN RED", img: A.navright, by: 6, ty: 20, cmd: "team red" },
				{ id: "joinBlue", label: "JOIN BLUE", img: A.navrightb, by: 36, ty: 50, cmd: "team blue" },
				{ id: "autoJoin", label: "AUTO JOIN", img: A.navrighty, by: 66, ty: 80, cmd: "team free" },
				{ id: "spectate", label: "SPECTATE", img: A.navrighty, by: 96, ty: 110, cmd: "team s" }
			];
			rows.forEach( function ( row )
			{
				var hot = hit( row.id, 485, y0 + row.by, 80, 20, function ()
				{
					if ( demoMode ) { toast( "exec \"" + row.cmd + "\"" ); return; }
					cbuf( row.cmd );
					state.open = false;
					state.popup = null;
				} );
				drawImage( row.img, 485, y0 + row.by, 80, 20 );
				text( row.label, 539, y0 + row.ty, { scale: 0.2, fontSize: 16, align: "right", color: hot ? FOCUS : "#fff" } );
			} );
		}
		else
		{
			var hot = hit( "joinMatch", 463, y0 + 63, 103, 24, function ()
			{
				toast( "exec \"team a\" - JOIN MATCH" );
				state.open = false;
			} );
			drawImage( A.bigred, 463, y0 + 63, 103, 24 );
			text( "JOIN MATCH", 549, y0 + 80, { scale: 0.25, fontSize: 24, align: "right", color: hot ? FOCUS : "#fff" } );
		}

		var tog = hit( "toggleMode", 78, y0 + 108, 140, 14, function ()
		{
			state.teamMode = !state.teamMode;
			toast( state.teamMode ? "Team gametype UI" : "FFA / CA join UI" );
		} );
		text( state.teamMode ? "[demo] show FFA join" : "[demo] show team join", 78, y0 + 118, { scale: 0.16, fontSize: 16, color: tog ? FOCUS : "#666" } );
	}

	function drawControls()
	{
		var y0 = 129;
		panelFrame( 154 );
		text( "CONTROLS", 78, y0 + 12, { scale: 0.18, fontSize: 16 } );
		[ "MOVEMENT", "WEAPONS", "LOOK", "MISC" ].forEach( function ( c, i )
		{
			var x = 80 + i * 110;
			var active = i === 0;
			var hot = hit( "ctrlcat:" + i, x, y0 + 36, 100, 18, function () { toast( "Controls: " + c ); } );
			if ( active )
				fillRect( x, y0 + 36, 100, 18, "rgba(40,40,40,0.85)" );
			text( c, x + 50, y0 + 48, { scale: 0.18, fontSize: 16, align: "center", color: active ? "#ff6644" : hot ? FOCUS : "#999" } );
		} );
		[ [ "Forward", "W" ], [ "Back", "S" ], [ "Left", "A" ], [ "Right", "D" ], [ "Jump", "SPACE" ], [ "Crouch", "C" ] ].forEach( function ( b, i )
		{
			var rowY = y0 + 70 + i * 16;
			text( b[0], 95, rowY + 12, { scale: 0.2, fontSize: 16, color: "#ccc" } );
			var hot = hit( "bind:" + i, 320, rowY, 160, 14, function () { toast( "Rebind " + b[0] ); } );
			fillRect( 320, rowY, 160, 14, hot ? "rgba(255,191,0,0.15)" : "rgba(0,0,0,0.35)" );
			text( b[1], 400, rowY + 12, { scale: 0.2, fontSize: 16, align: "center", color: hot ? FOCUS : "#fff" } );
		} );
	}

	function drawOptions()
	{
		var y0 = 129;
		panelFrame( 194 );
		text( "GAME SETTINGS", 78, y0 + 12, { scale: 0.18, fontSize: 16 } );
		[
			{ label: "Crosshair", value: "Style 2" },
			{ label: "Crosshair Size", value: "####--" },
			{ label: "FOV", value: "110" },
			{ label: "Mouse Sensitivity", value: "4.2" },
			{ label: "Show FPS", value: "On" },
			{ label: "Weapon Bar", value: "On" },
			{ label: "Blood", value: "On" },
			{ label: "Simple Items", value: "Off" }
		].forEach( function ( o, i )
		{
			var rowY = y0 + 40 + i * 18;
			var hot = hit( "opt:" + i, 90, rowY, 420, 16, function () { toast( "Toggled " + o.label ); } );
			text( o.label, 95, rowY + 12, { scale: 0.2, fontSize: 16, color: hot ? FOCUS : "#ddd" } );
			text( o.value, 480, rowY + 12, { scale: 0.2, fontSize: 16, align: "right", color: "#aaa" } );
		} );
	}

	function drawVote()
	{
		var y0 = 129;
		panelFrame( 214 );
		text( "CALL VOTE OPTIONS", 78, y0 + 12, { scale: 0.18, fontSize: 16 } );
		text( "Map Options", 90, y0 + 40, { scale: 0.18, fontSize: 16, color: "#ccc" } );
		[
			{ label: "map dm_campgrounds", y: 47 },
			{ label: "Restart Map", y: 67 },
			{ label: "Next Map", y: 87 },
			{ label: "Shuffle Teams", y: 107 },
			{ label: "kick Bob", y: 140 },
			{ label: "timelimit 15", y: 160 },
			{ label: "g_doWarmup 0", y: 180 },
			{ label: "Call Vote", y: 200 }
		].forEach( function ( v, i )
		{
			var x = i < 4 ? 90 : 320;
			var rowY = y0 + ( i < 4 ? v.y : 47 + ( i - 4 ) * 20 );
			var hot = hit( "vote:" + i, x, rowY, 200, 16, function ()
			{
				toast( "callvote " + v.label );
				state.open = false;
			} );
			fillRect( x, rowY, 200, 16, hot ? "rgba(255,191,0,0.18)" : "rgba(0,0,0,0.35)" );
			text( v.label, x + 8, rowY + 12, { scale: 0.2, fontSize: 16, color: hot ? FOCUS : "#eee" } );
		} );
	}

	function drawPopupBox( x, y, w, h )
	{
		fillRect( x + 4, y + 10, w - 8, h - 20, "rgba(0,0,0,0.8)" );
		drawImage( A.fade, x + 5, y + 14, w - 10, h - 26, 0.35 );
		drawImage( A.boxtl, x, y, 64, 64 );
		drawImage( A.boxtr, x + w - 64, y, 64, 64 );
		drawImage( A.boxbl, x, y + h - 64, 64, 64 );
		drawImage( A.boxbr, x + w - 64, y + h - 64, 64, 64 );
	}

	function drawJoinPopup()
	{
		var x = 121, y = 78, w = 128, h = 128;
		drawPopupBox( x, y, w, h );
		[
			{ label: "Auto TEAM", cy: 20, cmd: "team free" },
			{ label: "Team RED", cy: 40, cmd: "team red" },
			{ label: "Team BLUE", cy: 60, cmd: "team blue" },
			{ label: "Spectate", cy: 80, cmd: "team s" }
		].forEach( function ( it )
		{
			var hot = hit( "joinpop:" + it.label, x, y + it.cy, w, 20, function ()
			{
				toast( "exec \"cmd " + it.cmd + "\"" );
				state.popup = null;
				state.open = false;
			} );
			text( it.label, x + 64, y + it.cy + 18, { scale: 0.25, fontSize: 24, align: "center", color: hot ? FOCUS : "#fff" } );
		} );
	}

	function drawLeavePopup()
	{
		var x = 121, y = 238, w = 128, h = 128;
		drawPopupBox( x, y, w, h );

		if ( state.popup === "leave" )
		{
			[
				{ label: "Return to Game", cy: 30, action: function () { state.open = false; state.popup = null; if ( !demoMode ) cbuf( "webcore_closemenu" ); } },
				{ label: "Restart", cy: 50, action: function () { state.popup = "restartConfirm"; } },
				{ label: "Quit", cy: 70, action: function () { state.popup = "quitConfirm"; } }
			].forEach( function ( it )
			{
				var hot = hit( "leave:" + it.label, x, y + it.cy, w, 20, it.action );
				text( it.label, x + 64, y + it.cy + 18, { scale: 0.25, fontSize: 24, align: "center", color: hot ? FOCUS : "#fff" } );
			} );
			return;
		}

		var isQuit = state.popup === "quitConfirm";
		text( "Want to", x + 64, y + 33 + 18, { scale: 0.25, fontSize: 24, align: "center" } );
		text( isQuit ? "Quit Game?" : "Restart Map?", x + 64, y + 50 + 18, { scale: 0.25, fontSize: 24, align: "center" } );

		var yesHot = hit( "confirmYes", x + 18, y + 80, 40, 20, function ()
		{
			if ( demoMode ) { toast( isQuit ? "disconnect" : "map_restart" ); state.popup = null; return; }
			cbuf( isQuit ? "disconnect" : "map_restart" );
			state.open = false;
			state.popup = null;
		} );
		var noHot = hit( "confirmNo", x + 70, y + 80, 40, 20, function () { state.popup = "leave"; } );

		drawImage( A.btnBack, x + 18, y + 80, 40, 20 );
		drawImage( A.btnBack, x + 70, y + 80, 40, 20 );
		fillRect( x + 18, y + 80, 40, 20, yesHot ? "rgba(26,94,26,0.85)" : "rgba(94,26,26,0.85)" );
		fillRect( x + 70, y + 80, 40, 20, noHot ? "rgba(26,94,26,0.85)" : "rgba(94,26,26,0.85)" );
		text( "Yes", x + 38, y + 80 + 15, { scale: 0.25, fontSize: 24, align: "center" } );
		text( "No", x + 90, y + 80 + 15, { scale: 0.25, fontSize: 24, align: "center" } );
	}

	// --- warmup view (intro.smenu) ---------------------------------------------------

	function readyCount() { return PLAYERS.filter( function ( p ) { return p.ready && p.team !== "spec"; } ).length; }
	function playingCount() { return PLAYERS.filter( function ( p ) { return p.team !== "spec"; } ).length; }

	function setMyTeam( team )
	{
		state.myTeam = team;
		PLAYERS[0].team = team === "free" ? "red" : team;
		if ( team === "spec" )
		{
			state.myReady = false;
			PLAYERS[0].ready = false;
			PLAYERS[0].team = "spec";
			cbuf( "team s" );
		}
		else
		{
			state.myReady = true;
			PLAYERS[0].ready = true;
			cbuf( team === "free" ? "team a" : team === "red" ? "team r" : "team b" );
		}
	}

	// The original's countdown/FIGHT/live phases only ran in its browser demo;
	// in the engine the view stays in the waiting phase.
	function drawWarmupCenterHud()
	{
		text( "WARMUP", 320, 70, { size: 42, align: "center", color: "#ffe566", cls: "pulse-warmup" } );
		text( "Waiting for players to ready up", 320, 100, { size: 13, align: "center", color: "#c8c8c8" } );
		text( readyCount() + " / 4 ready   ·   " + playingCount() + " in match", 320, 118, { size: 11, align: "center", color: "#8a93a3" } );
		if ( state.myTeam === "spec" )
			text( "Join a team to play", 320, 140, { size: 12, align: "center", color: FOCUS } );
		else if ( !state.myReady )
			text( "Press F3 / click READY", 320, 140, { size: 12, align: "center", color: FOCUS } );
		else
			text( "You are READY", 320, 140, { size: 12, align: "center", color: "#7dff9a" } );

		// mini ready roster (top right)
		var y = 56;
		text( "READY", 620, y, { size: 9, align: "right", color: "#888" } );
		y += 14;
		PLAYERS.filter( function ( p ) { return p.team !== "spec"; } ).slice( 0, 8 ).forEach( function ( p )
		{
			var col = p.team === "red" ? "#ff6b5a" : p.team === "blue" ? "#6fb6ff" : "#ccc";
			text( ( p.ready ? "●" : "○" ) + " " + p.name, 620, y, { size: 10, align: "right", color: p.ready ? col : "#666" } );
			y += 13;
		} );
	}

	// ui_hud.shader bgfill[_red]: a base plus two additive scrolling layers and two
	// rotating orbs; the canvas composited it per frame, here CSS animates it.
	function drawBgfill( x, y, w, h, team )
	{
		var c = team ? " team" : "";
		ops.push( { type: "Panel", cls: "bgfill" + c, style: box( x, y, w, h ), children: [
			{ cls: "bgfill-base", style: "background-image:url(\"" + ( team ? A.bgfillRed : A.bgfill ) + "\");" },
			{ cls: "bgfill-scroll o1" + c, img: A.bgfillO1 },
			{ cls: "bgfill-scroll o2" + c, img: A.bgfillO2 },
			{ cls: "bgfill-orb o6", style: orbStyle( w, h ) + "background-image:url(\"" + A.bgfillO6 + "\");" },
			{ cls: "bgfill-orb o7", style: orbStyle( w, h ) + "background-image:url(\"" + A.bgfillO7 + "\");" }
		] } );
	}

	function orbStyle( w, h )
	{
		var s = Math.max( w, h ) * 1.7;
		return box( w * 0.22 - s / 2, h * 0.55 - s / 2, s, s );
	}

	// the canvas filled the image's shape with one colour; wash-color tints it
	function drawModulated( img, x, y, w, h, r, g, b, a )
	{
		drawImage( img, x, y, w, h, a, "wash-color:rgb(" + r + "," + g + "," + b + ");" );
	}

	function drawGametypeTabs( x, y, w, h )
	{
		drawModulated( A.gtbox, x, y, w, h, 55, 58, 64, 0.5 );
		var labels = state.teamMode ? [ "TDM", "Timelimit: 10", "Frag Limit: 0" ] : [ "Duel", "Timelimit: 5", "Frag Limit: 10" ];
		var tabW = [ 78, 110, 118 ];
		var tx = x + 4;
		for ( var i = 0; i < 3; i++ )
		{
			var tw = tabW[i];
			// the canvas drew skewed tabs; Panorama has no skew, so they are boxes
			ops.push( { type: "Panel", cls: "rect", style: box( tx + 3, y - 1, tw - 6, h + 2 ) +
				"background-color:rgba(48, 52, 58, 0.75);border:" + px( 1 ) + " solid rgba(170, 170, 170, 0.2);" } );
			text( labels[i], tx + tw / 2, y + h / 2, { font: "hooge_05_57", scale: 0.18, fontSize: 16, align: "center", baseline: "middle", color: "#e8e8e8" } );
			tx += tw + 6;
		}
	}

	function drawJoinGamePanel()
	{
		var y0 = -10;
		drawImage( A.frametl, 62, 74 + y0, 40, 40, 0.9 );
		drawImage( A.framet, 102, 74 + y0, 436, 40, 0.9 );
		drawImage( A.frametr, 538, 74 + y0, 40, 40, 0.9 );
		drawImage( A.framel, 62, 114 + y0, 40, 210, 0.9 );
		drawImage( A.framem, 102, 114 + y0, 436, 210, 0.9 );
		drawImage( A.framer, 538, 114 + y0, 40, 210, 0.9 );
		drawImage( A.framebl, 62, 324 + y0, 40, 20, 0.9 );
		drawImage( A.frameb, 102, 324 + y0, 436, 20, 0.9 );
		drawImage( A.framebr, 538, 324 + y0, 40, 20, 0.9 );

		text( "MATCH WARMUP - Campgrounds", 82, 88 + y0, { scale: 0.2, fontSize: 16, color: "#ccc" } );

		drawBgfill( 83, 94 + y0, 322, 75, state.teamMode );
		drawModulated( A.logo2, 68, 91 + y0, 111, 101, 232, 94, 3, 0.18 );
		drawImage( A.arrow, 69, 79 + y0, 10, 10 );
		drawGametypeTabs( 78, 96 + y0, 323, 11 );

		drawImage( A.adtl, 50, 93 + y0, 40, 80 );
		drawImage( A.adtm, 90, 93 + y0, 160, 80 );
		drawImage( A.adtr, 250, 93 + y0, 320, 80 );
		drawImage( A.adbr, 250, 173 + y0, 320, 20 );

		fillRect( 407, 95 + y0, 160, 80, "rgba(0,0,0,0.9)" );
		drawImage( A.ad2x1, 407, 95 + y0, 160, 80 );

		text( "Welcome to Quake Live", 77, 127 + y0, { scale: 0.35, fontSize: 24 } );
		text( "Unranked Match", 80, 143 + y0, { scale: 0.24, fontSize: 24, color: "#b3b3b3" } );
		text( "This game is hosted by Quake Live", 80, 164 + y0, { scale: 0.24, fontSize: 24, color: FOCUS } );

		drawImage( A.scoretlA, 71, 185 + y0, 160, 20 );
		drawImage( A.scoretl2, 231, 185 + y0, 80, 20 );
		drawImage( A.scoretmA, 311, 185 + y0, 107, 20 );
		drawImage( A.scoretr2, 418, 185 + y0, 40, 20 );
		drawImage( A.scorel, 71, 205 + y0, 10, 88 );
		drawImage( A.scorem, 81, 205 + y0, 367, 88 );
		drawImage( A.scorer, 448, 205 + y0, 10, 88 );
		drawImage( A.scorebl, 71, 293 + y0, 10, 10 );
		drawImage( A.scoreb, 81, 293 + y0, 367, 10 );
		drawImage( A.scorebr, 448, 293 + y0, 10, 10 );

		text( "ABOUT THIS MATCH", 78, 197 + y0, { scale: 0.18, fontSize: 16 } );
		var grey = { scale: 0.2, fontSize: 24, color: "#999" };
		if ( state.teamMode )
		{
			text( "This is a Team Deathmatch game", 86, 218 + y0, { scale: 0.2, fontSize: 24 } );
			text( "Eliminate the other team!", 95, 236 + y0, grey );
			text( "First team to reach the Frag Limit wins.", 95, 249 + y0, grey );
			text( "If time runs out, highest score wins.", 95, 262 + y0, grey );
			text( "Warmup until enough players are ready.", 95, 275 + y0, grey );
			text( "Server: localdemo", 95, 288 + y0, grey );
		}
		else
		{
			text( "This is a 1 vs 1 Duel game", 86, 218 + y0, { scale: 0.2, fontSize: 24 } );
			text( "Defeat your opponent!", 95, 236 + y0, grey );
			text( "First player to reach the Frag Limit wins.", 95, 249 + y0, grey );
			text( "If time runs out, the player with the highest score wins.", 95, 262 + y0, grey );
			text( "If the time limit is hit and there is a tie a Sudden Death round begins.", 95, 275 + y0, grey );
			text( "Server: localdemo", 95, 288 + y0, grey );
		}

		// bottom nav: LEAVE MATCH | status | SPECTATE
		hit( "wLeave", 73, 312 + y0, 80, 20, function () { cbuf( "disconnect" ); } );
		drawImage( A.navlefty, 73, 312 + y0, 80, 20 );
		text( "LEAVE MATCH", 99, 326 + y0, { scale: 0.2, fontSize: 16, color: "#ccc" } );
		text( state.myTeam === "spec" ? "Join Match to Begin Playing" : "Warmup - free fire until ready", 320, 326 + y0,
			{ scale: 0.2, fontSize: 16, align: "center", color: FOCUS } );
		hit( "wSpecNav", 485, 312 + y0, 80, 20, function () { setMyTeam( "spec" ); } );
		drawImage( A.navrighty, 485, 312 + y0, 80, 20 );
		text( "SPECTATE", 539, 326 + y0, { scale: 0.2, fontSize: 16, align: "right", color: "#ccc" } );

		// spectator strip: timer | player ticker | count
		drawImage( A.specl, 71, 346 + y0, 80, 20, 0.5 );
		drawImage( A.specm, 151, 346 + y0, 338, 20, 0.5 );
		drawImage( A.specflip, 489, 346 + y0, 80, 20, 0.5 );
		text( "0:00", 96, 360 + y0, { scale: 0.22, fontSize: 16, color: "#bababa" } );
		text( PLAYERS.map( function ( p ) { return p.name; } ).join( "   " ) || "—", 320, 360 + y0, { scale: 0.2, fontSize: 16, align: "center", color: "#999" } );
		text( "2/16 Players", 529, 360 + y0, { scale: 0.2, fontSize: 16, align: "center", color: "#999" } );

		if ( state.teamMode )
		{
			[
				{ id: "wJoinRed", label: "JOIN RED", img: A.navright, y: 206, team: "red" },
				{ id: "wJoinBlue", label: "JOIN BLUE", img: A.navrightb, y: 236, team: "blue" },
				{ id: "wAuto", label: "AUTO JOIN", img: A.navrighty, y: 266, team: "free" }
			].forEach( function ( row )
			{
				var hy = row.y + y0;
				var hot = hit( row.id, 485, hy, 80, 20, function () { setMyTeam( row.team ); } );
				drawImage( row.img, 485, hy, 80, 20 );
				text( row.label, 539, hy + 18, { scale: 0.2, fontSize: 16, align: "right", color: hot || state.myTeam === row.team ? FOCUS : "#fff" } );
			} );
		}
		else
		{
			var hy = 272 + y0;
			var hot = hit( "wJoinMatch", 464, hy, 103, 24, function () { setMyTeam( "free" ); } );
			drawImage( A.bigred, 464, hy, 103, 24 );
			text( "JOIN MATCH", 550, hy + 18, { scale: 0.25, fontSize: 24, align: "right", color: hot ? FOCUS : "#fff" } );
		}

		if ( state.myTeam !== "spec" )
		{
			var readyHot = hit( "wReady", 78, 290 + y0, 100, 18, function () { cbuf( "ready" ); } );
			fillRect( 78, 290 + y0, 100, 18, state.myReady ? "rgba(30,120,50,0.8)" : "rgba(80,30,30,0.8)" );
			text( state.myReady ? "READY" : "CLICK TO READY", 128, 299 + y0, { scale: 0.18, fontSize: 16, align: "center", color: readyHot ? FOCUS : "#fff" } );
		}

		var tog = hit( "wToggleTeam", 200, 290 + y0, 140, 16, function ()
		{
			state.teamMode = !state.teamMode;
			toast( state.teamMode ? "Team warmup UI" : "FFA warmup UI" );
		} );
		text( state.teamMode ? "[demo] FFA buttons" : "[demo] team buttons", 200, 298 + y0, { scale: 0.16, fontSize: 16, color: tog ? FOCUS : "#555" } );
	}

	function drawViewTabs()
	{
		[ { id: "warmup", label: "1 WARMUP", x: 8 }, { id: "esc", label: "2 ESC MENU", x: 88 } ].forEach( function ( t )
		{
			var on = state.view === t.id;
			var hot = hit( "view:" + t.id, t.x, 4, 76, 16, function ()
			{
				state.view = t.id;
				state.open = t.id === "esc";
				state.popup = null;
				toast( t.id === "warmup" ? "Warmup / join match UI" : "Esc in-game menu" );
			} );
			fillRect( t.x, 4, 76, 16, on ? "rgba(255,191,0,0.25)" : hot ? "rgba(255,255,255,0.1)" : "rgba(0,0,0,0.45)" );
			text( t.label, t.x + 38, 14, { scale: 0.16, fontSize: 16, align: "center", color: on || hot ? FOCUS : "#9aa3b2" } );
		} );
	}

	// --- arena selector -----------------------------------------------------------------

	// chip: navright 80x20, label right-aligned at x+54; join: bigred 103x24, label at x+86
	function drawArenaButton( id, label, x, y, onClick, opts )
	{
		opts = opts || {};
		var join = opts.kind === "join";
		var active = !!opts.active;
		var w = join ? 103 : 80, h = join ? 24 : 20;
		var hot = hit( id, x, y, w, h, onClick );
		var art = opts.img || ( join ? A.bigred : active || hot ? A.navrighty : A.navright );
		drawImage( art, x, y, w, h );
		text( label, x + ( join ? 86 : 54 ), y + 18, {
			scale: join ? 0.25 : 0.2,
			fontSize: join ? 24 : 16,
			align: "right",
			color: hot || active ? FOCUS : "#fff"
		} );
	}

	function arenaHeroLabel( hero )
	{
		return String( ( hero && ( hero.name || hero.id ) ) || "" ).replace( /^hero_/i, "" );
	}

	function drawArenaSelector()
	{
		var a = arenaSnapshot;
		if ( !a || !a.active )
		{
			text( "Waiting for arena state…", 320, 235, { size: 16, align: "center", color: "#bbb" } );
			return;
		}

		var phase = String( a.phase || "WARMUP" );
		var local = a.local || {};
		var playerState = Number( local.state || 0 );
		var needsHero = !!local.needsHero;
		var heroName = local.hero || "No hero selected";
		var players = ( Array.isArray( a.players ) ? a.players.filter( Boolean ) : [] ).slice( 0, 2 );
		var canReady = playerState === 0 && !!local.hero && !needsHero;
		var chipX = 468;
		var joinX = 445;
		// the game names its arena mode ("mode": duel, freezetag, archstiletto;
		// "modeName": FREEZE TAG ...); a snapshot without one is Duel's
		var modeId = String( a.mode || "duel" ).toLowerCase();
		var isDuel = modeId === "duel";
		var modeTitle = String( a.modeName || a.mode || "DUEL" ).toUpperCase() + " ARENA";

		scoreInset( 68, 58, 504, 332 );
		text( modeTitle, 84, 88, { scale: 0.35, fontSize: 24, color: "#fff" } );
		text( String( a.map || "unknown" ).toUpperCase(), 84, 108, { scale: 0.2, fontSize: 16, color: FOCUS } );
		text( phase, 550, 88, { scale: 0.26, fontSize: 24, align: "right", color: phase === "LIVE" ? "#7dff9a" : FOCUS } );
		text( String( a.round || "Warmup" ), 550, 108, { scale: 0.18, fontSize: 16, align: "right", color: "#aaa" } );

		text( isDuel ? "CURRENT DUEL" : "CURRENT MATCH", 92, 145, { scale: 0.18, fontSize: 16, color: "#aaa" } );
		if ( players.length )
			players.forEach( function ( n, i ) { text( n, 105, 170 + i * 24, { scale: 0.24, fontSize: 24, color: i ? "#ff8a78" : "#9ecfff" } ); } );
		else
			text( "Waiting for competitors", 105, 170, { scale: 0.22, fontSize: 24, color: "#777" } );
		text( Number( a.readyCount || 0 ) + " queued and ready", 92, 232, { scale: 0.2, fontSize: 16, color: "#bbb" } );
		var status = needsHero ? "CHOOSE A HERO" : playerState === 1 ? "READY" : playerState === 2 ? "SPECTATING" :
			playerState === 3 ? "COMPETING" : canReady ? "PRESS READY UP" : "WAITING";
		text( "YOUR STATUS: " + status, 92, 258, { scale: 0.2, fontSize: 16, color: needsHero || canReady ? FOCUS : "#ddd" } );
		text( "HERO: " + heroName, 92, 280, { scale: 0.2, fontSize: 16, color: "#aaa" } );

		if ( needsHero )
		{
			var gridX = 300, colStep = 88; // 80px chip + 8px gutter
			text( "SELECT YOUR HERO", gridX, 145, { scale: 0.2, fontSize: 16, color: FOCUS } );
			var heroes = ( Array.isArray( a.heroes ) ? a.heroes : [] ).slice( 0, 8 );
			heroes.forEach( function ( hero, i )
			{
				var picked = local.hero === hero.id;
				drawArenaButton( "hero:" + hero.id, arenaHeroLabel( hero ), gridX + ( i % 2 ) * colStep, 160 + Math.floor( i / 2 ) * 28,
					function () { arenaAction( "hero:" + hero.id ); },
					{ active: picked, img: picked ? A.navrighty : A.navright } );
			} );
			var rows = Math.ceil( heroes.length / 2 );
			drawArenaButton( "spectate", "SPECTATE", chipX, 160 + rows * 28 + 8, function () { arenaAction( "spectate" ); }, { img: A.navrighty } );
		}
		else
		{
			var by = 206;
			if ( playerState === 2 )
			{
				// pure spectator: back in through the join/hero path
				drawArenaButton( "play", "JOIN QUEUE", joinX, by, function () { arenaAction( "play" ); }, { kind: "join" } );
			}
			else if ( playerState === 1 || playerState === 3 )
			{
				// queued or competing: change hero, or leave to non-competing spectate
				drawArenaButton( "choosehero", "CHOOSE HERO", chipX, by, function () { arenaAction( "choosehero" ); }, { img: A.navright } );
				by += 30;
				drawArenaButton( "leaveQueue", "LEAVE QUEUE", chipX, by, function () { arenaAction( "spectate" ); }, { img: A.navrighty } );
			}
			else if ( canReady )
			{
				drawArenaButton( "ready", "READY UP", joinX, by, function () { arenaAction( "ready" ); }, { kind: "join" } );
				by += 30;
				drawArenaButton( "choosehero", "CHOOSE HERO", chipX, by, function () { arenaAction( "choosehero" ); }, { img: A.navright } );
				by += 30;
				drawArenaButton( "leaveQueue", "LEAVE QUEUE", chipX, by, function () { arenaAction( "spectate" ); }, { img: A.navrighty } );
			}
			else
			{
				drawArenaButton( "join", "JOIN MATCH", joinX, by, function () { arenaAction( "join" ); }, { kind: "join" } );
				by += 30;
				drawArenaButton( "spectate", "SPECTATE", chipX, by, function () { arenaAction( "spectate" ); }, { img: A.navrighty } );
			}
		}

		text( state.status, 320, 342, { scale: 0.18, fontSize: 16, align: "center", color: now() < state.flashUntil ? FOCUS : "#aeb8c6", alpha: 0.9 } );
		text( "ESC / B  BACK", 532, 368, { scale: 0.18, fontSize: 16, align: "right", color: "#999" } );
	}

	// --- frame ----------------------------------------------------------------------------

	function draw()
	{
		ops = [];
		hits = [];
		hitById = {};
		if ( arenaMode )
		{
			drawArenaSelector();
			return;
		}
		drawViewTabs();
		if ( state.view === "warmup" )
		{
			drawWarmupCenterHud();
			drawJoinGamePanel();
		}
		else if ( !state.open )
		{
			drawClosedHint();
		}
		else
		{
			drawShell();
			if ( state.tab === "about" ) drawAbout();
			else if ( state.tab === "controls" ) drawControls();
			else if ( state.tab === "options" ) drawOptions();
			else if ( state.tab === "vote" ) drawVote();
			drawBottomNav();
			if ( state.popup === "join" ) drawJoinPopup();
			if ( state.popup === "leave" || state.popup === "quitConfirm" || state.popup === "restartConfirm" ) drawLeavePopup();
		}
		text( state.status, 320, 468, { scale: 0.18, fontSize: 16, align: "center", color: now() < state.flashUntil ? FOCUS : "#7a8494", alpha: 0.9 } );
	}

	// ops with nested children (the bgfill compositor) build their own subtree
	function makeOp( op )
	{
		return function ( parent )
		{
			var props = { "class": op.cls, style: op.style, hittest: "false" };
			if ( op.text !== undefined )
				props.text = op.text;
			var p = $.CreatePanel( op.type, parent, "", props );
			( op.children || [] ).forEach( function ( c )
			{
				if ( c.img )
				{
					// a strip two images wide that CSS slides by half its width
					var strip = $.CreatePanel( "Panel", p, "", { "class": c.cls, hittest: "false" } );
					$.CreatePanel( "Panel", strip, "", { "class": "bgfill-tile", style: "background-image:url(\"" + c.img + "\");", hittest: "false" } );
					$.CreatePanel( "Panel", strip, "", { "class": "bgfill-tile", style: "background-image:url(\"" + c.img + "\");", hittest: "false" } );
				}
				else
				{
					$.CreatePanel( "Panel", p, "", { "class": c.cls, style: c.style || "", hittest: "false" } );
				}
			} );
			return p;
		};
	}

	// 3_cursor3.png is 32x32 with the arrow tip (hotspot) at about 18,18. The
	// pointer is mapped into the stage and clamped to it, as the canvas did.
	var CURSOR_HOT = 18, CURSOR_SIZE = 32;
	var cursorK = 0, cursorAt = "";
	function drawCursor()
	{
		if ( cursorK !== K )
		{
			cursorK = K;
			cursor.style.width = px( CURSOR_SIZE );
			cursor.style.height = px( CURSOR_SIZE );
		}
		var mx = ( Bulwark.MouseX() - ( viewW - W * K ) / 2 ) / K;
		var my = ( Bulwark.MouseY() - ( viewH - H * K ) / 2 ) / K;
		mx = Math.max( 0, Math.min( W, mx ) );
		my = Math.max( 0, Math.min( H, my ) );
		// position can't be restyled from script; a translation moves the paint only
		var at = "translate3d( " + px( mx - CURSOR_HOT ) + ", " + px( my - CURSOR_HOT ) + ", 0px )";
		if ( at !== cursorAt )
		{
			cursorAt = at;
			cursor.style.transform = at;
		}
	}

	var lastScaleCheck = 0;
	function frame()
	{
		var t = now();
		if ( t >= lastScaleCheck )
		{
			lastScaleCheck = t + 100;
			var k = computeScale();
			if ( Math.abs( k - K ) > 0.0001 || !stage.__sized )
			{
				K = k;
				stage.__sized = true;
				stage.style.width = px( W );
				stage.style.height = px( H );
			}
		}
		syncArena( t );
		draw();
		artLayer.commit( ops.map( function ( op )
		{
			return { key: op.type + "|" + op.cls + "|" + op.style + "|" + ( op.text || "" ) + "|" + ( op.children ? JSON.stringify( op.children ) : "" ), make: makeOp( op ) };
		} ) );
		hitLayer.commit( hitList() );
		drawCursor();
		$.Schedule( 1 / 60, frame );
	}

	// keyboard arrows and a controller's d-pad (the engine names it as the
	// arrows) move the highlight to the nearest hit area that way; Enter or
	// A presses it, as a click would
	function padMove( dx, dy )
	{
		var list = hits.filter( function ( h ) { return h.id !== "__outside"; } );
		if ( !list.length )
			return;
		var cur = null;
		list.forEach( function ( h ) { if ( h.id === state.hover ) cur = h; } );
		if ( !cur )
		{
			// nothing lit yet: start at the top left
			list.sort( function ( a, b ) { return ( a.y - b.y ) || ( a.x - b.x ); } );
			state.hover = list[0].id;
			return;
		}
		var cx = cur.x + cur.w / 2, cy = cur.y + cur.h / 2, best = null, bestScore = Infinity;
		list.forEach( function ( h )
		{
			if ( h === cur )
				return;
			var ox = h.x + h.w / 2 - cx, oy = h.y + h.h / 2 - cy;
			var along = ox * dx + oy * dy, across = Math.abs( ox * dy ) + Math.abs( oy * dx );
			if ( along <= 0 )
				return;
			var score = along + across * 2;
			if ( score < bestScore )
				bestScore = score, best = h;
		} );
		if ( best )
			state.hover = best.id;
	}

	var PAD_DIRS = { ArrowUp: [ 0, -1 ], ArrowDown: [ 0, 1 ], ArrowLeft: [ -1, 0 ], ArrowRight: [ 1, 0 ] };

	function onKey( e )
	{
		if ( !e || !e.down )
			return;
		var key = e.key;
		if ( PAD_DIRS[key] )
		{
			padMove( PAD_DIRS[key][0], PAD_DIRS[key][1] );
			return;
		}
		if ( key === "Enter" )
		{
			if ( state.hover && hitById[state.hover] )
				onHitClick( state.hover );
			else
				padMove( 0, 1 );
			return;
		}
		if ( key === "1" && !arenaMode )
		{
			state.view = "warmup";
			state.open = false;
			state.popup = null;
			state.myTeam = "spec";
			state.myReady = false;
			PLAYERS[0].team = "spec";
			PLAYERS[0].ready = false;
			toast( "Warmup view (reset)" );
			return;
		}
		if ( key === "2" && !arenaMode )
		{
			state.view = "esc";
			state.open = true;
			state.popup = null;
			toast( "Esc menu view" );
			return;
		}
		if ( key === "Escape" )
		{
			if ( arenaMode )
			{
				arenaAction( "close" );
				return;
			}
			// esc menu: close it; warmup: the engine closes the surface itself
			if ( state.view === "esc" && state.open )
			{
				state.open = false;
				cbuf( "webcore_closemenu" );
			}
		}
	}

	function init()
	{
		if ( fte_query( "cvar:ql_menu_debug" ) === "1" )
		{
			var q = fte_query;
			fte_query = function ( r ) { var x = q( r ); if ( r !== "getarena" && r.indexOf( "cvar:" ) !== 0 ) $.Msg( "QL " + r + " -> " + x ); return x; };
		}
		root = $( "#QlPage" ) || $.GetContextPanel();
		stage = $.CreatePanel( "Panel", root, "Stage", { hittest: "false" } );
		artLayer = new Layer( $.CreatePanel( "Panel", stage, "Art", { "class": "layer", hittest: "false" } ) );
		hitLayer = new Layer( $.CreatePanel( "Panel", stage, "Hits", { "class": "layer", hittest: "false" } ) );
		cursor = $.CreatePanel( "Panel", stage, "Cursor", { "class": "cursor", hittest: "false" } );
		toast( "Warmup UI · join a team + ready (need 4) · 1/2 views" );
		if ( typeof Bulwark === "object" )
			Bulwark.On( "key", onKey );
		frame();
	}

	init();

	// for tests through ui_eval
	return {
		state: state,
		hover: function ( id ) { state.hover = id; },
		click: function ( id ) { onHitClick( id ); },
		key: function ( k ) { onKey( { key: k, down: true } ); }
	};
})();
