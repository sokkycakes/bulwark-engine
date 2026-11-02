// WolfTeam-style team match header (layout/hud/wt-scoreboard.xml).
//
// WtScore.Set( fields ) updates any subset of:
//   mode        string   mode name ("Deathmatch")
//   goal        number   points to win (160); goalUnit string ("Points")
//   red, blue   number   team scores; the lead arrow follows them
//   time        number   seconds left in the round (shown mm :ss)
//   kills, deaths            the local player's counts (left edge)
//   watchName, watchKills, watchDeaths
//                            a watched player (right edge); watchName ""
//                            hides that block
// A HUD progs posts the same fields as JSON to the view it opened:
//   ui_post( view, "state", "{\"red\":28,\"blue\":26}" );
// or from the console: ui_post <view> state {"red":28} / ui_eval <view> WtScore.Set({red:28})
// (ui_list shows the view number). The demo layout,
// layout/hud/wt-scoreboard-demo.xml, runs WtScore.Demo() by itself.

// Two layouts share this script: wt-scoreboard.xml (fonts, ids Wt*) and
// wt-scoreboard-original.xml (WolfTeam's digit strips, ids Wto*). label()
// writes to whichever exists: a Label gets text, a .WtoNum box gets one
// .Dg panel per digit (background-position picks the digit, like
// WolfTeam's "Number" widget).
var WtScore = ( function ()
{
	function digits( box, text )
	{
		var s = String( text );
		var kids = box.Children();
		while ( kids.length > s.length )
		{
			kids[ kids.length - 1 ].DeleteAsync( 0 );
			kids.pop();
		}
		while ( kids.length < s.length )
			kids.push( $.CreatePanel( 'Panel', box, '', { 'class': 'Dg', hittest: 'false' } ) );
		for ( var i = 0; i < s.length; i++ )
		{
			var c = s.charAt( i );
			for ( var d = 0; d < 10; d++ )
				kids[ i ].SetHasClass( 'd' + d, c === String( d ) );
			kids[ i ].SetHasClass( 'Blank', c < '0' || c > '9' );
		}
	}

	function label( id, text )
	{
		var p = $( '#' + id );
		if ( p )
			p.text = String( text );
		var o = $( '#Wto' + id.substring( 2 ) );
		if ( o )
		{
			if ( o.BHasClass( 'WtoNum' ) )
				digits( o, String( text ).replace( /^\//, '' ) );
			else
				o.text = String( text );
		}
	}

	function pad2( n )
	{
		return n < 10 ? '0' + n : String( n );
	}

	var state = { red: 0, blue: 0 };

	function Set( f )
	{
		if ( typeof f === 'string' )
			f = JSON.parse( f );
		if ( !f )
			return;

		if ( f.mode !== undefined ) label( 'WtMode', f.mode );
		if ( f.goal !== undefined ) label( 'WtGoal', f.goal );
		if ( f.goalUnit !== undefined ) label( 'WtGoalUnit', f.goalUnit );

		if ( f.red !== undefined ) { state.red = f.red | 0; label( 'WtRed', state.red ); }
		if ( f.blue !== undefined ) { state.blue = f.blue | 0; label( 'WtBlue', state.blue ); }
		var lead = $( '#WtLead' ) || $( '#WtoSign' );
		if ( lead )
		{
			lead.SetHasClass( 'LeadRed', state.red > state.blue );
			lead.SetHasClass( 'LeadBlue', state.blue > state.red );
			lead.SetHasClass( 'LeadNone', state.red === state.blue );
		}

		if ( f.time !== undefined )
		{
			var t = Math.max( 0, Math.floor( f.time ) );
			label( 'WtMin', pad2( Math.floor( t / 60 ) ) );
			label( 'WtSec', pad2( t % 60 ) );
		}

		if ( f.kills !== undefined ) label( 'WtSelfKills', f.kills );
		if ( f.deaths !== undefined ) label( 'WtSelfDeaths', '/' + f.deaths );

		if ( f.watchName !== undefined )
		{
			label( 'WtWatchName', f.watchName );
			var w = $( '#WtWatch' ) || $( '#WtoWatch' );
			if ( w )
				w.SetHasClass( 'Hidden', f.watchName === '' );
		}
		if ( f.watchKills !== undefined ) label( 'WtWatchKills', f.watchKills );
		if ( f.watchDeaths !== undefined ) label( 'WtWatchDeaths', '/' + f.watchDeaths );
	}

	// A round clock that runs by itself, for testing without a game:
	// WtScore.Demo() counts down from 35:52 and trades points now and then.
	function Demo()
	{
		var left = 35 * 60 + 52;
		Set( { mode: 'Deathmatch', goal: 160, red: 28, blue: 26, time: left,
			kills: 4, deaths: 8, watchName: 'VIPCaslese', watchKills: 12, watchDeaths: 4 } );
		function tick()
		{
			left = left > 0 ? left - 1 : 35 * 60 + 52;
			var f = { time: left };
			if ( left % 7 === 0 )
				f[ ( left % 2 ) ? 'red' : 'blue' ] = ( ( left % 2 ) ? state.red : state.blue ) + 1;
			Set( f );
			$.Schedule( 1.0, tick );
		}
		$.Schedule( 1.0, tick );
	}

	return { Set: Set, Demo: Demo };
} )();

if ( typeof Bulwark !== 'undefined' && Bulwark.On )
	Bulwark.On( 'state', function ( v ) { WtScore.Set( v ); } );

( function ()
{
	var screen = $( '#WtScreen' ) || $( '#WtoScreen' );
	if ( screen && screen.BHasClass( 'WtDemo' ) )
		WtScore.Demo();
} )();
