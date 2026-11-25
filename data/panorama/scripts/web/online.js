// The online hub (layout/web/online.xml). State from fte_query("getflow")
// (Live's state is in it), actions through "flow:..." and allowlisted cbuf:.
var Online = ( function ()
{
	var COUNT = 6;
	var SND_NAV = "misc/menu1", SND_OK = "misc/menu2", SND_BACK = "misc/menu3";
	var HERO_NAMES = { hero_archstiletto: "Archstiletto", hero_motionblue: "Motionblue", hero_skipstream: "Skipstream" };
	var selected = 0;
	var f = null;
	var noticeSeq = -1, noticeUntil = 0;
	var searchStart = 0;
	var askedCard = 0;
	var openedAt = Date.now();
	var askedSeats = "";

	function q( req )
	{
		if ( typeof fte_query !== "function" )
			return null;
		try { return fte_query( req ); } catch ( e ) { return null; }
	}
	function flow( v ) { return q( "flow:" + v ); }
	function cbuf( c ) { return q( "cbuf:" + c ) === "ok"; }
	function sound( s ) { q( "localsound:" + s ); }
	function show( id, on ) { $( id ).SetHasClass( "hidden", !on ); }
	function heroName( id )
	{
		if ( HERO_NAMES[ id ] ) return HERO_NAMES[ id ];
		var n = String( id || "" ).replace( /^hero_/, "" );
		return n.charAt( 0 ).toUpperCase() + n.slice( 1 );
	}
	function fmtTime( s )
	{
		s = Math.max( 0, Math.floor( s ) );
		var m = Math.floor( s / 60 ), r = s % 60;
		return m + ":" + ( r < 10 ? "0" : "" ) + r;
	}

	function live() { return ( f && f.live ) || {}; }
	function myCard()
	{
		var l = live();
		return l.cards && l.id ? l.cards[ String( l.id ) ] : null;
	}

	function select( i, snd )
	{
		i = ( i + COUNT ) % COUNT;
		if ( i !== selected && snd !== false )
			sound( SND_NAV );
		selected = i;
		for ( var k = 0; k < COUNT; k++ )
			$( "#Item" + k ).SetHasClass( "is-selected", k === selected );
	}

	function searching() { return !!live().queue; }

	function activate( i )
	{
		if ( foundShowing() )
			return;
		switch ( i )
		{
			case 0:
				if ( searching() ) { cancel(); return; }
				sound( SND_OK );
				flow( "queue:duel" );
				break;
			case 1:
				sound( SND_BACK );
				break;
			case 2:
				sound( SND_OK );
				if ( searching() )
					flow( "cancel" );
				if ( cbuf( "lobby_create_online" ) )
					cbuf( "menu_webcore_lobby" );
				break;
			case 3:
				sound( SND_OK );
				flow( "training" );
				break;
			case 4:
				flow( "after:" + ( f && f.after === "search" ? "menu" : "search" ) );
				sound( SND_NAV );
				break;
			case 5:
				back();
				break;
		}
	}

	function cancel()
	{
		if ( !searching() )
			return;
		flow( "cancel" );
		sound( SND_BACK );
	}

	function back()
	{
		if ( searching() )
			flow( "cancel" );
		sound( SND_BACK );
		cbuf( "menu_webcore_title" );
	}

	function formCells( parent, form )
	{
		parent.RemoveAndDeleteChildren();
		String( form || "" ).split( "" ).forEach( function ( c )
		{
			var cell = $.CreatePanel( "Panel", parent, "", { class: "form-cell " + c, hittest: "false" } );
			$.CreatePanel( "Label", cell, "", { text: c, hittest: "false" } );
		} );
	}

	var cardKey = "";
	function renderCard()
	{
		var c = myCard();
		var l = live();
		var key = JSON.stringify( c ) + "|" + ( l.connected ? 1 : 0 );
		if ( key === cardKey )
			return;
		cardKey = key;
		if ( !c || c.none )
		{
			$( "#CardName" ).text = l.connected ? q( "cvar:name" ) || "Fighter" : "Offline";
			$( "#CardDays" ).text = "0";
			$( "#CardSets" ).text = "0";
			$( "#CardWL" ).text = "0 - 0";
			$( "#CardDone" ).text = "-";
			formCells( $( "#CardForm" ), "" );
			$( "#CardHeroes" ).text = "No sets yet";
			$( "#CardQuits" ).text = "";
			return;
		}
		$( "#CardName" ).text = c.name || "Fighter";
		$( "#CardDays" ).text = String( c.active_days || 0 );
		$( "#CardSets" ).text = String( c.sets || 0 );
		$( "#CardWL" ).text = ( c.wins || 0 ) + " - " + ( c.losses || 0 );
		$( "#CardDone" ).text = ( c.sets ? c.completion : 100 ) + "%";
		formCells( $( "#CardForm" ), c.form );
		$( "#CardHeroes" ).text = ( c.heroes || [] ).map( function ( h ) { return heroName( h.id ) + " " + h.sets; } ).join( "   " ) || "-";
		var bits = [];
		if ( c.quits ) bits.push( c.quits + " quit" + ( c.quits > 1 ? "s" : "" ) + ( c.quits_behind ? " (" + c.quits_behind + " while behind)" : "" ) );
		if ( c.disconnects ) bits.push( c.disconnects + " disconnect" + ( c.disconnects > 1 ? "s" : "" ) );
		if ( c.runbacks ) bits.push( c.runbacks + " runback" + ( c.runbacks > 1 ? "s" : "" ) );
		if ( c.forfeit_wins ) bits.push( c.forfeit_wins + " forfeit win" + ( c.forfeit_wins > 1 ? "s" : "" ) );
		$( "#CardQuits" ).text = bits.join( "  -  " );
	}

	function foundShowing() { return f && f.match_age < 10 && !f.ingame && live().match; }

	var foundKey = "";
	function renderFound()
	{
		var on = !!foundShowing();
		show( "#Found", on );
		if ( !on )
			return;
		var l = live();
		var seats = ( l.match.seats || [] ).filter( function ( s ) { return s.slot === 0; } );
		// me on the left
		seats.sort( function ( a, b ) { return ( a.id === l.id ? 0 : 1 ) - ( b.id === l.id ? 0 : 1 ); } );
		var key = JSON.stringify( seats ) + JSON.stringify( l.cards || {} );
		if ( key !== foundKey )
		{
			foundKey = key;
			for ( var i = 0; i < 2; i++ )
			{
				var p = $( "#FoundCard" + i );
				p.RemoveAndDeleteChildren();
				var s = seats[ i ];
				if ( !s )
					continue;
				var c = l.cards ? l.cards[ String( s.id ) ] : null;
				$.CreatePanel( "Label", p, "", { class: "vs-name", text: s.name || ( c && c.name ) || "?", hittest: "false" } );
				var line = c && !c.none ? ( c.active_days || 0 ) + " active days  -  " + ( c.sets || 0 ) + " sets  -  " +
					( c.sets ? c.completion : 100 ) + "% finished" : "New fighter";
				$.CreatePanel( "Label", p, "", { class: "vs-line", text: line, hittest: "false" } );
				if ( c && c.heroes && c.heroes.length )
					$.CreatePanel( "Label", p, "", { class: "vs-line", text: "Plays " + c.heroes.map( function ( h ) { return heroName( h.id ); } ).join( ", " ), hittest: "false" } );
				var form = $.CreatePanel( "Panel", p, "", { class: "vs-form", hittest: "false" } );
				formCells( form, c && c.form );
			}
		}
		$( "#FoundSub" ).text = l.match.host === l.id ? "Hosting the set - loading the arena" : "Connecting to " + ( seats[ 1 ] ? seats[ 1 ].name : "host" ) + "...";
	}

	function render()
	{
		if ( !f )
			return;
		var l = live();
		var conn = $( "#Conn" );
		if ( l.connected )
			conn.text = "Bulwark Live  -  connected  -  " + ( l.rtt || 0 ) + " ms";
		else
			conn.text = l.error ? "Bulwark Live  -  " + l.error : "Bulwark Live  -  connecting...";
		conn.SetHasClass( "bad", !l.connected );
		var srch = searching();
		$( "#Item0" ).GetChild( 0 ).text = srch ? "Cancel Search" : "Casual Duel";
		$( "#AfterDesc" ).text = f.after === "search" ? "Search again (Enter to change)" : "Come back to this menu (Enter to change)";
		$( "#TrainDesc" ).text = srch ? "Practise while you wait for an opponent" : "Practise alone on duel_alley";
		show( "#Ribbon", srch && !foundShowing() );
		if ( srch )
		{
			if ( !searchStart )
				searchStart = Date.now();
			var waited = ( Date.now() - searchStart ) / 1000;
			$( "#RibbonText" ).text = "Searching  " + fmtTime( waited );
		}
		else
			searchStart = 0;
		// notices (why the last set ended, ...)
		if ( f.notice_seq !== noticeSeq )
		{
			noticeSeq = f.notice_seq;
			noticeUntil = f.notice ? Date.now() + 10000 : 0;
		}
		var nOn = !!f.notice && Date.now() < noticeUntil;
		show( "#Notice", nOn );
		if ( nOn )
			$( "#Notice" ).text = f.notice;
		renderCard();
		renderFound();
	}

	function poll()
	{
		var raw = q( "getflow" );
		try { f = JSON.parse( raw || "null" ); } catch ( e ) { f = null; }
		var l = live();
		// often at first: the last set's report may still be on its way
		if ( l.connected && Date.now() - askedCard > ( Date.now() - openedAt < 12000 ? 3000 : 15000 ) )
		{
			askedCard = Date.now();
			flow( "card:0" );
		}
		render();
		$.Schedule( 0.1, poll );
	}

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down )
			return;
		var k = e.key;
		if ( k === "ArrowUp" || k === "w" || k === "W" || k === "WheelUp" ) select( selected - 1 );
		else if ( k === "ArrowDown" || k === "s" || k === "S" || k === "WheelDown" ) select( selected + 1 );
		else if ( k === "Enter" || k === " " ) activate( selected );
		else if ( k === "t" || k === "T" ) { if ( searching() ) activate( 3 ); }
		else if ( k === "Escape" ) { if ( searching() ) cancel(); else back(); }
	} );

	if ( !q( "cvar:live_game" ) )
		flow( "game:stiletto" );
	if ( !live().connected )
		flow( "connect" );
	f = null;
	select( 0, false );
	poll();

	return {
		Hover: function ( i ) { select( i ); },
		Click: function ( i ) { $.GetContextPanel().SetFocus(); select( i, false ); activate( i ); },
		Cancel: function () { $.GetContextPanel().SetFocus(); cancel(); },
		Train: function () { $.GetContextPanel().SetFocus(); activate( 3 ); }
	};
} )();
