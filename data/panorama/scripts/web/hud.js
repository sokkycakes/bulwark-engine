// Panorama port of Stiletto's data/web/hud/app.js. The state handling, the
// bridge polling (gethud, falling back to getstats, plus cvar:/get_cvar for
// Drive, clip, reserve, reload and cooldowns) and the health shake follow the
// original; drawing is replaced by setting labels, classes and transforms on
// the panels of hud.xml, and only when a value changes.
'use strict';

var StilettoHud = (function ()
{
	var ASSETS = window.HUD_ASSETS || {};

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

	// --- static art the canvas drew from vector images ---

	function shape(parent, cls, css)
	{
		return $.CreatePanel('Panel', parent, '', { class: cls, style: css });
	}

	function px(v)
	{
		return v.toFixed(2) + 'px';
	}

	// The Drive gauge (assets/union.svg, 482 x 31.6): two yellow bars tapering
	// from round ends to a pinch in the middle, with a 5 px black rim. Each
	// tapered half is a centre strip plus two strips rotated onto its top and
	// bottom edges.
	function buildGauge(parent)
	{
		var cy = 15.8, ends = [15.79, 466.21], mid = 241;
		var layers = [
			{ cls: 'gauge-ink', end: 15.79, waist: 9, band: 12 },
			{ cls: 'gauge-fill', end: 10.79, waist: 4, band: 7.5 }
		];
		layers.forEach(function (l)
		{
			ends.forEach(function (x0)
			{
				var dir = x0 < mid ? 1 : -1;
				var len = Math.abs(mid - x0);
				var ang = Math.atan2(l.end - l.waist, len) * 180 / Math.PI;
				var left = Math.min(x0, mid);
				// centre strip, as tall as the waist
				shape(parent, l.cls, 'x: ' + px(left) + '; y: ' + px(cy - l.waist) + '; width: ' + px(len) +
					'; height: ' + px(l.waist * 2) + ';');
				// top and bottom edges, from the round end towards the middle
				var reach = len + 1;
				var topY = cy - l.end, botY = cy + l.end - l.band;
				var xs = dir > 0 ? x0 : x0 - reach;
				var origin = dir > 0 ? '0%' : '100%';
				shape(parent, l.cls, 'x: ' + px(xs) + '; y: ' + px(topY) + '; width: ' + px(reach) + '; height: ' + px(l.band) +
					'; transform-origin: ' + origin + ' 0%; transform: rotate( ' + (dir * ang).toFixed(3) + 'deg );');
				shape(parent, l.cls, 'x: ' + px(xs) + '; y: ' + px(botY) + '; width: ' + px(reach) + '; height: ' + px(l.band) +
					'; transform-origin: ' + origin + ' 100%; transform: rotate( ' + (-dir * ang).toFixed(3) + 'deg );');
				// round end
				shape(parent, l.cls + ' gauge-cap', 'x: ' + px(x0 - l.end) + '; y: ' + px(cy - l.end) + '; width: ' + px(l.end * 2) +
					'; height: ' + px(l.end * 2) + ';');
			});
		});
	}

	// Cooldown icons (Material symbols circles_ext, deployed_code, deblur),
	// drawn from their SVG geometry into the 20 px slot.
	function circle(parent, sx, sy, cx, cy, r)
	{
		shape(parent, 'cd-dot', 'x: ' + px((cx - r) * sx) + '; y: ' + px((cy - r) * sy) + '; width: ' + px(2 * r * sx) +
			'; height: ' + px(2 * r * sy) + '; border-radius: 50%;');
	}

	function buildCirclesIcon(parent)
	{
		var sx = 20 / 41.667, sy = 20 / 39.583;
		circle(parent, sx, sy, 8.333, 31.25, 8.333);
		circle(parent, sx, sy, 33.333, 31.25, 8.333);
		circle(parent, sx, sy, 20.833, 22.917, 3.958);
		circle(parent, sx, sy, 20.833, 8.333, 8.333);
	}

	function buildCubeIcon(parent)
	{
		var sx = 20 / 37.5, sy = 20 / 41.667;
		var w = 37.5 * sx, tip = 10.83 * sy;
		var side = w / Math.SQRT2;
		var squash = (2 * tip) / w;
		[tip, 20 - tip].forEach(function (cy)
		{
			shape(parent, 'cd-solid', 'x: ' + px((w - side) / 2) + '; y: ' + px(cy - side / 2) + '; width: ' + px(side) +
				'; height: ' + px(side) + '; transform: scale( 1, ' + squash.toFixed(4) + ' ) rotate( 45deg );');
		});
		shape(parent, 'cd-solid', 'x: 0px; y: ' + px(tip) + '; width: ' + px(w) + '; height: ' + px(20 - 2 * tip) + ';');
		// the "Y" fold lines of the cube
		var cx = w / 2, cy = 21.6 * sy, t = 1.6;
		shape(parent, 'cd-cut', 'x: ' + px(cx - t / 2) + '; y: ' + px(cy) + '; width: ' + px(t) + '; height: ' + px(34.5 * sy - cy) + ';');
		[-1, 1].forEach(function (d)
		{
			shape(parent, 'cd-cut', 'x: ' + px(d < 0 ? cx - 7.6 : cx) + '; y: ' + px(cy - t / 2) + '; width: 7.6px; height: ' + px(t) +
				'; transform-origin: ' + (d < 0 ? '100%' : '0%') + ' 50%; transform: rotate( ' + (d * -30) + 'deg );');
		});
	}

	function buildDeblurIcon(parent)
	{
		var sx = 20 / 38.542, sy = 20 / 39.583;
		// right half of a disc
		var half = shape(parent, 'cd-clip', 'x: ' + px(19.27 * sx) + '; y: 0px; width: ' + px(19.27 * sx) + '; height: 20px;');
		shape(half, 'cd-dot', 'x: ' + px(-19.27 * sx) + '; y: ' + px(0.5 * sy) + '; width: ' + px(38.54 * sx) +
			'; height: ' + px(38.54 * sy) + '; border-radius: 50%;');
		var dots = [
			[15.6, 1.1, 1], [15.6, 7.4, 2], [15.6, 16, 3.1], [15.6, 24.5, 3.1], [15.6, 33, 2], [15.6, 39.4, 1],
			[7.2, 7.4, 2], [7.2, 16, 2], [7.2, 24.5, 2], [7.2, 33, 2],
			[1, 16, 1], [1, 24.5, 1]
		];
		dots.forEach(function (d) { circle(parent, sx, sy, d[0], d[1], d[2]); });
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
		refreshState();
		updateHpShake(Date.now());
		render();
		$.Schedule(1 / 60, tick);
	}

	function init()
	{
		['Hud', 'HpBox', 'Speed', 'Timer', 'ScoreLeft', 'ScoreRight', 'NameLeft', 'NameRight', 'Clip', 'Reserve',
			'Reload', 'ExCount', 'Pip0', 'Pip1', 'Pip2', 'Cooldown0', 'Cooldown1', 'Cooldown2', 'CdTime0', 'CdTime1',
			'CdTime2'].forEach(function (id) { el[id] = $('#' + id); });

		// the 2048 px ring and the ability pip come from the page's own data-URL pack
		if (ASSETS.hudring)
			$('#Ring').SetImage(ASSETS.hudring);
		var pip = ASSETS.abilityPip || 'file://{game}/data/web/hud/assets/abilitypip1.png';
		$('#Hud').FindChildrenWithClassTraverse('ability-pip').forEach(function (p) { p.SetImage(pip); });

		buildGauge($('#Gauge'));
		buildCirclesIcon($('#CdIcon0'));
		buildCubeIcon($('#CdIcon1'));
		buildDeblurIcon($('#CdIcon2'));

		tick();
	}

	init();
	return { State: HudState, Receive: applyHudData };
})();
