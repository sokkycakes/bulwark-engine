// The in-game set view (layout/web/setflow.xml). Everything it shows comes
// from fte_query("getflow"): the arena director's serverinfo keys, this
// player's slot and Live's state. Choices go back as fte_query("flow:...").
// With several players on this screen (f.local, Local Play) each one picks
// a hero and votes on a card over their own view, with their own
// controller: a key event's "seat" says whose it is, and their choices go
// back as fte_query("flow:p<n>:...").
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
		if ( f.local )
		{
			$( "#StripName1" ).text = slotTag( 0 ) + names[ 0 ];
			$( "#StripName2" ).text = names[ 1 ] + slotTag( 1, true );
		}
		else
		{
			$( "#StripName1" ).text = names[ 0 ] + ( f.me === 0 ? "  (you)" : "" );
			$( "#StripName2" ).text = ( f.me === 1 ? "(you)  " : "" ) + names[ 1 ];
		}
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
		if ( f.me >= 0 && winner === f.me && !f.local )
			title = "You win game " + st.game;
		$( "#GameEndTitle" ).text = title;
		var sub = "Rounds " + last + "   -   set " + st.wins[ 0 ] + " - " + st.wins[ 1 ];
		var decided = st.wins[ 0 ] >= st.firstTo || st.wins[ 1 ] >= st.firstTo;
		if ( !decided && f.local && winner >= 0 )
			sub += "   -   " + names[ 1 - winner ] + " may change hero";
		else if ( !decided && f.me >= 0 )
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
		$( "#LeavingSub" ).text = f.local || f.localplay ? "Back to Local Play..." :
			f.after === "search" ? "Searching again..." : "Back to the online menu...";
	}

	// --- several players on this screen (Local Play) --------------------------------------

	var lc = {};        // per seat: { cursor, picked, vote }
	var layerKey = "";  // what the cards were built for
	var cards = {};     // seat -> { panel, tiles[], ... }

	function isRevealed()
	{
		return num( f.arena.lock, 0 ) && num( f.arena.lock, 1 ) && f.arena.hero1 && f.arena.hero2;
	}

	function seatOfSlot( slot )
	{
		var seats = f.seats || [];
		for ( var i = 0; i < seats.length; i++ )
			if ( seats[ i ].slot === slot )
				return seats[ i ];
		return null;
	}

	function slotTag( slot, after )
	{
		var se = seatOfSlot( slot );
		if ( !se )
			return "";
		return after ? "  P" + ( se.seat + 1 ) : "P" + ( se.seat + 1 ) + "  ";
	}

	// a free for all on this screen: each player picks a hero once, when it starts
	function pickMode() { return !!( f && f.pick && !f.setgame ); }

	function seatInfo( seat )
	{
		var seats = f.seats || [];
		for ( var i = 0; i < seats.length; i++ )
			if ( seats[ i ].seat === seat )
				return seats[ i ];
		return null;
	}

	function lstate( seat )
	{
		if ( !lc[ seat ] )
			lc[ seat ] = { cursor: 0, picked: "", vote: 0 };
		return lc[ seat ];
	}

	function localReset( s )
	{
		for ( var k in lc )
		{
			if ( s === "SELECT" )
			{
				var se = seatInfo( parseInt( k, 10 ) );
				lc[ k ].picked = se ? se.hero || "" : "";
				var at = heroIds.indexOf( lc[ k ].picked );
				if ( at >= 0 )
					lc[ k ].cursor = at;
			}
			if ( s === "SET END" )
				lc[ k ].vote = 0;
		}
	}

	function seatFlow( seat, verb ) { return flow( "p" + ( seat + 1 ) + ":" + verb ); }

	function pct( v ) { return ( v * 100 ).toFixed( 3 ) + "%"; }

	// a card over the seat's view, inset a little; the top row of views
	// leaves room for the set strip
	function cardStyle( r )
	{
		var top = r[ 1 ] < 0.01 ? 0.085 : 0.012;
		return "x: " + pct( r[ 0 ] + 0.012 ) + "; y: " + pct( r[ 1 ] + top ) + "; width: " + pct( r[ 2 ] - 0.024 ) +
			"; height: " + pct( r[ 3 ] - top - 0.012 ) + ";";
	}

	function cardHead( panel, se )
	{
		var head = $.CreatePanel( "Panel", panel, "", { class: "lcard-head", hittest: "false" } );
		var tag = $.CreatePanel( "Panel", head, "", { class: "lcard-tag", hittest: "false" } );
		$.CreatePanel( "Label", tag, "", { text: "P" + ( se.seat + 1 ), hittest: "false" } );
		var c = { panel: panel };
		c.name = $.CreatePanel( "Label", head, "", { class: "lcard-name", text: names[ se.slot ] || "", hittest: "false" } );
		c.timer = $.CreatePanel( "Label", head, "", { class: "lcard-timer", text: "", hittest: "false" } );
		return c;
	}

	function buildSelectCard( layer, se )
	{
		var panel = $.CreatePanel( "Panel", layer, "", { class: "lcard", style: cardStyle( se.rect ) } );
		var c = cardHead( panel, se );
		c.tiles = [];
		// 4 x 2 tiles between the head (13%) and the foot (from 80%)
		heroIds.forEach( function ( id, i )
		{
			var col = i % 4, row = Math.floor( i / 4 );
			var t = $.CreatePanel( "Button", panel, "", { class: "ltile", style:
				"x: " + ( 3 + col * 23.75 ) + "%; y: " + ( 15 + row * 32 ) + "%; width: 22.5%; height: 30%;" } );
			$.CreatePanel( "Label", t, "", { class: "ltile-name", text: heroName( id ), hittest: "false" } );
			$.CreatePanel( "Label", t, "", { class: "ltile-role", text: heroRole( id ), hittest: "false" } );
			t.SetPanelEvent( "onactivate", function () { localPick( se.seat, i, true ); } );
			c.tiles.push( t );
		} );
		c.pick = $.CreatePanel( "Label", panel, "", { class: "lcard-pick", style: "y: 82%;", text: "", hittest: "false" } );
		c.status = $.CreatePanel( "Label", panel, "", { class: "lcard-status", style: "y: 84%;", text: "", hittest: "false" } );
		return c;
	}

	function buildVoteCard( layer, se )
	{
		var panel = $.CreatePanel( "Panel", layer, "", { class: "lcard", style: cardStyle( se.rect ) } );
		var c = cardHead( panel, se );
		c.result = $.CreatePanel( "Label", panel, "", { class: "lcard-result", style: "y: 17%;", text: "", hittest: "false" } );
		c.sub = $.CreatePanel( "Label", panel, "", { class: "lcard-sub", style: "y: 36%;", text: "", hittest: "false" } );
		c.votes = [];
		[ "REMATCH", "NEW HERO", "LEAVE" ].forEach( function ( label, i )
		{
			var b = $.CreatePanel( "Button", panel, "", { class: "btn lvote", style:
				"x: " + ( 4 + i * 31 ) + "%; y: 50%; width: 30%; height: 20%;" } );
			$.CreatePanel( "Label", b, "", { text: label, hittest: "false" } );
			b.SetPanelEvent( "onactivate", function () { localVote( se.seat, i ); } );
			c.votes.push( b );
		} );
		c.status = $.CreatePanel( "Label", panel, "", { class: "lcard-sub", style: "y: 75%;", text: "", hittest: "false" } );
		$.CreatePanel( "Label", panel, "", { class: "lcard-hint", style: "y: 88%;",
			text: "Left / Right and A or Enter", hittest: "false" } );
		return c;
	}

	function timerText( c )
	{
		var t = parseInt( f.arena.deadline, 10 );
		c.timer.text = isNaN( t ) ? "" : String( t );
		c.timer.SetHasClass( "low", !isNaN( t ) && t <= 5 );
	}

	function paintSelectCard( c, se, st )
	{
		var ls = lstate( se.seat );
		var ffa = pickMode();
		var locked = ffa ? !!se.done : !!num( f.arena.lock, se.slot );
		var counter = !ffa && st.game > 1 && st.counterSide > 0;
		var canPick = ( ffa || se.slot >= 0 ) && !locked;
		if ( !ls.picked && se.hero )
			ls.picked = se.hero;
		c.name.text = ffa ? se.name || "P" + ( se.seat + 1 ) : names[ se.slot ] || "";
		if ( ffa )
			c.timer.text = "";
		else
			timerText( c );
		c.tiles.forEach( function ( t, i )
		{
			var id = heroIds[ i ];
			t.SetHasClass( "is-cursor", i === ls.cursor && canPick );
			t.SetHasClass( "is-picked", id === ls.picked );
			t.SetHasClass( "is-off", !canPick && id !== ls.picked );
		} );
		c.pick.text = heroName( canPick ? heroIds[ ls.cursor ] : ls.picked ) || "-";
		c.panel.SetHasClass( "is-done", locked );
		if ( locked )
			c.status.text = ffa ? "Locked in - waiting for the others" :
				counter && st.counterSide !== se.slot + 1 ? "Hero stays" : "Locked in";
		else if ( counter && st.counterSide === se.slot + 1 )
			c.status.text = "Counterpick: A / Enter";
		else
			c.status.text = "A / Enter: lock in";
	}

	function paintVoteCard( c, se, st )
	{
		var ls = lstate( se.seat );
		var mine = num( f.arena.vote, se.slot );
		var other = num( f.arena.vote, 1 - se.slot );
		var forfeit = st.result === 2;
		var won = st.winnerSide === se.slot + 1;
		c.name.text = names[ se.slot ] || "";
		timerText( c );
		c.result.text = st.winnerSide ? ( won ? "You win the set" : "You lose the set" ) : "Set over";
		c.result.SetHasClass( "win", won );
		var log = words( f.arena.set_log );
		c.sub.text = st.wins[ 0 ] + " - " + st.wins[ 1 ] + ( log.length ? "   -   games " + log.join( "  " ) : "" );
		c.votes.forEach( function ( b, i )
		{
			b.SetHasClass( "is-selected", i === ls.vote && !mine );
			b.SetHasClass( "is-chosen", mine === i + 1 );
			b.enabled = !mine && !forfeit;
		} );
		c.panel.SetHasClass( "is-done", !!mine );
		var otherName = names[ 1 - se.slot ] || "";
		var ot = forfeit ? "Leaving the set..." : mine ? "You chose: " + VOTE_TEXT[ mine ] : "";
		if ( !forfeit && other )
			ot += ( ot ? "   -   " : "" ) + ( other === 3 ? otherName + " is leaving" :
				otherName + " wants a rematch" + ( other === 2 ? " (new hero)" : "" ) );
		c.status.text = ot;
	}

	function localLayer( kind, st )
	{
		var layer = $( "#LocalLayer" );
		var seats = ( f && f.local && f.seats ) || [];
		show( "#LocalLayer", !!kind && seats.length > 0 );
		var key = kind ? kind + "|" + heroIds.join( "," ) + "|" + seats.map( function ( se )
		{
			return se.seat + ":" + se.slot + ":" + se.rect.join( "," );
		} ).join( ";" ) : "";
		if ( key !== layerKey )
		{
			layerKey = key;
			layer.RemoveAndDeleteChildren();
			cards = {};
			seats.forEach( function ( se )
			{
				if ( !kind || ( se.slot < 0 && !pickMode() ) )
					return;
				cards[ se.seat ] = kind === "select" ? buildSelectCard( layer, se ) : buildVoteCard( layer, se );
			} );
		}
		seats.forEach( function ( se )
		{
			var c = cards[ se.seat ];
			if ( !c )
				return;
			if ( kind === "select" )
				paintSelectCard( c, se, st );
			else if ( kind === "vote" )
				paintVoteCard( c, se, st );
		} );
	}

	function localPick( seat, i, andLock )
	{
		var se = seatInfo( seat );
		if ( !se || !heroIds[ i ] )
			return;
		if ( pickMode() ? se.done : ( se.slot < 0 || f.arena.state !== "SELECT" || num( f.arena.lock, se.slot ) ) )
			return;
		var ls = lstate( seat );
		ls.cursor = i;
		ls.picked = heroIds[ i ];
		if ( pickMode() && !andLock )
		{
			sound( SND_NAV );
			render();
			return;
		}
		seatFlow( seat, ( andLock ? "lock:" : "pick:" ) + ls.picked );
		sound( andLock ? SND_OK : SND_NAV );
		render();
	}

	function localVote( seat, i )
	{
		var se = seatInfo( seat );
		if ( !se || se.slot < 0 || f.arena.state !== "SET END" || num( f.arena.vote, se.slot ) )
			return;
		lstate( seat ).vote = i;
		seatFlow( seat, VOTE_CMDS[ i ] );
		sound( SND_OK );
		render();
	}

	function localKey( e )
	{
		var seat = typeof e.seat === "number" && e.seat >= 0 ? e.seat : 0;
		var se = seatInfo( seat );
		if ( !se || ( se.slot < 0 && !pickMode() ) )
			return;
		var k = e.key || "", b = String( e.bind || "" ).toUpperCase(), s = f.arena.state;
		var left = k === "ArrowLeft" || k === "a" || k === "A" || b === "GP_DPAD_LEFT";
		var right = k === "ArrowRight" || k === "d" || k === "D" || b === "GP_DPAD_RIGHT";
		var up = k === "ArrowUp" || k === "w" || k === "W" || b === "GP_DPAD_UP";
		var down = k === "ArrowDown" || k === "s" || k === "S" || b === "GP_DPAD_DOWN";
		var ok = k === "Enter" || k === " " || b === "GP_A";
		var ls = lstate( seat );
		var selecting = pickMode() ? !se.done : s === "SELECT" && !num( f.arena.lock, se.slot );
		if ( selecting && heroIds.length )
		{
			var n = heroIds.length, c = ls.cursor;
			if ( right ) c = ( c + 1 ) % n;
			else if ( left ) c = ( c + n - 1 ) % n;
			else if ( down ) c = ( c + 4 ) % n;
			else if ( up ) c = ( c + n - 4 ) % n;
			else if ( ok ) { localPick( seat, ls.cursor, true ); return; }
			else return;
			if ( c !== ls.cursor )
			{
				ls.cursor = c;
				ls.picked = heroIds[ c ];
				if ( !pickMode() )
					seatFlow( seat, "pick:" + ls.picked );
				sound( SND_NAV );
				render();
			}
		}
		else if ( s === "SET END" && !num( f.arena.vote, se.slot ) )
		{
			if ( right ) { ls.vote = Math.min( 2, ls.vote + 1 ); sound( SND_NAV ); render(); }
			else if ( left ) { ls.vote = Math.max( 0, ls.vote - 1 ); sound( SND_NAV ); render(); }
			else if ( ok ) localVote( seat, ls.vote );
			else if ( k === "1" || k === "2" || k === "3" ) localVote( seat, parseInt( k, 10 ) - 1 );
		}
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
		else if ( !heroIds.length )
			heroIds = Object.keys( HEROES );
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
			localReset( s );
			lastState = s;
			lastGame = st.game;
		}
		strip( st );
		var leaving = f.leaving;
		show( "#Leaving", leaving );
		var inSet = f.setgame && !leaving;
		show( "#Select", false );
		show( "#Reveal", false );
		var localCards = "";
		if ( inSet && s === "SELECT" )
		{
			selectScreen( st );
			if ( f.local && !isRevealed() )
			{
				show( "#Select", false );
				localCards = "select";
			}
		}
		if ( pickMode() && !leaving )
			localCards = "select";
		show( "#GameEnd", inSet && s === "MATCH END" );
		if ( inSet && s === "MATCH END" )
			gameEndScreen( st );
		show( "#SetEnd", inSet && s === "SET END" && !f.local );
		if ( inSet && s === "SET END" )
		{
			if ( f.local )
				localCards = "vote";
			else
				setEndScreen( st );
		}
		localLayer( localCards, st );
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
		if ( f.local )
		{
			localKey( e );
			return;
		}
		var k = e.key, s = f.arena.state;
		if ( s === "SELECT" && !myLock() )
		{
			var cols = 4;
			if ( k === "ArrowRight" || k === "d" || k === "D" ) moveCursor( cursor + 1 );
			else if ( k === "ArrowLeft" || k === "a" || k === "A" ) moveCursor( cursor - 1 );
			else if ( k === "ArrowDown" || k === "s" || k === "S" ) moveCursor( cursor + cols );
			else if ( k === "ArrowUp" || k === "w" || k === "W" ) moveCursor( cursor - cols );
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
