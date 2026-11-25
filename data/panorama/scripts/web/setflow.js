// The in-game set view (layout/web/setflow.xml, or setflow-v2.xml with the
// Deadlock-style hero select). Everything it shows comes
// from fte_query("getflow"): the arena director's serverinfo keys, this
// player's slot and Live's state. Choices go back as fte_query("flow:...").
var SetFlow = ( function ()
{
	var HEROES = {
		hero_angel: { name: "Angel", role: "Sniper" },
		hero_archstiletto: { name: "Archstiletto", role: "The namesake" },
		hero_beaumont: { name: "Beaumont", role: "Fast revolver" },
		hero_collier: { name: "Collier", role: "Revolver duelist" },
		hero_juliet: { name: "Juliet", role: "Zoner" },
		hero_motionblue: { name: "Motionblue", role: "Explosive slug launcher" },
		hero_skipstream: { name: "Skipstream", role: "Burst revolver" },
		hero_vagrant: { name: "Vagrant", role: "Knife brawler" }
	};
	var HP = { hero_archstiletto: 8 };
	// the hero select of setflow-v2.xml shows a line, a blurb and the kit
	// (weapon, melee, tool, guard: from decls/def/heroes/*.def)
	var KIT = {
		hero_angel: { line: "Holds the long lane", blurb: "Fights from range with a sniper rifle, and grapples to a new angle when the gap closes.",
			kit: [ "Sniper rifle", "", "Grapple", "Shield" ] },
		hero_archstiletto: { line: "The namesake", blurb: "8 HP, twice anyone else. A 2 HP armour buffer soaks hits before a punish lands, and a lunge stands in for the grapple.",
			kit: [ "Archstiletto", "Blade", "Lunge", "Shield" ] },
		hero_beaumont: { line: "Fast revolver", blurb: "A fast revolver for quick follow-up shots, with the grapple to reposition between them.",
			kit: [ "Revolver", "", "Grapple", "Shield" ] },
		hero_collier: { line: "Revolver duelist", blurb: "A sidearm for the mid range and a melee strike for anyone who gets too close.",
			kit: [ "Sidearm", "Strike", "Grapple", "Shield" ] },
		hero_juliet: { line: "Zoner", blurb: "Charges a gun to control space. No grapple: the other player has to come in.",
			kit: [ "Charge gun", "", "", "Shield" ] },
		hero_motionblue: { line: "Explosive slug launcher", blurb: "An explosive slug launcher that doubles as movement: blast jumps get Motionblue around.",
			kit: [ "Slug launcher", "", "Grapple", "Shield" ] },
		hero_skipstream: { line: "Burst revolver", blurb: "A burst revolver that rewards a steady aim, with the grapple to hold the right range.",
			kit: [ "Burst revolver", "", "Grapple", "Shield" ] },
		hero_vagrant: { line: "Knife brawler", blurb: "Always moves at a sprint. A warp knife gets in, the knife finishes up close.",
			kit: [ "Knife", "Melee", "Warp knife", "Shield" ] }
	};
	var KIT_SLOTS = [ "WEAPON", "MELEE", "TOOL", "GUARD" ];
	var V2 = !!$( "#HeroBlurb" );
	var VOTE_CMDS = [ "rematch", "rematch_hero", "leave" ];
	var VOTE_TEXT = [ "", "Rematch", "Rematch - new hero", "Leave" ];
	var SND_NAV = "misc/menu1", SND_OK = "misc/menu2";

	var f = null;          // the last getflow reply
	var heroIds = [];
	var cursor = 0;        // hero grid cursor
	var picked = "";       // hero this player picked this select
	var gridKey = "";
	var voteCursor = 0;
	var names = [ "", "" ];
	var lastState = "";
	var lastGame = -1;

	function q( req )
	{
		if ( typeof fte_query !== "function" )
			return null;
		try { return fte_query( req ); } catch ( e ) { return null; }
	}
	function flow( verb ) { return q( "flow:" + verb ); }
	function sound( s ) { q( "localsound:" + s ); }
	function show( id, on ) { $( id ).SetHasClass( "hidden", !on ); }
	function words( s ) { return String( s || "" ).trim().split( /\s+/ ).filter( function ( w ) { return w.length; } ); }
	function num( s, i ) { var w = words( s ); return i < w.length ? parseInt( w[ i ], 10 ) || 0 : 0; }
	function heroName( id ) { return id ? ( HEROES[ id ] ? HEROES[ id ].name : String( id ).replace( /^hero_/, "" ) ) : ""; }
	function heroRole( id ) { return HEROES[ id ] ? HEROES[ id ].role : ""; }

	function set()
	{
		var s = f.arena.set;
		return {
			firstTo: num( s, 0 ), game: num( s, 1 ), wins: [ num( s, 2 ), num( s, 3 ) ],
			runbacks: num( s, 4 ), result: num( s, 5 ), winnerSide: num( s, 6 ), counterSide: num( s, 7 )
		};
	}

	// --- set strip ---------------------------------------------------------------------

	function pips( panel, wins, firstTo )
	{
		var key = wins + "/" + firstTo;
		if ( panel.pipKey === key )
			return;
		panel.pipKey = key;
		panel.RemoveAndDeleteChildren();
		for ( var i = 0; i < firstTo; i++ )
			$.CreatePanel( "Panel", panel, "", { class: "pip" + ( i < wins ? " won" : "" ), hittest: "false" } );
	}

	function strip( st )
	{
		var on = f.setgame && st.firstTo > 0;
		show( "#Strip", on );
		if ( !on )
			return;
		$( "#StripName1" ).text = names[ 0 ] + ( f.me === 0 ? "  (you)" : "" );
		$( "#StripName2" ).text = ( f.me === 1 ? "(you)  " : "" ) + names[ 1 ];
		pips( $( "#StripPips1" ), st.wins[ 0 ], st.firstTo );
		pips( $( "#StripPips2" ), st.wins[ 1 ], st.firstTo );
		$( "#StripGame" ).text = st.game > 0 ? "Game " + st.game : "Set";
		$( "#StripRule" ).text = "First to " + st.firstTo + ( st.runbacks ? "  -  runback " + st.runbacks : "" );
	}

	// --- hero select -------------------------------------------------------------------

	function buildGrid()
	{
		var key = heroIds.join( " " );
		if ( key === gridKey )
			return;
		gridKey = key;
		var grid = $( "#HeroGrid" );
		grid.RemoveAndDeleteChildren();
		heroIds.forEach( function ( id, i )
		{
			if ( V2 )
			{
				var t = $.CreatePanel( "Button", grid, "Hero" + i, { class: "tile" } );
				$.CreatePanel( "Label", t, "", { class: "tile-initial", text: heroName( id ).charAt( 0 ), hittest: "false" } );
				$.CreatePanel( "Label", t, "", { class: "tile-name", text: heroName( id ), hittest: "false" } );
				$.CreatePanel( "Label", t, "", { class: "tile-hp", text: ( HP[ id ] || 4 ) + " HP", hittest: "false" } );
				t.SetPanelEvent( "onactivate", function () { pick( i ); } );
				t.SetPanelEvent( "onmouseover", function () { moveCursor( i, false ); } );
				return;
			}
			var p = $.CreatePanel( "Button", grid, "Hero" + i, { class: "hero" } );
			$.CreatePanel( "Label", p, "", { class: "hero-hp", text: ( HP[ id ] || 4 ) + " HP", hittest: "false" } );
			$.CreatePanel( "Label", p, "", { class: "hero-name", text: heroName( id ), hittest: "false" } );
			$.CreatePanel( "Label", p, "", { class: "hero-role", text: heroRole( id ), hittest: "false" } );
			p.SetPanelEvent( "onactivate", function () { pick( i ); } );
			p.SetPanelEvent( "onmouseover", function () { moveCursor( i, false ); } );
		} );
	}

	function paintGrid( locked, canPick )
	{
		heroIds.forEach( function ( id, i )
		{
			var p = $( "#Hero" + i );
			if ( !p )
				return;
			p.SetHasClass( "is-cursor", i === cursor && !locked );
			p.SetHasClass( "is-picked", id === picked );
			p.SetHasClass( "is-off", ( locked || !canPick ) && id !== picked );
		} );
		var shown = locked || !canPick ? picked : heroIds[ cursor ];
		$( "#PickName" ).text = heroName( shown ) || "-";
		$( "#PickRole" ).text = heroRole( shown );
		if ( V2 )
			showHero( shown );
	}

	var shownKey = "";
	function showHero( id )
	{
		if ( id === shownKey )
			return;
		shownKey = id;
		var k = KIT[ id ] || { line: heroRole( id ), blurb: "", kit: [ "", "", "", "" ] };
		$( "#PickRole" ).text = k.line;
		$( "#HeroBlurb" ).text = k.blurb;
		var kit = $( "#Kit" );
		kit.RemoveAndDeleteChildren();
		k.kit.forEach( function ( name, i )
		{
			var c = $.CreatePanel( "Panel", kit, "", { class: "kit" + ( name ? "" : " none" ), hittest: "false" } );
			var disc = $.CreatePanel( "Panel", c, "", { class: "kit-disc", hittest: "false" } );
			$.CreatePanel( "Label", disc, "", { class: "kit-glyph", text: KIT_SLOTS[ i ], hittest: "false" } );
			var key = $.CreatePanel( "Panel", c, "", { class: "kit-key", hittest: "false" } );
			$.CreatePanel( "Label", key, "", { text: String( i + 1 ), hittest: "false" } );
			$.CreatePanel( "Label", c, "", { class: "kit-name", text: name || "none", hittest: "false" } );
		} );
		$( "#StageInitial" ).text = heroName( id ).charAt( 0 );
		$( "#StageName" ).text = heroName( id );
		var hp = $( "#StageHp" );
		hp.RemoveAndDeleteChildren();
		for ( var n = 0; n < ( HP[ id ] || 4 ); n++ )
			$.CreatePanel( "Panel", hp, "", { class: "hp-pip", hittest: "false" } );
	}

	function moveCursor( i, snd )
	{
		if ( !heroIds.length )
			return;
		i = ( i + heroIds.length ) % heroIds.length;
		if ( i !== cursor && snd !== false )
			sound( SND_NAV );
		cursor = i;
		render();
	}

	function myLock() { return f && f.me >= 0 ? num( f.arena.lock, f.me ) : 0; }

	function pick( i )
	{
		if ( !f || f.arena.state !== "SELECT" || myLock() )
			return;
		cursor = i;
		picked = heroIds[ i ];
		flow( "pick:" + picked );
		sound( SND_NAV );
		render();
	}

	function lock()
	{
		if ( !f || f.arena.state !== "SELECT" || myLock() )
			return;
		if ( !picked || picked !== heroIds[ cursor ] )
			picked = heroIds[ cursor ];
		if ( !picked )
			return;
		flow( "lock:" + picked );
		sound( SND_OK );
	}

	function selectScreen( st )
	{
		var me = f.me, opp = me === 0 ? 1 : 0;
		var locks = [ num( f.arena.lock, 0 ), num( f.arena.lock, 1 ) ];
		var allLocked = locks[ 0 ] && locks[ 1 ];
		var revealed = allLocked && f.arena.hero1 && f.arena.hero2;
		show( "#Select", !revealed );
		show( "#Reveal", !!revealed );
		if ( revealed )
		{
			$( "#RevealHero1" ).text = heroName( f.arena.hero1 );
			$( "#RevealHero2" ).text = heroName( f.arena.hero2 );
			$( "#RevealName1" ).text = names[ 0 ];
			$( "#RevealName2" ).text = names[ 1 ];
			$( "#RevealGame" ).text = "Game " + st.game + "  -  first to " + st.firstTo;
			return;
		}
		buildGrid();
		var counter = st.game > 1 && st.counterSide > 0;
		var mySide = me + 1;
		var canPick = me >= 0 && !locks[ me ];
		if ( !picked && f.myhero )
			picked = f.myhero;
		$( "#SelectTitle" ).text = counter ? ( st.counterSide === mySide ? "Counterpick" : "Opponent may counterpick" ) :
			( st.runbacks && st.game === 1 ? "Runback - choose your fighter" : "Choose your fighter" );
		$( "#SelectSub" ).text = counter ?
			( st.counterSide === mySide ? "You lost the last game: keep your hero or change it" : "You won the last game: your hero stays" ) :
			"Blind pick: your opponent sees your hero when you both lock in";
		$( "#Select" ).SetHasClass( "is-locked", !canPick );
		if ( V2 )
			$( "#SelectMode" ).text = "CASUAL DUEL  -  FIRST TO " + st.firstTo + "  -  GAME " + Math.max( 1, st.game ) +
				( st.runbacks ? "  -  RUNBACK " + st.runbacks : "" );
		$( "#LockLabel" ).text = canPick ? "LOCK IN" : "LOCKED";
		$( "#LockBtn" ).enabled = canPick;
		var t = parseInt( f.arena.deadline, 10 );
		$( "#SelectTimer" ).text = isNaN( t ) ? "" : String( t );
		$( "#SelectTimer" ).SetHasClass( "low", !isNaN( t ) && t <= 5 );
		var opps = me < 0 ? "" : locks[ opp ] ? names[ opp ] + " is locked in" : names[ opp ] + " is choosing...";
		if ( me >= 0 && locks[ me ] )
			opps = ( locks[ opp ] ? "Both locked in" : "Locked in - waiting for " + names[ opp ] );
		$( "#OppStatus" ).text = opps;
		$( "#OppStatus" ).SetHasClass( "ready", !!locks[ opp ] );
		paintGrid( !canPick, canPick );
	}

	// --- between games ------------------------------------------------------------------

	function gameEndScreen( st )
	{
		var winner = ( parseInt( f.arena.winner, 10 ) || 0 ) - 1; // arena_winner: slot + 1, 0 none
		var log = words( f.arena.set_log );
		var last = log.length ? log[ log.length - 1 ] : "";
		var title = winner >= 0 && names[ winner ] ? names[ winner ] + " wins game " + st.game : "Game " + st.game + " over";
		if ( f.me >= 0 && winner === f.me )
			title = "You win game " + st.game;
		$( "#GameEndTitle" ).text = title;
		var sub = "Rounds " + last + "   -   set " + st.wins[ 0 ] + " - " + st.wins[ 1 ];
		var decided = st.wins[ 0 ] >= st.firstTo || st.wins[ 1 ] >= st.firstTo;
		if ( !decided && f.me >= 0 )
			sub += winner === f.me ? "   -   opponent may change hero" : "   -   you may change hero";
		$( "#GameEndSub" ).text = sub;
	}

	// --- set end -----------------------------------------------------------------------

	function paintVotes( chosen )
	{
		for ( var i = 0; i < 3; i++ )
		{
			$( "#Vote" + i ).SetHasClass( "is-selected", i === voteCursor && !chosen );
			$( "#Vote" + i ).SetHasClass( "is-chosen", chosen === i + 1 );
			$( "#Vote" + i ).enabled = !chosen;
		}
	}

	function setEndScreen( st )
	{
		var me = f.me, opp = me === 0 ? 1 : 0;
		var mySide = me + 1;
		var winner = st.winnerSide;
		var title = winner ? names[ winner - 1 ] + " wins the set" : "Set over";
		if ( me >= 0 && winner === mySide )
			title = "You win the set";
		else if ( me >= 0 && winner )
			title = "You lose the set";
		$( "#SetEndTitle" ).text = title;
		$( "#SetEndTitle" ).SetHasClass( "win", me >= 0 && winner === mySide );
		$( "#SetEndScore" ).text = st.wins[ 0 ] + "  -  " + st.wins[ 1 ];
		var log = words( f.arena.set_log );
		$( "#SetEndLog" ).text = log.length ? "Games: " + log.join( "   " ) : "";
		var left = words( f.arena.set_left );
		$( "#SetEndForfeit" ).text = st.result === 2 && left.length ?
			left.slice( 0, -1 ).join( " " ) + " left" + ( left[ left.length - 1 ] === "behind" ? " while behind" : "" ) + " - forfeit win" : "";
		var t = parseInt( f.arena.deadline, 10 );
		$( "#SetTimer" ).text = isNaN( t ) ? "" : String( t );
		$( "#SetTimer" ).SetHasClass( "low", !isNaN( t ) && t <= 5 );
		var forfeit = st.result === 2;
		show( "#VoteRow", !forfeit );
		var mine = me >= 0 ? num( f.arena.vote, me ) : 0;
		var theirs = me >= 0 ? num( f.arena.vote, opp ) : 0;
		paintVotes( mine );
		$( "#MyVote" ).text = forfeit ? "Leaving the set..." : mine ? "You chose: " + VOTE_TEXT[ mine ] : "";
		var ot = forfeit ? "" : !theirs ? names[ opp ] + " is deciding..." :
			theirs === 3 ? names[ opp ] + " is leaving" : names[ opp ] + " wants a rematch" + ( theirs === 2 ? " (new hero)" : "" );
		$( "#OppVote" ).text = ot;
		$( "#OppVote" ).SetHasClass( "yes", theirs === 1 || theirs === 2 );
		$( "#AfterSearch" ).SetHasClass( "is-on", f.after === "search" );
		$( "#AfterMenu" ).SetHasClass( "is-on", f.after !== "search" );
	}

	function vote( i )
	{
		if ( !f || f.arena.state !== "SET END" || f.me < 0 || num( f.arena.vote, f.me ) )
			return;
		voteCursor = i;
		flow( VOTE_CMDS[ i ] );
		sound( SND_OK );
	}

	// --- ribbon (searching while training) ------------------------------------------------

	function fmtTime( s )
	{
		s = Math.max( 0, Math.floor( s ) );
		var m = Math.floor( s / 60 ), r = s % 60;
		return m + ":" + ( r < 10 ? "0" : "" ) + r;
	}

	var searchStart = 0;
	function ribbon()
	{
		var live = f.live || {};
		var found = f.match_age < 8 && !f.setgame;
		var queued = live.queue && !f.setgame;
		show( "#Ribbon", !!( queued || found ) );
		$( "#Ribbon" ).SetHasClass( "found", !!found );
		if ( found )
		{
			$( "#RibbonText" ).text = "Match found - get ready";
			$( "#RibbonHint" ).text = "";
		}
		else if ( queued )
		{
			if ( !searchStart )
				searchStart = Date.now();
			$( "#RibbonText" ).text = "Searching " + ( live.queue.playlist === "duel" ? "casual duel" : live.queue.playlist ) +
				"  " + fmtTime( ( Date.now() - searchStart ) / 1000 );
			$( "#RibbonHint" ).text = "Esc - menu to cancel";
		}
		if ( !queued )
			searchStart = 0;
	}

	// --- leaving ---------------------------------------------------------------------------

	function leavingScreen()
	{
		$( "#LeavingTitle" ).text = f.notice || "The set is over";
		$( "#LeavingSub" ).text = f.after === "search" ? "Searching again..." : "Back to the online menu...";
	}

	// --- frame -----------------------------------------------------------------------------

	function render()
	{
		if ( !f )
			return;
		var st = set();
		var s = f.arena.state;
		if ( f.arena.name1 ) names[ 0 ] = f.arena.name1;
		if ( f.arena.name2 ) names[ 1 ] = f.arena.name2;
		var ids = words( f.arena.heroes );
		if ( ids.length )
			heroIds = ids;
		if ( s !== lastState || st.game !== lastGame )
		{
			if ( s === "SELECT" )
			{
				picked = f.myhero || "";
				var at = heroIds.indexOf( picked );
				cursor = at >= 0 ? at : cursor;
			}
			if ( s === "SET END" )
				voteCursor = 0;
			lastState = s;
			lastGame = st.game;
		}
		strip( st );
		var leaving = f.leaving;
		show( "#Leaving", leaving );
		var inSet = f.setgame && !leaving;
		show( "#Select", false );
		show( "#Reveal", false );
		if ( inSet && s === "SELECT" )
			selectScreen( st );
		show( "#GameEnd", inSet && s === "MATCH END" );
		if ( inSet && s === "MATCH END" )
			gameEndScreen( st );
		show( "#SetEnd", inSet && s === "SET END" );
		if ( inSet && s === "SET END" )
			setEndScreen( st );
		if ( leaving )
			leavingScreen();
		ribbon();
	}

	function poll()
	{
		var raw = q( "getflow" );
		try { f = JSON.parse( raw || "null" ); } catch ( e ) { f = null; }
		render();
		$.Schedule( 0.1, poll );
	}

	Bulwark.On( "key", function ( e )
	{
		if ( !e || !e.down || !f )
			return;
		var k = e.key, s = f.arena.state;
		if ( s === "SELECT" && !myLock() )
		{
			var cols = V2 ? 0 : 4; // the v2 roster is one row
			if ( k === "ArrowRight" || k === "d" || k === "D" ) moveCursor( cursor + 1 );
			else if ( k === "ArrowLeft" || k === "a" || k === "A" ) moveCursor( cursor - 1 );
			else if ( cols && ( k === "ArrowDown" || k === "s" || k === "S" ) ) moveCursor( cursor + cols );
			else if ( cols && ( k === "ArrowUp" || k === "w" || k === "W" ) ) moveCursor( cursor - cols );
			else if ( k === "Enter" || k === " " ) { pick( cursor ); lock(); }
		}
		else if ( s === "SET END" )
		{
			if ( k === "ArrowRight" || k === "d" || k === "D" ) { voteCursor = Math.min( 2, voteCursor + 1 ); sound( SND_NAV ); render(); }
			else if ( k === "ArrowLeft" || k === "a" || k === "A" ) { voteCursor = Math.max( 0, voteCursor - 1 ); sound( SND_NAV ); render(); }
			else if ( k === "Enter" || k === " " ) vote( voteCursor );
			else if ( k === "1" || k === "2" || k === "3" ) vote( parseInt( k, 10 ) - 1 );
		}
	} );

	poll();

	return {
		Lock: lock,
		Vote: vote,
		VoteHover: function ( i ) { voteCursor = i; render(); },
		After: function ( a ) { flow( "after:" + a ); sound( SND_NAV ); }
	};
} )();
