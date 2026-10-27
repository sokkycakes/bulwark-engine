// Lobby page: the logic of data/web/lobby-menu/src (main.jsx, model.js,
// bridge.js, navigation.js and the components) on Panorama panels. It polls
// getlobby every 250 ms, rebuilds only what changed, and keys player rows by
// seat and chat rows by message id.
var LobbyMenu = (function ()
{
	// --- bridge.js: the only code that speaks fte_query ---
	function query( request )
	{
		if ( typeof fte_query !== "function" )
			return null;
		try { return fte_query( request ); } catch ( e ) { return null; }
	}

	function accepted( reply )
	{
		if ( reply === "ok" )
			return true;
		try { return JSON.parse( reply ).ok === true; } catch ( e ) { return false; }
	}

	function readLobby()
	{
		var raw = query( "getlobby" );
		if ( !raw )
			throw new Error( "Waiting for the lobby connection…" );
		try
		{
			var snapshot = JSON.parse( raw );
			if ( snapshot.uiVersion )
				snapshot.chat = JSON.parse( query( "getlobbychat" ) || "[]" );
			return snapshot;
		}
		catch ( e ) { throw new Error( "Unable to read the lobby." ); }
	}

	function action( name ) { return accepted( query( "lobby_action:" + name ) ); }

	function leave()
	{
		if ( !action( "close" ) )
			return false;
		if ( !accepted( query( "cbuf:menu_webcore_title" ) ) )
			query( "cbuf:menu_webcore" );
		return true;
	}

	var MENU_SOUNDS = { navigate: "misc/menu1", confirm: "misc/menu2", back: "misc/menu3" };
	function menuSound( kind )
	{
		if ( MENU_SOUNDS[ kind ] )
			query( "localsound:" + MENU_SOUNDS[ kind ] );
	}

	// UTF-8 hex keeps user text out of console syntax (quotes, semicolons, newlines)
	function encodeChat( text )
	{
		var bytes = unescape( encodeURIComponent( text ) );
		if ( bytes.length > 240 )
			throw new Error( "Message is too long (240 UTF-8 bytes maximum)." );
		var out = "";
		for ( var i = 0; i < bytes.length; i++ )
			out += ( "0" + bytes.charCodeAt( i ).toString( 16 ) ).slice( -2 );
		return out;
	}

	// --- model.js ---
	var MAPS = [
		{ id: "envtest", name: "Env Test" }, { id: "lean", name: "Lean Test" },
		{ id: "arch_testbed", name: "Arch Testbed" }, { id: "arch_crux_test", name: "Arch Crux Test" }
	];
	var RULESETS = [
		{ id: "DUEL", name: "Duel" }, { id: "DEATHMATCH", name: "Deathmatch" },
		{ id: "TEAMDM", name: "Team Deathmatch" }, { id: "DOMINATION", name: "Domination" }
	];

	function normalize( data )
	{
		if ( !data || typeof data !== "object" )
			throw new Error( "Invalid lobby state." );
		var players = ( Array.isArray( data.players ) ? data.players : [] ).filter( Boolean ).map( function ( p, i )
		{
			if ( typeof p === "object" )
				return p;
			var ready = p.indexOf( "[READY]" ) === 0;
			var name = ready ? p.slice( 7 ).trim() : p;
			return { seat: i, name: name, ready: ready, connected: true, host: name === data.host };
		} );
		var settings = data.settings || {};
		var s = {};
		for ( var k in data )
			s[ k ] = data[ k ];
		s.players = players;
		s.phase = String( data.phase || "" ).toUpperCase();
		s.sessionId = data.sessionId || "";
		s.room = data.roomCode || data.room || "";
		s.localSeat = data.localSeat == null ? -1 : data.localSeat;
		s.max = data.max || 16;
		s.map = data.map || "";
		s.ruleset = settings.ruleset || data.ruleset || "";
		s.serverType = settings.servertype || data.serverType || "LISTEN SERVER";
		s.timelimit = settings.timelimit == null ? "" : settings.timelimit;
		s.fraglimit = settings.fraglimit == null ? "" : settings.fraglimit;
		s.canLeave = data.canLeave !== false;
		s.canConfigure = !!( data.active && data.isHost && data.canChangeMap );
		s.canChat = !!data.canChat;
		s.chat = Array.isArray( data.chat ) ? data.chat : [];
		s.network = data.network || data.transport || "";
		s.receivedAt = Date.now();
		return s;
	}

	function localPlayer( s )
	{
		for ( var i = 0; i < s.players.length; i++ )
			if ( s.players[ i ].seat === s.localSeat )
				return s.players[ i ];
		return undefined;
	}

	function countdown( s )
	{
		if ( s.phase !== "COUNTDOWN" )
			return "";
		var now = ( s.serverTime || 0 ) + ( Date.now() - s.receivedAt ) / 1000;
		var seconds = Math.max( 0, Math.ceil( ( s.countdownEnd || 0 ) - now ) );
		return seconds ? "Starting in " + seconds + "…" : "Starting…";
	}

	function mapTitle( id )
	{
		for ( var i = 0; i < MAPS.length; i++ )
			if ( MAPS[ i ].id === id )
				return MAPS[ i ].name;
		return id || "Waiting…";
	}

	// --- page state (main.jsx) ---
	var nav = $( "#LobbyMenuNav" );
	var snapshot = normalize( { active: false, players: [] } );
	var notice = "";
	var connectionError = "";
	var dialog = "";
	var caller = null;
	var revision = "";
	var lastPoll = "";
	var failedSince = 0;
	var focused = null; // the panel with keyboard focus, from onfocus/onblur
	var menuHover = false;
	var selected = 0;
	var selectedRef = -1;
	var focusKey = "";
	var sessionKey = "";
	var lastChatId = 0;
	var limitTime = "0";
	var limitFrags = "0";

	function setText( panel, text )
	{
		text = String( text );
		if ( panel && panel.text !== text )
			panel.text = text;
	}

	function setEnabled( panel, on )
	{
		if ( panel && panel.enabled !== on )
			panel.enabled = on;
	}

	// --- controls: click, focus and blur go through one table ---
	var clicks = {};

	function bindControl( panel, onClick )
	{
		clicks[ panel.id ] = onClick;
		panel.SetPanelEvent( "onfocus", function () { focused = panel; Focused( panel ); } );
		panel.SetPanelEvent( "onblur", function () { if ( focused && focused.id === panel.id ) focused = null; } );
		if ( onClick )
			panel.SetPanelEvent( "onactivate", function () { Click( panel ); } );
	}

	function Click( panel )
	{
		if ( !panel || !panel.enabled || !clicks[ panel.id ] )
			return;
		clicks[ panel.id ]();
	}

	function isEntry( panel )
	{
		return !!panel && ( panel.id === "ChatInput" || panel.id === "TimeLimit" || panel.id === "FragLimit" );
	}

	function focus( panel )
	{
		if ( panel && panel.enabled )
			panel.SetFocus();
	}

	// --- refresh ---
	function refresh()
	{
		try
		{
			var raw = readLobby();
			failedSince = 0;
			connectionError = "";
			var token = ( raw.sessionId || "" ) + ":" + raw.revision;
			if ( revision !== token )
			{
				revision = token;
				notice = "";
			}
			// serverTime changes on every call; only the countdown needs it
			var copy = {};
			for ( var k in raw )
				copy[ k ] = raw[ k ];
			copy.serverTime = raw.phase === "COUNTDOWN" ? Math.floor( raw.serverTime ) : 0;
			var key = JSON.stringify( copy );
			if ( lastPoll !== key )
			{
				lastPoll = key;
				snapshot = normalize( raw );
			}
		}
		catch ( e )
		{
			// keep the last good state; report sustained failures only
			if ( !failedSince )
				failedSince = Date.now();
			if ( Date.now() - failedSince >= 2000 )
				connectionError = e.message;
		}
		render();
	}

	function poll()
	{
		refresh();
		$.Schedule( 0.25, poll );
	}

	function run( name )
	{
		if ( !action( name ) )
		{
			notice = "That lobby action is unavailable.";
			render();
			return false;
		}
		notice = "Updating lobby…";
		$.Schedule( 0.05, refresh );
		render();
		return true;
	}

	function toggleReady()
	{
		var p = localPlayer( snapshot );
		return run( p && p.ready ? "unready" : "ready" );
	}

	// --- dialogs ---
	function OpenDialog( kind )
	{
		if ( kind !== "leave" && !snapshot.isHost )
			return;
		caller = focused;
		dialog = kind;
		if ( kind === "options" )
		{
			limitTime = String( snapshot.timelimit === "" ? 0 : snapshot.timelimit );
			limitFrags = String( snapshot.fraglimit === "" ? 0 : snapshot.fraglimit );
			$( "#TimeLimit" ).text = limitTime;
			$( "#FragLimit" ).text = limitFrags;
		}
		render();
		if ( kind === "leave" )
			focus( $( "#BtnCancelLeave" ) );
		else
		{
			var list = controls();
			if ( list.length )
				focus( list[ 0 ] );
		}
	}

	function CloseDialog( playBack )
	{
		if ( !dialog )
			return;
		if ( playBack !== false )
			menuSound( "back" );
		dialog = "";
		render();
		if ( caller && caller.IsValid() && caller.enabled )
			focus( caller );
		caller = null;
	}

	function choose( name )
	{
		if ( run( name ) )
			CloseDialog( false );
	}

	function limitsValid()
	{
		return /^\d{1,3}$/.test( limitTime ) && Number( limitTime ) <= 180 && /^\d{1,3}$/.test( limitFrags );
	}

	// --- lobby list (LobbyMenu.jsx) ---
	var ROWS = [ "BtnLobbyOptions", "BtnChangeRuleset", "BtnChangeMap", "BtnServerType", "BtnStart" ];

	function select( index )
	{
		if ( selectedRef === index )
			return;
		if ( selectedRef >= 0 )
			menuSound( "navigate" );
		selectedRef = index;
		selected = index;
		renderMenu();
	}

	function rowAction( index )
	{
		if ( index === 0 ) return function () { OpenDialog( "options" ); };
		if ( index === 1 ) return function () { OpenDialog( "ruleset" ); };
		if ( index === 2 ) return function () { OpenDialog( "map" ); };
		if ( index === 3 ) return function () { OpenDialog( "server" ); };
		return function ()
		{
			if ( snapshot.isHost )
				run( snapshot.canCancel ? "cancel" : "start" );
			else
				toggleReady();
		};
	}

	function rowIndex( panel )
	{
		return panel ? ROWS.indexOf( panel.id ) : -1;
	}

	function menuItems()
	{
		var out = [];
		for ( var i = 0; i < ROWS.length; i++ )
		{
			var p = $( "#" + ROWS[ i ] );
			if ( p.enabled )
				out.push( p );
		}
		return out;
	}

	function RowHover( index )
	{
		var p = $( "#" + ROWS[ index ] );
		if ( !dialog && p.enabled )
			focus( p );
	}

	function MenuHover( on ) { menuHover = on; }

	// --- the controls the keyboard visits, in document order ---
	var playerSlots = {};   // seat -> { slot, plate, name, meta }
	var playerOrder = [];
	var chatRows = {};      // message id -> row panel
	var chatOrder = [];

	function controls()
	{
		var ids = [];
		var out = [];
		if ( dialog === "leave" )
			ids = [ "BtnConfirmLeave", "BtnCancelLeave" ];
		else if ( dialog === "map" || dialog === "ruleset" )
		{
			var list = dialog === "map" ? MAPS : RULESETS;
			for ( var i = 0; i < list.length; i++ )
				ids.push( choiceId( dialog, list[ i ].id ) );
			ids.push( "BtnSettingsBack" );
		}
		else if ( dialog === "server" )
			ids = [ "BtnListenServer", "BtnDedicatedServer", "BtnSettingsBack" ];
		else if ( dialog === "options" )
			ids = [ "BtnOptionsReady", "TimeLimit", "FragLimit", "BtnApplyLimits", "BtnSettingsBack" ];
		else
		{
			ids = ROWS.slice();
			ids.push( "BtnCopyRoomCode" );
			for ( var j = 0; j < playerOrder.length; j++ )
				ids.push( "Nameplate" + playerOrder[ j ] );
			ids.push( "ChatInput", "BtnSendChat", "BtnLeaveLobby" );
		}
		for ( var n = 0; n < ids.length; n++ )
		{
			var p = $( "#" + ids[ n ] );
			if ( p && p.enabled && p.visible )
				out.push( p );
		}
		return out;
	}

	function indexOf( list, panel )
	{
		if ( !panel )
			return -1;
		for ( var i = 0; i < list.length; i++ )
			if ( list[ i ].id === panel.id )
				return i;
		return -1;
	}

	// navigation.js: arrows, W/S and Tab move focus; Esc opens or closes Leave
	function onKey( e )
	{
		if ( !e || !e.down )
			return;
		var key = e.key;
		if ( key === "Escape" )
		{
			if ( dialog )
				CloseDialog( true );
			else
				OpenDialog( "leave" );
			return;
		}
		if ( key === "WheelUp" || key === "WheelDown" )
		{
			if ( dialog || !menuHover )
				return;
			var rows = menuItems();
			if ( !rows.length )
				return;
			var cur = indexOf( rows, focused );
			focus( rows[ ( cur + ( key === "WheelDown" ? 1 : -1 ) + rows.length ) % rows.length ] );
			return;
		}
		// text fields keep their editing keys
		if ( isEntry( focused ) && key !== "Tab" )
			return;
		if ( key === " " )
		{
			// Enter is the engine's: it activates the focused control itself
			if ( focused )
				Click( focused );
			return;
		}
		var backward = [ "ArrowUp", "ArrowLeft", "w", "W" ].indexOf( key ) !== -1;
		var forward = [ "ArrowDown", "ArrowRight", "s", "S", "Tab" ].indexOf( key ) !== -1;
		if ( !backward && !forward )
			return;
		var inMenu = !dialog && rowIndex( focused ) >= 0;
		var all = inMenu && key !== "Tab" ? menuItems() : controls();
		if ( !all.length )
			return;
		var index = indexOf( all, focused );
		focus( all[ ( index + ( backward ? -1 : 1 ) + all.length ) % all.length ] );
	}

	function Focused( panel )
	{
		var i = rowIndex( panel );
		if ( i >= 0 )
			select( i );
	}

	// --- rendering ---
	function renderMenu()
	{
		var s = snapshot;
		var guestLocked = s.active && !s.isHost;
		var current = guestLocked ? 4 : selected;
		for ( var i = 0; i < 5; i++ )
		{
			nav.SetHasClass( "sel-" + i, i === current );
			$( "#" + ROWS[ i ] ).SetHasClass( "is-selected", i === current );
		}
	}

	function renderPlayers()
	{
		var s = snapshot;
		var list = $( "#PlayersList" );
		var seen = {};
		var order = [];
		for ( var i = 0; i < s.players.length; i++ )
		{
			var p = s.players[ i ];
			var seat = p.seat;
			seen[ seat ] = true;
			order.push( seat );
			var row = playerSlots[ seat ];
			if ( !row )
			{
				var slot = $.CreatePanel( "Panel", list, "PlayerSlot" + seat, { class: "player-slot" } );
				var plate = $.CreatePanel( "Button", slot, "Nameplate" + seat, { class: "ui-button nameplate" } );
				plate.SetAttributeString( "tabindex", "-1" );
				var name = $.CreatePanel( "Label", plate, "", { class: "slot-name", text: "" } );
				var meta = $.CreatePanel( "Label", plate, "", { class: "slot-meta", text: "" } );
				bindControl( plate, toggleReady );
				row = playerSlots[ seat ] = { slot: slot, plate: plate, name: name, meta: meta };
			}
			setText( row.name, p.name || "Player" );
			setText( row.meta, p.ready ? "READY" : p.connected === false ? "…" : "" );
			row.plate.SetHasClass( "is-ready", !!p.ready );
			setEnabled( row.plate, seat === s.localSeat && !!s.canReady );
		}
		for ( var k in playerSlots )
		{
			if ( !seen[ k ] )
			{
				if ( focused && focused.id === playerSlots[ k ].plate.id )
					focused = null;
				playerSlots[ k ].slot.visible = false;
				playerSlots[ k ].slot.DeleteAsync( 0 );
				delete playerSlots[ k ];
			}
		}
		// keep the rows in seat order
		for ( var n = order.length - 1; n > 0; n-- )
			list.MoveChildBefore( playerSlots[ order[ n - 1 ] ].slot, playerSlots[ order[ n ] ].slot );
		playerOrder = order;
	}

	function renderChat()
	{
		var s = snapshot;
		var box = $( "#ChatMessages" );
		var seen = {};
		var last = 0;
		for ( var i = 0; i < s.chat.length; i++ )
		{
			var m = s.chat[ i ];
			seen[ m.id ] = true;
			last = m.id;
			if ( chatRows[ m.id ] )
				continue;
			// name and text are separate labels: text is never read as markup
			var row = $.CreatePanel( "Panel", box, "ChatLine" + m.id, { class: "chat-message" } );
			$.CreatePanel( "Label", row, "", { class: "chat-name", text: ( m.name || "Player" ) + ": " } );
			$.CreatePanel( "Label", row, "", { class: "chat-text", text: String( m.text ) } );
			chatRows[ m.id ] = row;
		}
		for ( var k in chatRows )
		{
			if ( !seen[ k ] )
			{
				chatRows[ k ].visible = false;
				chatRows[ k ].DeleteAsync( 0 );
				delete chatRows[ k ];
			}
		}
		if ( last !== lastChatId )
		{
			lastChatId = last;
			$.Schedule( 0.0, function () { box.ScrollToBottom(); } );
		}
	}

	function renderDialogs()
	{
		var s = snapshot;
		var local = localPlayer( s );
		var settings = $( "#SettingsDialog" );
		var open = dialog !== "" && dialog !== "leave" && s.isHost;
		settings.SetHasClass( "is-open", open );
		[ "map", "ruleset", "server", "options" ].forEach( function ( kind )
		{
			settings.SetHasClass( "kind-" + kind, dialog === kind );
		} );
		setText( $( "#SettingsTitle" ), { map: "Choose map", ruleset: "Choose ruleset", server: "Server type", options: "Lobby options" }[ dialog ] || "" );
		for ( var i = 0; i < MAPS.length; i++ )
		{
			var b = $( "#" + choiceId( "map", MAPS[ i ].id ) );
			setEnabled( b, s.canConfigure );
			b.SetHasClass( "is-pressed", s.map === MAPS[ i ].id );
		}
		for ( var j = 0; j < RULESETS.length; j++ )
		{
			var r = $( "#" + choiceId( "ruleset", RULESETS[ j ].id ) );
			setEnabled( r, s.canConfigure );
			r.SetHasClass( "is-pressed", s.ruleset === RULESETS[ j ].id );
		}
		setText( $( "#SessionDetails" ), ( s.network || "Local" ) + " lobby · " + s.players.length + " / " + s.max + " players" );
		setEnabled( $( "#BtnOptionsReady" ), !!s.canReady );
		setText( $( "#OptionsReadyLabel" ), local && local.ready ? "Unready" : "Ready" );
		var limitsOn = s.canConfigure && !!s.uiVersion;
		setEnabled( $( "#TimeLimit" ), limitsOn );
		setEnabled( $( "#FragLimit" ), limitsOn );
		$( "#BtnApplyLimits" ).visible = !!s.isHost;
		setEnabled( $( "#BtnApplyLimits" ), limitsOn && limitsValid() );

		$( "#LeaveConfirm" ).SetHasClass( "is-open", dialog === "leave" );
		setText( $( "#LeaveText" ), s.isHost ? "Leaving closes your lobby for everyone." : "You will return to the title menu." );
	}

	function render()
	{
		var s = snapshot;
		var local = localPlayer( s );
		var counting = countdown( s );
		var guestLocked = s.active && !s.isHost;

		// losing host authority closes any settings dialog
		if ( !s.isHost && dialog && dialog !== "leave" )
			dialog = "";

		// a new session starts with an empty draft
		if ( s.sessionId !== sessionKey )
		{
			sessionKey = s.sessionId;
			$( "#ChatInput" ).text = "";
		}

		// MapPreview
		setText( $( "#LblSummaryLine0" ), s.ruleset || "Waiting for settings" );
		setText( $( "#LblSummaryLine1" ), s.timelimit !== "" ? ( Number( s.timelimit ) ? s.timelimit + " minute time limit" : "No time limit" ) : "" );
		setText( $( "#LblSummaryLine2" ), s.fraglimit !== "" ? ( Number( s.fraglimit ) ? s.fraglimit + " frag limit" : "No frag limit" ) : "" );
		setText( $( "#ImgLevelImage" ), mapTitle( s.map ) );

		// LobbyMenu
		nav.SetHasClass( "lobby-active", !!s.active );
		nav.SetHasClass( "guest-locked", guestLocked );
		setEnabled( $( "#BtnLobbyOptions" ), !!( s.active && s.isHost ) );
		setEnabled( $( "#BtnChangeRuleset" ), s.canConfigure && !!s.uiVersion );
		setEnabled( $( "#BtnChangeMap" ), s.canConfigure );
		setEnabled( $( "#BtnServerType" ), !!( s.active && s.isHost ) );
		setText( $( "#RulesetValue" ), s.ruleset || "—" );
		setText( $( "#MapValue" ), mapTitle( s.map ) );
		setText( $( "#ServerTypeValue" ), s.serverType );
		var start = $( "#BtnStart" );
		if ( s.isHost )
		{
			setEnabled( start, !!( s.canCancel || s.canStart ) );
			setText( $( "#StartLabel" ), s.canCancel ? ( counting || "Ready check" ) + " · Cancel"
				: s.phase === "STARTING" ? "Starting…" : "Start game" );
		}
		else
		{
			setEnabled( start, !!s.canReady );
			setText( $( "#StartLabel" ), counting || ( local && local.ready ? "Unready" : "Ready" ) );
		}
		renderMenu();

		// PlayerRoster
		var copy = $( "#BtnCopyRoomCode" );
		setEnabled( copy, !!s.room );
		copy.SetHasClass( "room-code-long", s.room.length > 4 );
		setText( $( "#RoomCodeValue" ), s.room || "—" );
		setText( $( "#PlayerCount" ), s.players.length );
		setText( $( "#PlayerMax" ), s.max );
		renderPlayers();

		// ChatPanel
		var input = $( "#ChatInput" );
		setEnabled( input, s.canChat );
		setEnabled( $( "#BtnSendChat" ), s.canChat && input.text.trim() !== "" );
		renderChat();

		// footer and status
		setEnabled( $( "#BtnLeaveLobby" ), s.canLeave );
		var status = $( "#StatusLine" );
		setText( status, s.error || connectionError || notice || ( s.phase === "JOINING" || s.phase === "CLOSING" ? s.status : "" ) );
		status.SetHasClass( "error", !!s.error );

		renderDialogs();

		// main.jsx: put focus on the first control when the lobby's capabilities change
		var key = [ s.active, s.isHost, s.canChangeMap, s.canReady, s.canStart, s.canCancel ].join( "," );
		if ( key !== focusKey )
		{
			focusKey = key;
			if ( s.active && ( !focused || !focused.IsValid() || !focused.enabled ) )
				focus( $( s.isHost ? "#BtnLobbyOptions" : "#BtnStart" ) );
		}
	}

	// --- chat ---
	function SendChat()
	{
		var input = $( "#ChatInput" );
		var text = input.text.trim();
		if ( !text || !snapshot.canChat )
			return;
		try
		{
			if ( !action( "chat:" + encodeChat( text ) ) )
			{
				notice = "Unable to send your message.";
				render();
				return;
			}
			input.text = "";
			$.Schedule( 0.05, refresh );
		}
		catch ( e ) { notice = e.message; }
		render();
	}

	function DraftChanged()
	{
		setEnabled( $( "#BtnSendChat" ), snapshot.canChat && $( "#ChatInput" ).text.trim() !== "" );
	}

	function LimitsChanged()
	{
		limitTime = $( "#TimeLimit" ).text;
		limitFrags = $( "#FragLimit" ).text;
		setEnabled( $( "#BtnApplyLimits" ), snapshot.canConfigure && !!snapshot.uiVersion && limitsValid() );
	}

	function ApplyLimits()
	{
		if ( run( "setlimits:" + Number( limitTime ) + ":" + Number( limitFrags ) ) )
			CloseDialog( false );
	}

	function CopyCode()
	{
		notice = accepted( query( "clipboard_copy:" + snapshot.room ) ) ? "Room code copied." : "Unable to copy the room code.";
		render();
	}

	function Leave()
	{
		if ( !leave() )
		{
			notice = "Unable to leave the lobby.";
			render();
		}
	}

	function choiceId( kind, id )
	{
		return ( kind === "map" ? "MapChoice_" : "RulesetChoice_" ) + id;
	}

	// --- set-up ---
	function buildChoices( kind, list, parentId )
	{
		var parent = $( "#" + parentId );
		list.forEach( function ( item )
		{
			var b = $.CreatePanel( "Button", parent, choiceId( kind, item.id ), { class: "ui-button choice" } );
			b.SetAttributeString( "tabindex", "-1" );
			$.CreatePanel( "Label", b, "", { text: item.name } );
			bindControl( b, function () { choose( ( kind === "map" ? "setmap:" : "setruleset:" ) + item.id ); } );
		} );
	}

	function init()
	{
		buildChoices( "map", MAPS, "MapChoices" );
		buildChoices( "ruleset", RULESETS, "RulesetChoices" );
		for ( var i = 0; i < ROWS.length; i++ )
		{
			( function ( index )
			{
				var p = $( "#" + ROWS[ index ] );
				bindControl( p, function () { menuSound( "confirm" ); rowAction( index )(); } );
				p.SetPanelEvent( "onmouseover", function () { RowHover( index ); } );
			} )( i );
		}
		bindControl( $( "#BtnCopyRoomCode" ), CopyCode );
		bindControl( $( "#ChatInput" ), null );
		bindControl( $( "#BtnSendChat" ), SendChat );
		bindControl( $( "#BtnLeaveLobby" ), function () { OpenDialog( "leave" ); } );
		bindControl( $( "#BtnListenServer" ), function () { CloseDialog( true ); } );
		bindControl( $( "#BtnDedicatedServer" ), null );
		bindControl( $( "#BtnOptionsReady" ), toggleReady );
		bindControl( $( "#TimeLimit" ), null );
		bindControl( $( "#FragLimit" ), null );
		bindControl( $( "#BtnApplyLimits" ), ApplyLimits );
		bindControl( $( "#BtnSettingsBack" ), function () { CloseDialog( true ); } );
		bindControl( $( "#BtnConfirmLeave" ), Leave );
		bindControl( $( "#BtnCancelLeave" ), function () { CloseDialog( true ); } );
		Bulwark.On( "key", onKey );
		poll();
	}

	init();

	return {
		OpenDialog: OpenDialog,
		CloseDialog: CloseDialog,
		MenuHover: MenuHover,
		SendChat: SendChat,
		DraftChanged: DraftChanged,
		LimitsChanged: LimitsChanged,
		State: function () { return { dialog: dialog, focused: focused ? focused.id : "", selected: selected, snapshot: snapshot }; }
	};
})();
