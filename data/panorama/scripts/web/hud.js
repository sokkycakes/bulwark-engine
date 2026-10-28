// Panorama port of Stiletto's data/web/hud/app.js. The state handling, the
// bridge polling (gethud, falling back to getstats, plus cvar:/get_cvar for
// Drive, clip, reserve, reload and cooldowns) and the health shake follow the
// original; drawing is replaced by setting labels, classes and transforms on
// the panels of hud.xml, and only when a value changes.
'use strict';

var StilettoHud = (function ()
{
	var ASSETS = window.HUD_ASSETS || {};
	var DESIGN_HEIGHT = 1080;

	var HudState = {
		matchTimeSeconds: 0,
		playerOne: { name: 'P1', score: 0 },
		playerTwo: { name: 'P2', score: 0 },
		ammoCurrent: 0,
		ammoMax: 0,
		health: 100,
		healthMax: 100,
		healthVisualState: 'normal',
		resourceCount: 0,
		resourceAtMax: false,
		abilityCharges: 3,
		abilityChargeMax: 3,
		reloadSeconds: 0,
		speed: 0,
		cooldowns: []
	};

	// Godot hp.gd: shake_intensity 10, shake_duration 0.5; red while the timer is above 80%
	var HP_SHAKE_INTENSITY = 10;
	var HP_SHAKE_DURATION = 0.5;
	var hpShakeTimer = 0;
	var hpShakeOx = 0;
	var hpShakeOy = 0;
	var hpShakeFlashRed = false;
	var prevHudHealth = null;
	var lastTick = 0;

	function startHpShake()
	{
		hpShakeTimer = HP_SHAKE_DURATION;
	}

	function updateHpShake(now)
	{
		if (!lastTick)
			lastTick = now;
		var dt = Math.min(0.1, (now - lastTick) / 1000);
		lastTick = now;
		if (hpShakeTimer > 0)
			hpShakeTimer -= dt;
		if (hpShakeTimer <= 0)
		{
			hpShakeTimer = 0;
			hpShakeOx = 0;
			hpShakeOy = 0;
			hpShakeFlashRed = false;
			return;
		}
		var intensity = HP_SHAKE_INTENSITY * hpShakeTimer / HP_SHAKE_DURATION;
		hpShakeOx = (Math.random() * 2 - 1) * intensity;
		hpShakeOy = (Math.random() * 2 - 1) * intensity;
		hpShakeFlashRed = hpShakeTimer > HP_SHAKE_DURATION * 0.8;
	}

	// --- engine bridge (as the original) ---

	function fteQuery(cmd)
	{
		if (typeof fte_query !== 'function')
			return null;
		try { return fte_query(cmd); } catch (e) { return null; }
	}

	function cvarNum(name, fallback)
	{
		var raw = fteQuery('cvar:' + name);
		if (raw === '' || raw == null)
			raw = fteQuery('get_cvar ' + name);
		var n = parseFloat(raw);
		return isNaN(n) ? fallback : n;
	}

	function queryHud()
	{
		var raw = fteQuery('gethud');
		if (!raw)
		{
			raw = fteQuery('getstats');
			if (!raw)
				return null;
			try
			{
				var stats = JSON.parse(raw);
				return { health: typeof stats[0] === 'number' ? stats[0] : 100, ammo: typeof stats[3] === 'number' ? stats[3] : 0, ammoCurrent: 0, ammoMax: 0, timer: '00:00', speed: 0, charges: 0, scoreLeft: 0, scoreRight: 0, player1: '', player2: '' };
			}
			catch (e) { return null; }
		}
		try { return JSON.parse(raw); } catch (e) { return null; }
	}

	function applyHudData(data)
	{
		if (!data)
			return;
		if (typeof data.health === 'number')
		{
			if (prevHudHealth != null && data.health < prevHudHealth)
				startHpShake();
			HudState.health = data.health;
			prevHudHealth = data.health;
		}
		if (typeof data.healthMax === 'number') HudState.healthMax = Math.max(1, data.healthMax);
		if (typeof data.ammoCurrent === 'number') HudState.ammoCurrent = Math.max(0, Math.round(data.ammoCurrent));
		if (typeof data.ammoMax === 'number') HudState.ammoMax = Math.max(0, Math.round(data.ammoMax));
		else if (typeof data.ammo === 'number') HudState.ammoMax = Math.max(0, Math.round(data.ammo));
		if (typeof data.charges === 'number') HudState.abilityCharges = Math.max(0, Math.min(HudState.abilityChargeMax, Math.round(data.charges)));
		if (data.timer)
		{
			var parts = String(data.timer).split(':');
			HudState.matchTimeSeconds = parseInt(parts[0]) * 60 + parseInt(parts[1]);
		}
		if (typeof data.speed === 'number') HudState.speed = data.speed;
		if (data.scoreLeft != null) HudState.playerOne.score = data.scoreLeft;
		if (data.scoreRight != null) HudState.playerTwo.score = data.scoreRight;
		if (data.exLevel != null) HudState.resourceCount = data.exLevel;
		if (data.reload != null) HudState.reloadSeconds = Math.max(0, Number(data.reload) || 0);
		if (data.player1 != null) HudState.playerOne.name = data.player1;
		if (data.player2 != null) HudState.playerTwo.name = data.player2;
		var hpRound = Math.round(HudState.health);
		if (hpRound <= 0) HudState.healthVisualState = 'dead';
		else if (hpRound === 1) HudState.healthVisualState = 'low';
		else HudState.healthVisualState = 'normal';
	}

	// the engine may push the same object instead of being polled
	window.WebCoreHud_Receive = function (data) { applyHudData(data); };

	function refreshState()
	{
		applyHudData(queryHud());

		var drive = cvarNum('webcore_hud_drive', -1);
		var driveMax = Math.max(1, cvarNum('webcore_hud_drive_max', 1000));
		if (drive >= 0)
			HudState.resourceAtMax = drive >= driveMax;

		var clip = cvarNum('webcore_hud_clip', -1);
		var reserve = cvarNum('webcore_hud_ammo_max', -1);
		if (clip >= 0) HudState.ammoCurrent = Math.round(clip);
		if (reserve >= 0) HudState.ammoMax = Math.round(reserve);

		HudState.reloadSeconds = Math.max(0, cvarNum('webcore_hud_reload', HudState.reloadSeconds));

		HudState.cooldowns = [];
		for (var i = 0; i < 3; i++)
		{
			var cd = cvarNum('webcore_hud_cooldown_' + i, -1);
			if (cd >= 0)
				HudState.cooldowns.push({ value: cd, ready: cd <= 0 });
		}
	}

	// --- art: the page's own image pack (assets-data.js), as the canvas drew it ---

	// the file each pack entry was made from, if the pack is missing
	var ASSET_FILES = {
		hudring: 'hudring.png', timeIndicator: 'time_indicator.svg', hpFull: 'hp_vector.svg', hpLow: 'hpLow.png',
		hpDead: 'hp_dead_vector.svg', exIndicator: 'ex_indicator.svg', abilityPip: 'abilitypip1.png',
		union: 'union.svg', circlesExt1: 'circles_ext1.svg', deployedCode: 'deployed_code.svg', deblur: 'deblur.svg'
	};

	function loadArt(panel)
	{
		var key = panel.GetAttributeString('data-asset', '');
		if (key)
			panel.SetImage(ASSETS[key] || 'file://{game}/data/web/hud/assets/' + ASSET_FILES[key]);
		var kids = panel.Children();
		for (var i = 0; i < kids.length; i++)
			loadArt(kids[i]);
	}

	// The canvas scales everything by viewHeight / 1080: centred clusters stay
	// centred on the view, the resource panel stays on its left edge.
	function fitView()
	{
		if (typeof Bulwark === 'undefined' || !Bulwark.Height)
			return;
		var h = Bulwark.Height();
		if (!(h > 0) || shown.viewH === h)
			return;
		shown.viewH = h;
		var k = (h / DESIGN_HEIGHT).toFixed(4);
		var t = Math.abs(h - DESIGN_HEIGHT) < 0.5 ? 'none' : 'scale( ' + k + ', ' + k + ' )';
		el.Stage.style.transform = t;
		el.Left.style.transform = t;
	}

	// --- per-frame update: touch panels only when their value changes ---

	var el = {};
	var shown = {};

	function setText(key, value)
	{
		if (shown[key] === value)
			return;
		shown[key] = value;
		el[key].text = value;
	}

	function setClass(panel, key, cls, on)
	{
		if (shown[key] === on)
			return;
		shown[key] = on;
		panel.SetHasClass(cls, on);
	}

	function pad2(n)
	{
		return (n < 10 ? '0' : '') + n;
	}

	function render()
	{
		var s = HudState;

		var speedStr = String(Math.min(Math.max(0, Math.floor(s.speed)), 999));
		while (speedStr.length < 3)
			speedStr = '0' + speedStr;
		setText('Speed', speedStr);

		var t = isNaN(s.matchTimeSeconds) ? 0 : s.matchTimeSeconds;
		setText('Timer', pad2(Math.floor(t / 60)) + ':' + pad2(t % 60));
		setText('ScoreLeft', String(s.playerOne.score));
		setText('ScoreRight', String(s.playerTwo.score));
		setText('NameLeft', String(s.playerOne.name));
		setText('NameRight', String(s.playerTwo.name));

		// health icon: state art, damage shake, red flash, blue at full health
		var hud = el.Hud;
		var state = s.healthVisualState;
		setClass(hud, 'hpNormal', 'hp-normal', state === 'normal');
		setClass(hud, 'hpLow', 'hp-low', state === 'low');
		setClass(hud, 'hpDead', 'hp-dead', state === 'dead');
		var full = state === 'normal' && Math.round(s.health) > 0 && Math.round(s.health) >= s.healthMax;
		setClass(el.HpBox, 'hpRed', 'hp-red', hpShakeFlashRed);
		setClass(el.HpBox, 'hpBlue', 'hp-blue', full && hpShakeTimer <= 0);
		var shake = 'translate3d( ' + hpShakeOx.toFixed(1) + 'px, ' + hpShakeOy.toFixed(1) + 'px, 0px )';
		if (shown.shake !== shake)
		{
			shown.shake = shake;
			el.HpBox.style.transform = shake;
		}

		setText('Clip', Math.round(s.ammoCurrent) + '|');
		setText('Reserve', String(Math.round(s.ammoMax)));
		var pipCount = Math.min(s.abilityCharges, s.abilityChargeMax);
		for (var i = 0; i < 3; i++)
			setClass(el['Pip' + i], 'pip' + i, 'on', i < pipCount);
		setClass(hud, 'reloading', 'reloading', s.reloadSeconds > 0);
		if (s.reloadSeconds > 0)
			setText('Reload', s.reloadSeconds.toFixed(1));

		setText('ExCount', String(s.resourceCount));
		setClass(hud, 'driveMax', 'drive-max', !!s.resourceAtMax);

		for (var c = 0; c < 3; c++)
		{
			var cd = s.cooldowns[c];
			setClass(el['Cooldown' + c], 'cdShown' + c, 'shown', !!cd);
			setClass(el['Cooldown' + c], 'cdReady' + c, 'ready', !!cd && cd.ready);
			var label = '';
			if (cd && !cd.ready && cd.value > 0)
				label = cd.value < 10 ? cd.value.toFixed(1) : String(Math.round(cd.value));
			setClass(el['CdTime' + c], 'cdTimeShown' + c, 'shown', label !== '');
			setText('CdTime' + c, label);
		}
	}

	function tick()
	{
		fitView();
		refreshState();
		updateHpShake(Date.now());
		render();
		$.Schedule(1 / 60, tick);
	}

	function init()
	{
		['Hud', 'HpBox', 'Speed', 'Timer', 'ScoreLeft', 'ScoreRight', 'NameLeft', 'NameRight', 'Clip', 'Reserve',
			'Reload', 'ExCount', 'Pip0', 'Pip1', 'Pip2', 'Cooldown0', 'Cooldown1', 'Cooldown2', 'CdTime0', 'CdTime1',
			'CdTime2', 'Stage', 'Left'].forEach(function (id) { el[id] = $('#' + id); });

		loadArt(el.Hud);

		tick();
	}

	init();
	return { State: HudState, Receive: applyHudData };
})();
