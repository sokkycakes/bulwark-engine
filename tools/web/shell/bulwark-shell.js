/* Bulwark web shell: loads the engine and a game's data, then hands the
 * page over to the game. What the browser keeps from the engine (the
 * mouse, fullscreen, sound, the Back button) is managed here, so the game
 * behaves as it does on the desktop. ISC licence, see LICENSE.
 *
 * bulwark-web.json (written by tools/web/export.py):
 *   title, engine: {js, wasm, wasmSize}, args: [...],
 *   packs: [{url, size, rawSize, gzip, files: [[path, offset, size], ...]}],
 *   options: {fullscreenOnPlay: "auto"|"always"|"never", maxPixelRatio}
 *
 * Page query options, for testing and embedding:
 *   ?autostart=1   skip the Play button
 *   ?args=...      extra engine arguments (space separated, appended)
 *   ?nocache=1     don't use the Cache Storage copy of the data
 *   ?connect=host  join that server (through tools/web/wsrelay.py)
 *   ?debug=1       print the engine's log to the browser console as it goes
 */
(function () {
	"use strict";

	var $ = function (id) { return document.getElementById(id); };
	var stage = $("stage"), canvas = $("canvas"), cover = $("cover");
	var statusEl = $("status"), fill = $("fill"), bar = $("bar"), detail = $("detail");
	var playBtn = $("play"), hint = $("hint"), actions = $("actions");
	var resumeEl = $("resume"), fsButton = $("fsbutton"), toastEl = $("toast");

	var query = new URLSearchParams(location.search);
	/* a query value as typed: "+map" stays "+map" (URLSearchParams reads
	 * a + as a space, and engine commands start with one) */
	function rawParam(name) {
		var m = new RegExp("[?&]" + name + "=([^&]*)").exec(location.search);
		if (!m)
			return null;
		try { return decodeURIComponent(m[1]); } catch (e) { return m[1]; }
	}
	var debug = query.has("debug");
	var log = [];
	var config = null;
	var started = false, ended = false, failed = false;
	var wantGrab = false;        /* the game wants the mouse */
	var lastInput = "mouse";     /* "mouse", "touch" or "pad": what the player used last */
	var lastEscAt = -1e9;        /* when an Esc last reached the page */
	var pendingFullscreen = null; /* true/false: a fullscreen change waiting for a click or key */
	var lockRequestedAt = 0;

	function logLine(text) {
		log.push(text);
		if (log.length > 4000)
			log.splice(0, 1000);
		if (debug)
			console.log(text);
	}

	/* --- what the browser can do -------------------------------------- */

	var touchFirst = window.matchMedia && matchMedia("(pointer: coarse)").matches;
	var isAndroid = /Android/i.test(navigator.userAgent);
	var isFirefox = /Firefox\//.test(navigator.userAgent);

	function fullscreenElement() {
		return document.fullscreenElement || document.webkitFullscreenElement || null;
	}
	function isFullscreen() { return !!fullscreenElement(); }

	function webgl2Problem() {
		try {
			var c = document.createElement("canvas");
			var gl = c.getContext("webgl2");
			if (!gl)
				return "This browser can't draw the game: WebGL 2 is off or unsupported. " +
					"Turn on hardware acceleration in the browser's settings, or try an up-to-date Chrome, Edge or Firefox.";
			var lose = gl.getExtension("WEBGL_lose_context");
			if (lose)
				lose.loseContext();
		} catch (e) {
			return "This browser can't draw the game (WebGL 2: " + e.message + ").";
		}
		if (typeof WebAssembly !== "object")
			return "This browser can't run the game: it has no WebAssembly.";
		return null;
	}

	/* --- the cover card ------------------------------------------------ */

	function setStatus(text, isError) {
		statusEl.textContent = text;
		statusEl.classList.toggle("error", !!isError);
	}
	function setProgress(frac) {
		bar.hidden = frac == null;
		if (frac != null)
			fill.style.width = Math.max(0, Math.min(100, frac * 100)).toFixed(1) + "%";
	}
	function mb(n) { return (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + " MB"; }

	function showCover(clear) {
		cover.classList.remove("hidden");
		cover.classList.toggle("clear", !!clear);
	}
	function hideCover() { cover.classList.add("hidden"); }

	var toastTimer = 0;
	function toast(text, ms) {
		toastEl.textContent = text;
		toastEl.hidden = false;
		toastEl.classList.remove("fade");
		clearTimeout(toastTimer);
		toastTimer = setTimeout(function () {
			toastEl.classList.add("fade");
			toastTimer = setTimeout(function () { toastEl.hidden = true; }, 350);
		}, ms || 2600);
	}

	function fail(message, extra) {
		if (failed)
			return;
		failed = true;
		wantGrab = false;
		try { if (document.pointerLockElement) document.exitPointerLock(); } catch (e) {}
		resumeEl.hidden = true;
		showCover(false);
		setStatus(message, true);
		setProgress(null);
		detail.textContent = extra || "";
		playBtn.hidden = true;
		hint.hidden = true;
		actions.hidden = false;
		logLine("ERROR " + message + (extra ? " " + extra : ""));
	}

	$("reload").addEventListener("click", function () { location.reload(); });
	$("copylog").addEventListener("click", function () {
		var text = log.join("\n");
		var done = function () { toast("Log copied"); };
		if (navigator.clipboard && navigator.clipboard.writeText)
			navigator.clipboard.writeText(text).then(done, function () { fallbackCopy(text); done(); });
		else {
			fallbackCopy(text);
			done();
		}
	});
	function fallbackCopy(text) {
		var t = document.createElement("textarea");
		t.value = text;
		document.body.appendChild(t);
		t.select();
		try { document.execCommand("copy"); } catch (e) {}
		t.remove();
	}

	window.addEventListener("error", function (e) {
		if (started || !config)
			fail("The game stopped because of an error.", e.message || "");
	});
	window.addEventListener("unhandledrejection", function (e) {
		var r = e.reason;
		if (r && /abort|RuntimeError|out of memory/i.test(String(r)))
			fail("The game stopped because of an error.", String(r));
	});

	/* --- downloads ------------------------------------------------------ */

	var CACHE_NAME = "bulwark-data-v1";
	var useCache = !query.has("nocache") && typeof caches !== "undefined" && window.isSecureContext;

	/* the bytes at url, with progress(bytesSoFar); from Cache Storage when
	 * an earlier visit saved them there (the names carry a content hash) */
	function download(url, size, progress) {
		var fromNetwork = function () {
			return fetch(url, { cache: "default" }).then(function (resp) {
				if (!resp.ok)
					throw new Error(url + ": HTTP " + resp.status);
				var copy = useCache ? resp.clone() : null;
				return readAll(resp, size, progress).then(function (buf) {
					if (copy)
						caches.open(CACHE_NAME).then(function (c) { return c.put(url, copy); }).catch(function () {});
					return buf;
				});
			});
		};
		if (!useCache)
			return fromNetwork();
		return caches.open(CACHE_NAME).then(function (c) {
			return c.match(url).then(function (hit) {
				if (!hit)
					return fromNetwork();
				return readAll(hit, size, progress).catch(fromNetwork);
			});
		}).catch(fromNetwork);
	}

	function readAll(resp, size, progress) {
		if (!resp.body || !resp.body.getReader)
			return resp.arrayBuffer().then(function (b) { progress(b.byteLength); return new Uint8Array(b); });
		var reader = resp.body.getReader();
		var chunks = [], got = 0;
		var pump = function () {
			return reader.read().then(function (r) {
				if (r.done) {
					if (chunks.length === 1)
						return chunks[0];
					var out = new Uint8Array(got), at = 0;
					for (var i = 0; i < chunks.length; i++) {
						out.set(chunks[i], at);
						at += chunks[i].length;
					}
					return out;
				}
				chunks.push(r.value);
				got += r.value.length;
				progress(got);
				return pump();
			});
		};
		return pump();
	}

	/* a pack is gzip unless a server already undid that on the way */
	function unpack(bytes) {
		if (bytes.length < 2 || bytes[0] !== 0x1f || bytes[1] !== 0x8b)
			return Promise.resolve(bytes);
		if (typeof DecompressionStream === "undefined")
			return Promise.reject(new Error("this browser can't unpack the game's data (no DecompressionStream)"));
		var ds = new DecompressionStream("gzip");
		var stream = new Blob([bytes]).stream().pipeThrough(ds);
		return new Response(stream).arrayBuffer().then(function (b) { return new Uint8Array(b); });
	}

	/* Cache Storage keeps only the current files */
	function pruneCache(keep) {
		if (!useCache)
			return;
		var wanted = {};
		keep.forEach(function (u) { wanted[new URL(u, location.href).href] = true; });
		caches.open(CACHE_NAME).then(function (c) {
			return c.keys().then(function (reqs) {
				reqs.forEach(function (r) { if (!wanted[r.url]) c.delete(r); });
			});
		}).catch(function () {});
	}

	/* --- the engine ----------------------------------------------------- */

	var Module = window.Module = {
		canvas: canvas,
		noInitialRun: true,
		print: function (t) { logLine(t); },
		printErr: function (t) { logLine("! " + t); },
		setStatus: function () {},
		locateFile: function (path) {
			if (/\.wasm$/.test(path) && config)
				return config.engine.wasm;
			return path;
		},
		/* game packets to a server go out as WebSocket messages to
		 * ws://<host>:<port>/ (wss:// from an https page): a server takes
		 * them through tools/web/wsrelay.py */
		websocket: { url: location.protocol === "https:" ? "wss://" : "ws://", subprotocol: "binary" },
		/* main returned without starting its frame loop: a bad argument, or
		 * the game couldn't start; or the game ran its course */
		onExit: function (code) {
			if (!started || ended || failed)
				return;
			if (code)
				fail("The game couldn't start.", "It stopped with code " + code + ". Copy the log for details.");
			else
				gameEnded();
		},
		onAbort: function (what) { fail("The game stopped because of an error.", String(what || "")); },
		/* the engine's requests to the page (src/client/vid_sdl.c, src/platform/web) */
		bulwarkSetGrab: function (grab) { setGrab(grab); },
		bulwarkSetFullscreen: function (on) { requestFullscreenState(on); },
		bulwarkIsFullscreen: function () { return isFullscreen(); },
		bulwarkSetTitle: function (text) { if (text) document.title = text; },
		bulwarkUserChanged: function () { scheduleSave(); },
		bulwarkQuit: function () { gameEnded(); }
	};

	/* WebAssembly from bytes we fetched ourselves (with progress and cache) */
	var wasmBytes = null;
	Module.instantiateWasm = function (imports, done) {
		wasmBytes.then(function (bytes) {
			return WebAssembly.instantiate(bytes, imports);
		}).then(function (r) {
			done(r.instance, r.module);
		}).catch(function (e) {
			fail("The game couldn't start.", "WebAssembly: " + e.message);
		});
		return {};
	};

	var runtimeReady = new Promise(function (resolve) { Module.onRuntimeInitialized = resolve; });

	function loadScript(src) {
		return new Promise(function (resolve, reject) {
			var s = document.createElement("script");
			s.src = src;
			s.async = true;
			s.onload = resolve;
			s.onerror = function () { reject(new Error("couldn't load " + src)); };
			document.body.appendChild(s);
		});
	}

	/* --- the player's files: /user, kept in IndexedDB --------------------- */

	var saveTimer = 0, saving = false, saveAgain = false;
	/* soon after the first write, however many follow (a log written every
	 * frame must not keep putting it off) */
	function scheduleSave() {
		if (!started || saveTimer)
			return;
		saveTimer = setTimeout(function () {
			saveTimer = 0;
			saveNow();
		}, 800);
	}
	function saveNow() {
		var FS = Module.FS;
		if (!FS || !userMounted)
			return;
		if (saving) {
			saveAgain = true;
			return;
		}
		saving = true;
		FS.syncfs(false, function (err) {
			saving = false;
			if (err)
				logLine("! couldn't save the player's files: " + err);
			if (saveAgain) {
				saveAgain = false;
				saveNow();
			}
		});
	}
	var userMounted = false;
	function mountUser() {
		var FS = Module.FS;
		return new Promise(function (resolve) {
			try {
				FS.mkdir("/user");
				FS.mount(Module.IDBFS || FS.filesystems.IDBFS, {}, "/user");
				userMounted = true;
			} catch (e) {
				logLine("! no saved files: " + e);
				resolve();
				return;
			}
			FS.syncfs(true, function (err) {
				if (err)
					logLine("! couldn't read the saved files (private window?): " + err);
				resolve();
			});
		});
	}

	function writeFiles(pack, bytes) {
		var FS = Module.FS;
		var made = {};
		pack.files.forEach(function (f) {
			var path = f[0], at = f[1], size = f[2];
			var dir = path.substring(0, path.lastIndexOf("/"));
			if (dir && !made[dir]) {
				FS.mkdirTree(dir);
				made[dir] = true;
			}
			/* a view, not a copy: MEMFS keeps it as the file's contents */
			FS.writeFile(path, bytes.subarray(at, at + size), { canOwn: true });
		});
	}

	/* --- loading ---------------------------------------------------------- */

	function start() {
		var problem = webgl2Problem();
		if (problem) {
			fail("Can't run here", problem);
			return;
		}
		setStatus("Loading…");
		setProgress(0);
		fetch("bulwark-web.json", { cache: "no-cache" }).then(function (r) {
			if (!r.ok)
				throw new Error("bulwark-web.json: HTTP " + r.status);
			return r.json();
		}).then(function (c) {
			config = c;
			config.options = config.options || {};
			if (config.title) {
				document.title = config.title;
				$("title").textContent = config.title;
			}
			return load();
		}).catch(function (e) {
			fail("The game couldn't load.", e.message + (location.protocol === "file:" ?
				"\nOpen it from a web server (python3 tools/web/serve.py <folder>), not as a file." : ""));
		});
	}

	function load() {
		var total = config.engine.wasmSize || 0, got = {};
		config.packs.forEach(function (p) { total += p.size; });
		var shown = 0;
		var progress = function (key) {
			return function (n) {
				if (failed)
					return;
				got[key] = n;
				var sum = 0;
				for (var k in got)
					sum += got[k];
				var frac = total ? sum / total : 0;
				if (frac - shown > 0.002 || frac >= 1) {
					shown = frac;
					setProgress(frac);
					detail.textContent = mb(sum) + " of " + mb(total);
				}
			};
		};
		wasmBytes = download(config.engine.wasm, config.engine.wasmSize, progress("wasm"));
		pruneCache([config.engine.wasm].concat(config.packs.map(function (p) { return p.url; })));

		/* a few downloads at a time, each unpacked as it lands */
		var queue = config.packs.slice(), packsDone = [];
		var unpacked = [];
		var worker = function () {
			var p = queue.shift();
			if (!p)
				return Promise.resolve();
			return download(p.url, p.size, progress(p.url)).then(unpack).then(function (bytes) {
				unpacked.push({ pack: p, bytes: bytes });
				return worker();
			});
		};
		for (var i = 0; i < Math.min(3, config.packs.length); i++)
			packsDone.push(worker());

		return loadScript(config.engine.js).then(function () {
			return Promise.all([runtimeReady, wasmBytes].concat(packsDone));
		}).then(function () {
			setStatus("Getting ready…");
			unpacked.forEach(function (u) { writeFiles(u.pack, u.bytes); });
			unpacked = null;
			return mountUser();
		}).then(ready);
	}

	function ready() {
		setProgress(null);
		detail.textContent = "";
		if (query.has("autostart")) {
			play();
			return;
		}
		setStatus("");
		playBtn.hidden = false;
		hint.hidden = false;
		hint.textContent = touchFirst ? "Tap Play, or press A on the controller" :
			"Click Play, or press Enter. Esc opens the menu.";
		try { playBtn.focus({ preventScroll: true }); } catch (e) {}
	}

	/* --- playing ---------------------------------------------------------- */

	function play(fromGesture) {
		if (started || failed)
			return;
		started = true;
		playBtn.hidden = true;
		hint.hidden = true;
		hideCover();
		unlockAudio();
		var fsMode = config.options.fullscreenOnPlay || "auto";
		var wantFs = fsMode === "always" || (fsMode === "auto" && (touchFirst || isAndroid));
		if (wantFs && fromGesture)
			enterFullscreen();
		else if (wantFs || !fromGesture) {
			/* started from a controller, which browsers don't count as a
			 * click: fullscreen and sound wait for the first tap or key */
			if (wantFs)
				pendingFullscreen = true;
			setTimeout(function () {
				var soundOff = audioContexts.some(function (c) { return c.state !== "running"; });
				if (soundOff || (wantFs && !isFullscreen()))
					toast(touchFirst ? "Tap the screen once for " + (wantFs && !isFullscreen() ? "fullscreen and sound" : "sound") :
						"Click or press a key once for sound", 5000);
			}, 900);
		}
		guardBackButton();
		var args = (config.args || []).slice();
		/* ?connect=host[:port]: a link that joins a server, in place of the
		 * map or server the export starts with */
		var join = rawParam("connect");
		if (join && /^[\w.\-\[\]:]+$/.test(join)) {
			var kept = [];
			for (var i = 0; i < args.length; i++) {
				if (args[i] === "+map" || args[i] === "+connect" || args[i] === "+playdemo") {
					while (i + 1 < args.length && !/^[+\-]/.test(args[i + 1]))
						i++;
					continue;
				}
				kept.push(args[i]);
			}
			args = kept.concat(["+connect", join]);
		}
		var extra = rawParam("args");
		if (extra)
			args = args.concat(extra.split(" ").filter(Boolean));
		logLine("bulwark " + args.join(" "));
		try { canvas.focus({ preventScroll: true }); } catch (e) {}
		try {
			/* main returns once its frame loop is set up (0), or at once
			 * with an error (a bad argument): the page must not sit black */
			var ret = Module.callMain(args);
			if (ret)
				fail("The game couldn't start.", "It stopped with code " + ret + ". Copy the log for details: " +
					log.slice(-3).join(" / "));
		} catch (e) {
			if (!(e && e.name === "ExitStatus"))
				fail("The game couldn't start.", String(e));
		}
		/* the engine opened its sound device just now, still inside the
		 * click: Firefox wants the resume asked for here */
		unlockAudio();
		fsButton.hidden = false;
		updateFsButton();
	}

	playBtn.addEventListener("click", function () { play(true); });

	function gameEnded() {
		if (ended)
			return;
		ended = true;
		wantGrab = false;
		saveNow();
		try { if (document.pointerLockElement) document.exitPointerLock(); } catch (e) {}
		resumeEl.hidden = true;
		fsButton.hidden = true;
		if (isFullscreen())
			exitFullscreen();
		showCover(false);
		setStatus("Thanks for playing.");
		setProgress(null);
		detail.textContent = "";
		actions.hidden = false;
		$("reload").textContent = "Play again";
		$("copylog").hidden = true;
	}

	/* --- sound: browsers start it paused until a click or key ------------- */

	var audioContexts = [];
	(function trackAudio() {
		var AC = window.AudioContext || window.webkitAudioContext;
		if (!AC)
			return;
		var Wrapped = function (opts) {
			var ctx = opts ? new AC(opts) : new AC();
			audioContexts.push(ctx);
			/* a resume still pending when the game quits and closes the
			 * context is no error worth reporting */
			var resume = ctx.resume;
			ctx.resume = function () {
				var p = resume.apply(ctx, arguments);
				if (p && p.catch)
					p.catch(function () {});
				return p;
			};
			if (ctx.state !== "running" && started)
				soundHint();
			return ctx;
		};
		Wrapped.prototype = AC.prototype;
		window.AudioContext = Wrapped;
		if (window.webkitAudioContext)
			window.webkitAudioContext = Wrapped;
	})();
	function unlockAudio() {
		audioContexts.forEach(function (ctx) {
			if (ctx.state === "suspended")
				ctx.resume().catch(function () {});
		});
	}
	var soundHinted = false;
	function soundHint() {
		if (soundHinted)
			return;
		soundHinted = true;
		setTimeout(function () {
			var stillOff = audioContexts.some(function (c) { return c.state !== "running"; });
			if (stillOff)
				toast(touchFirst ? "Tap the screen to turn on sound" : "Click or press a key to turn on sound", 4000);
		}, 1200);
	}

	/* --- the mouse ------------------------------------------------------- */

	function lockPointer() {
		if (!started || ended || failed || document.pointerLockElement === canvas)
			return;
		lockRequestedAt = performance.now();
		var onFail = function () {
			/* refused (not in a click, or too soon after Esc): ask for one */
			if (wantGrab && document.pointerLockElement !== canvas)
				showResume();
		};
		try {
			/* raw mouse motion where the browser has it, as on the desktop */
			var p = canvas.requestPointerLock({ unadjustedMovement: true });
			if (p && p.catch) {
				p.catch(function (e) {
					if (e && e.name === "NotSupportedError") {
						var q = canvas.requestPointerLock();
						if (q && q.catch)
							q.catch(onFail);
					} else
						onFail();
				});
			}
		} catch (e) {
			try {
				var q = canvas.requestPointerLock();
				if (q && q.catch)
					q.catch(onFail);
			} catch (e2) {
				onFail();
			}
		}
	}

	function setGrab(grab) {
		wantGrab = grab;
		updateFsButton();
		if (grab) {
			/* playing on a controller or touch screen: nothing to capture */
			if (lastInput !== "mouse")
				return;
			lockPointer();
		} else {
			resumeEl.hidden = true;
			if (document.pointerLockElement)
				document.exitPointerLock();
		}
	}

	function showResume() {
		if (!wantGrab || ended || failed || lastInput !== "mouse")
			return;
		resumeEl.hidden = false;
	}

	document.addEventListener("pointerlockchange", function () {
		var locked = document.pointerLockElement === canvas;
		updateFsButton();
		if (locked) {
			resumeEl.hidden = true;
			return;
		}
		if (!wantGrab || ended || failed)
			return;
		/* the browser let go of the mouse (Esc, alt-tab, a dialog): the game
		 * hears its Esc and opens the menu, as on the desktop */
		var escSeen = performance.now() - lastEscAt < 400;
		if (Module._bulwark_pointer_lock_lost)
			Module._bulwark_pointer_lock_lost(escSeen ? 1 : 0);
		/* still wanted after that (no menu came up): offer the click */
		setTimeout(function () { if (wantGrab && document.pointerLockElement !== canvas) showResume(); }, 120);
	});
	document.addEventListener("pointerlockerror", function () {
		if (wantGrab)
			showResume();
	});

	resumeEl.addEventListener("mousedown", function (e) {
		e.preventDefault();
		e.stopPropagation();
		lastInput = "mouse";
		lockPointer();
		unlockAudio();
	});

	/* a click on the game while it wants the mouse but doesn't have it */
	canvas.addEventListener("mousedown", function () {
		lastInput = "mouse";
		if (wantGrab && document.pointerLockElement !== canvas)
			lockPointer();
	});
	canvas.addEventListener("contextmenu", function (e) { e.preventDefault(); });
	document.addEventListener("contextmenu", function (e) { if (started) e.preventDefault(); });

	/* --- fullscreen --------------------------------------------------------- */

	function enterFullscreen() {
		var el = stage;
		var req = el.requestFullscreen || el.webkitRequestFullscreen;
		if (!req)
			return Promise.resolve(false);
		try {
			var r = req.call(el, { navigationUI: "hide" });
			return Promise.resolve(r).then(function () { return true; }, function () { return false; });
		} catch (e) {
			return Promise.resolve(false);
		}
	}
	function exitFullscreen() {
		var ex = document.exitFullscreen || document.webkitExitFullscreen;
		if (ex && fullscreenElement()) {
			try { Promise.resolve(ex.call(document)).catch(function () {}); } catch (e) {}
		}
	}

	/* the game asked (vid_fullscreen, vid_toggle, its options menu): only a
	 * click or key may enter fullscreen, so this waits for the next one when
	 * the request came without */
	function requestFullscreenState(on) {
		if (!started)
			return;
		if (!!on === isFullscreen())
			return;
		if (!on) {
			exitFullscreen();
			return;
		}
		enterFullscreen().then(function (ok) {
			if (!ok)
				pendingFullscreen = true;
		});
	}

	function onFullscreenChange() {
		var fs = isFullscreen();
		updateFsButton();
		/* in fullscreen the game gets Esc and the browser's shortcuts
		 * (Chrome and Edge: holding Esc still leaves), and a handheld turns
		 * sideways and stays there */
		if (fs) {
			if (navigator.keyboard && navigator.keyboard.lock)
				navigator.keyboard.lock().catch(function () {});
			if (screen.orientation && screen.orientation.lock && (touchFirst || isAndroid))
				screen.orientation.lock("landscape").catch(function () {});
		} else {
			if (navigator.keyboard && navigator.keyboard.unlock)
				navigator.keyboard.unlock();
		}
		/* the canvas changed size, which the engine follows each frame; its
		 * vid_fullscreen follows too, so its menu shows the truth and the
		 * next visit starts the same way (at the first click) */
		if (started && !ended)
			window.bulwark.command("vid_fullscreen " + (fs ? 2 : 0));
	}
	document.addEventListener("fullscreenchange", onFullscreenChange);
	document.addEventListener("webkitfullscreenchange", onFullscreenChange);

	fsButton.addEventListener("click", function (e) {
		e.stopPropagation();
		if (isFullscreen())
			exitFullscreen();
		else
			enterFullscreen();
	});
	function updateFsButton() {
		/* only over menus, never over the game itself */
		fsButton.hidden = !started || ended || failed || wantGrab || !!document.pointerLockElement ||
			isFullscreen() || lastInput === "pad";
		/* playing on a controller or a touch screen: no arrow left over the game */
		canvas.style.cursor = started && wantGrab && lastInput !== "mouse" ? "none" : "";
	}

	/* a click or key: what browsers allow to start fullscreen, the pointer lock and sound */
	function userGesture() {
		unlockAudio();
		if (pendingFullscreen) {
			pendingFullscreen = null;
			enterFullscreen();
		}
	}

	/* --- keys --------------------------------------------------------------- */

	window.addEventListener("keydown", function (e) {
		if (e.key === "Escape")
			lastEscAt = performance.now();
		lastInput = "mouse";
		if (!started) {
			if ((e.key === "Enter" || e.key === " ") && !playBtn.hidden && document.activeElement !== playBtn) {
				e.preventDefault();
				play(true);
			}
			return;
		}
		userGesture();
		/* F11: the page's fullscreen, so the game keeps its keys */
		if (e.key === "F11") {
			e.preventDefault();
			if (isFullscreen())
				exitFullscreen();
			else
				enterFullscreen();
			return;
		}
		/* keys the browser would otherwise act on (find, help, the address
		 * bar, scrolling, saving the page) belong to the game while it runs */
		if (/^F([1-9]|10)$/.test(e.key) || e.key === "Tab" || e.key === "Backspace" || e.key === "Alt" ||
			(e.ctrlKey && /^[a-z]$/i.test(e.key) && "cvx".indexOf(e.key.toLowerCase()) < 0) ||
			e.key === "ArrowUp" || e.key === "ArrowDown" || e.key === "PageUp" || e.key === "PageDown" ||
			e.key === "Home" || e.key === "End")
			e.preventDefault();
	}, true);
	/* Firefox's quick find opens on ' and / from the keypress: the game
	 * still gets the keypress, only the browser's default is cancelled */
	window.addEventListener("keypress", function (e) {
		if (started)
			e.preventDefault();
	});
	window.addEventListener("keyup", function (e) {
		if (started && (e.key === "Alt" || e.key === "F10"))
			e.preventDefault(); /* the menu bar on Windows and Linux */
	}, true);

	/* --- pointer and touch ---------------------------------------------- */

	window.addEventListener("pointerdown", function (e) {
		lastInput = e.pointerType === "touch" || e.pointerType === "pen" ? "touch" : "mouse";
		if (started)
			userGesture();
	}, true);
	window.addEventListener("pointerup", function () { if (started) userGesture(); }, true);
	window.addEventListener("touchend", function () { if (started) userGesture(); }, true);
	/* nothing of the page scrolls or zooms */
	document.addEventListener("touchmove", function (e) { if (e.cancelable) e.preventDefault(); }, { passive: false });
	document.addEventListener("gesturestart", function (e) { e.preventDefault(); });
	window.addEventListener("wheel", function (e) { if (started && e.ctrlKey) e.preventDefault(); }, { passive: false });

	/* --- controllers (handhelds: Steam Deck, Ayn, Ayaneo, Retroid, ...) ---- */

	var padDown = {};
	function pollPads() {
		var pads = navigator.getGamepads ? navigator.getGamepads() : [];
		for (var i = 0; i < pads.length; i++) {
			var p = pads[i];
			if (!p || !p.connected)
				continue;
			for (var b = 0; b < p.buttons.length; b++) {
				var down = p.buttons[b].pressed || p.buttons[b].value > 0.5;
				var key = i + ":" + b;
				if (down && !padDown[key]) {
					lastInput = "pad";
					if (!resumeEl.hidden)
						resumeEl.hidden = true;
					updateFsButton();
					/* A or Start on the Play card */
					if (!started && !playBtn.hidden && (b === 0 || b === 9))
						play(false);
				}
				padDown[key] = down;
			}
			for (var a = 0; a < p.axes.length; a++)
				if (Math.abs(p.axes[a]) > 0.6 && lastInput !== "pad") {
					lastInput = "pad";
					resumeEl.hidden = true;
					updateFsButton();
				}
		}
		if (padsSeen)
			requestAnimationFrame(pollPads);
	}
	/* browsers show a controller only once it's pressed, and say so with
	 * gamepadconnected; polling before that costs (Firefox) and finds nothing */
	var padsSeen = false;
	function padSeen() {
		if (padsSeen)
			return;
		padsSeen = true;
		requestAnimationFrame(pollPads);
	}
	window.addEventListener("gamepadconnected", function (e) {
		padSeen();
		var name = (e.gamepad && e.gamepad.id || "Controller").replace(/\s*\(.*?\)\s*/g, " ").trim();
		toast("Controller connected: " + (name || "controller"));
		lastInput = "pad";
		resumeEl.hidden = true;
		updateFsButton();
	});
	window.addEventListener("gamepaddisconnected", function () { toast("Controller disconnected"); });

	/* Android maps a controller's B (and the system back gesture) to Back,
	 * which would leave the page in the middle of a game: the page keeps a
	 * history entry of its own to absorb it */
	var backGuarded = false;
	function guardBackButton() {
		if (backGuarded || !history.pushState)
			return;
		backGuarded = true;
		try { history.pushState({ bulwark: 1 }, ""); } catch (e) {}
		window.addEventListener("popstate", function () {
			if (ended || failed)
				return;
			try { history.pushState({ bulwark: 1 }, ""); } catch (e) {}
		});
	}

	/* --- the canvas's resolution ----------------------------------------- */

	/* Phones and handhelds report 2 to 4 device pixels per CSS pixel; drawing
	 * every one of them costs frames for no visible gain at arm's length. The
	 * engine sizes its drawing buffer from devicePixelRatio, so the page caps
	 * what it reports (options.maxPixelRatio, else about 1440p worth). */
	(function capPixelRatio() {
		var desc = Object.getOwnPropertyDescriptor(window, "devicePixelRatio") ||
			Object.getOwnPropertyDescriptor(Window.prototype, "devicePixelRatio");
		if (!desc || !desc.get)
			return;
		var real = function () { return desc.get.call(window); };
		try {
			Object.defineProperty(window, "devicePixelRatio", {
				configurable: true,
				get: function () {
					var r = real();
					var cap = config && config.options.maxPixelRatio;
					var w = Math.max(1, stage.clientWidth), h = Math.max(1, stage.clientHeight);
					var budget = Math.sqrt((2560 * 1440) / (w * h));
					var limit = cap ? Math.min(cap, r) : Math.min(r, Math.max(1, budget));
					return Math.max(1, Math.round(limit * 100) / 100);
				}
			});
		} catch (e) {}
	})();

	/* --- leaving --------------------------------------------------------- */

	function pageHidden() {
		if (!started || ended)
			return;
		try {
			if (Module._bulwark_page_hidden)
				Module._bulwark_page_hidden();
		} catch (e) {}
		saveNow();
	}
	document.addEventListener("visibilitychange", function () {
		if (document.visibilityState === "hidden")
			pageHidden();
		else
			unlockAudio();
	});
	window.addEventListener("pagehide", pageHidden);
	/* Ctrl+W is crouch and forward in many games: in a game, closing the tab
	 * asks first (fullscreen's keyboard lock catches it where supported) */
	window.addEventListener("beforeunload", function (e) {
		pageHidden();
		var inGame = started && !ended && !failed && Module._bulwark_client_state &&
			Module._bulwark_client_state() >= 3;
		if (inGame && config.options.confirmLeave !== false) {
			e.preventDefault();
			e.returnValue = "";
			return "";
		}
	});

	/* --- for tests (tools/web/test) -------------------------------------- */
	window.bulwark = {
		log: log,
		command: function (text) {
			if (!Module._bulwark_command)
				return false;
			var p = Module.stringToNewUTF8(text);
			Module._bulwark_command(p);
			Module._free(p);
			return true;
		},
		state: function () {
			return {
				started: started, ended: ended, failed: failed, wantGrab: wantGrab,
				locked: document.pointerLockElement === canvas, fullscreen: isFullscreen(),
				resumeShown: !resumeEl.hidden, lastInput: lastInput,
				client: Module._bulwark_client_state ? Module._bulwark_client_state() : -1,
				frames: Module._bulwark_frames ? Module._bulwark_frames() : 0,
				audio: audioContexts.map(function (c) { return c.state; }),
				canvas: [canvas.width, canvas.height, canvas.clientWidth, canvas.clientHeight],
				firefox: isFirefox
			};
		}
	};

	start();
})();
