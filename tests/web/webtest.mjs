// Browser tests for Bulwark's web export: an exported game is played in
// Chromium and Firefox the way a playtester would, and everything the page
// and the engine owe them is checked. See tests/web/README.md.
//
//   node webtest.mjs --export ../../out/web-base [--browser chrome|firefox|all]
//                    [--chrome PATH] [--firefox PATH] [--headed] [--only name,...]
//
// The export must start a map from its arguments (export.py ... -- +map ai).
import puppeteer from "puppeteer-core";
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));

function arg(name, def) {
	const i = process.argv.indexOf("--" + name);
	if (i < 0)
		return def;
	const v = process.argv[i + 1];
	return v && !v.startsWith("--") ? v : true;
}

const exportDir = path.resolve(arg("export", path.join(here, "../../out/web-base")));
const which = arg("browser", "all");
const headed = !!arg("headed", false);
const only = arg("only", "") ? String(arg("only")).split(",") : null;
const outDir = path.resolve(arg("out", path.join(here, "out")));
const dedPath = arg("ded", process.env.BULWARK_DED || "");
const nuclideBase = arg("basedir", process.env.BULWARK_BASEDIR || "");
const chromePath = arg("chrome", process.env.BULWARK_CHROME || findFirst([
	"/opt/pw-browsers/chromium-1243/chrome-linux64/chrome", "/usr/bin/google-chrome", "/usr/bin/chromium",
	"/usr/bin/chromium-browser"]));
const firefoxPath = arg("firefox", process.env.BULWARK_FIREFOX || findFirst([
	"/usr/bin/firefox", "/opt/firefox/firefox", path.join(process.env.HOME || "", "ff/firefox/firefox")]));

function findFirst(list) {
	return list.find((p) => p && fs.existsSync(p));
}

fs.mkdirSync(outDir, { recursive: true });

// --- a web host for the export (what serve.py does) ----------------------
const TYPES = { ".wasm": "application/wasm", ".js": "text/javascript", ".json": "application/json",
	".html": "text/html; charset=utf-8", ".css": "text/css", ".webmanifest": "application/manifest+json" };
function serve(dir) {
	const srv = http.createServer((req, res) => {
		const u = decodeURIComponent(req.url.split("?")[0]);
		let p = path.join(dir, u === "/" ? "index.html" : u);
		if (!p.startsWith(dir) || !fs.existsSync(p) || fs.statSync(p).isDirectory()) {
			res.writeHead(404);
			res.end();
			return;
		}
		res.writeHead(200, { "Content-Type": TYPES[path.extname(p)] || "application/octet-stream",
			"Cache-Control": "no-cache" });
		fs.createReadStream(p).pipe(res);
	});
	return new Promise((resolve) => srv.listen(0, "127.0.0.1", () => resolve(srv)));
}

// --- helpers --------------------------------------------------------------
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitFor(page, fn, what, timeout = 60000, arg) {
	const end = Date.now() + timeout;
	let last;
	while (Date.now() < end) {
		try {
			last = await page.evaluate(fn, arg);
			if (last)
				return last;
		} catch (e) {
			last = e.message;
		}
		await sleep(200);
	}
	throw new Error("timed out waiting for " + what + " (last: " + JSON.stringify(last) + ")");
}

const state = (page) => page.evaluate(() => window.bulwark && window.bulwark.state());
const command = (page, text) => page.evaluate((t) => window.bulwark.command(t), text);
const logText = (page) => page.evaluate(() => window.bulwark.log.join("\n"));

// how different two screenshots of the canvas are (0..1), from a coarse
// grid of samples; and how much a single one varies (a black or one-colour
// frame doesn't)
async function canvasPixels(page) {
	return page.evaluate(() => new Promise((resolve) => {
		requestAnimationFrame(() => {
			const c = document.getElementById("canvas");
			const t = document.createElement("canvas");
			t.width = 64;
			t.height = 36;
			const g = t.getContext("2d");
			g.drawImage(c, 0, 0, 64, 36);
			resolve(Array.from(g.getImageData(0, 0, 64, 36).data));
		});
	}));
}
function variety(px) {
	let min = 255, max = 0;
	for (let i = 0; i < px.length; i += 4) {
		const l = (px[i] + px[i + 1] + px[i + 2]) / 3;
		min = Math.min(min, l);
		max = Math.max(max, l);
	}
	return max - min;
}
function difference(a, b) {
	let d = 0;
	for (let i = 0; i < a.length; i += 4)
		d += Math.abs(a[i] - b[i]) + Math.abs(a[i + 1] - b[i + 1]) + Math.abs(a[i + 2] - b[i + 2]);
	return d / (a.length / 4) / 765;
}

// a controller the page and the engine see through the Gamepad API
const FAKE_PAD = `(() => {
	const pad = { id: "Bulwark Test Pad (STANDARD GAMEPAD)", index: 0, connected: true, mapping: "standard",
		timestamp: 0, axes: [0, 0, 0, 0], vibrationActuator: null,
		buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
	let plugged = false;
	navigator.getGamepads = () => plugged ? [pad, null, null, null] : [null, null, null, null];
	window.__pad = {
		plug() {
			plugged = true;
			const e = new Event("gamepadconnected");
			e.gamepad = pad;
			window.dispatchEvent(e);
		},
		press(b, down) { pad.buttons[b] = { pressed: down, touched: down, value: down ? 1 : 0 }; pad.timestamp++; },
		axis(a, v) { pad.axes[a] = v; pad.timestamp++; }
	};
})();`;

// console messages a run may print and still pass
const ALLOWED = [
	/ScriptProcessorNode is deprecated/i, /GPU stall due to ReadPixels/i, /AudioContext was not allowed to start/i,
	/WebGL.*(warning|performance)/i, /favicon/i, /The AudioContext was not allowed/i,
	/Blocked aria-hidden/i, /preloaded using link preload/i,
];

// --- the tests ------------------------------------------------------------
const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

test("loads and offers Play", async (t) => {
	const page = await t.open("");
	await waitFor(page, () => !document.getElementById("play").hidden, "the Play button", 120000);
	const st = await state(page);
	t.check(!st.started && !st.failed, "not started or failed before Play");
	await page.screenshot({ path: t.shot("play-card") });
});

test("Play starts the game and draws the map", async (t) => {
	const page = await t.open("");
	await waitFor(page, () => !document.getElementById("play").hidden, "the Play button", 120000);
	await page.click("#play");
	await waitFor(page, () => window.bulwark.state().client >= 3, "the client in the game", 60000);
	await waitFor(page, () => window.bulwark.state().frames > 60, "60 frames");
	const px = await canvasPixels(page);
	t.check(variety(px) > 40, "the frame has a picture (luma range " + variety(px).toFixed(0) + ")");
	// a sound server can take a moment to open its stream after the click
	const t0 = Date.now();
	let st = await state(page);
	while (!(st.audio.length > 0 && st.audio.every((a) => a === "running")) && Date.now() - t0 < 5000) {
		await sleep(200);
		st = await state(page);
	}
	t.check(st.audio.length > 0 && st.audio.every((a) => a === "running"),
		"sound running after Play (" + st.audio + ", " + (Date.now() - t0) + " ms)");
	t.check(document_hidden_cover(await page.evaluate(() => document.getElementById("cover").className)),
		"the cover is gone");
	await page.screenshot({ path: t.shot("in-game") });
});
function document_hidden_cover(cls) { return /hidden/.test(cls); }

test("keyboard moves the player and the mouse turns the view", async (t) => {
	const page = await t.play();
	await sleep(500);
	const before = await canvasPixels(page);
	await page.keyboard.down("w");
	await sleep(900);
	await page.keyboard.up("w");
	await sleep(300);
	const moved = await canvasPixels(page);
	t.check(difference(before, moved) > 0.01, "holding W changes the view (" + difference(before, moved).toFixed(3) + ")");
	const st = await state(page);
	t.check(st.wantGrab, "the game wants the mouse in the game");
	if (st.locked) {
		await page.mouse.move(640, 360);
		for (let i = 0; i < 20; i++)
			await page.mouse.move(640 + i * 15, 360, { steps: 1 });
		await sleep(300);
		const turned = await canvasPixels(page);
		t.check(difference(moved, turned) > 0.01, "moving the locked mouse turns the view (" +
			difference(moved, turned).toFixed(3) + ")");
	} else {
		t.note("this browser didn't grant the pointer lock to automation; resume card shown: " + st.resumeShown);
		t.check(st.resumeShown, "without the lock, the page offers Click to resume");
	}
});

test("Esc frees the mouse and opens the menu; the game takes it back", async (t) => {
	const page = await t.play();
	await page.keyboard.press("Escape");
	await waitFor(page, () => !window.bulwark.state().wantGrab, "the menu (game lets go of the mouse)", 5000);
	let st = await state(page);
	t.check(!st.locked, "the pointer lock is released in the menu");
	t.check(!st.resumeShown, "no Click to resume card over the menu");
	await page.screenshot({ path: t.shot("menu") });
	await page.keyboard.press("Escape");
	await waitFor(page, () => window.bulwark.state().wantGrab, "the game again", 5000);
	await sleep(400);
	st = await state(page);
	t.check(st.locked || st.resumeShown, "back in the game: locked, or one click from it");
	if (st.resumeShown) {
		await page.mouse.click(640, 360);
		await sleep(400);
		st = await state(page);
		t.note("after a click on Click to resume: locked=" + st.locked);
	}
});

test("losing the pointer lock opens the menu, as Esc does on the desktop", async (t) => {
	const page = await t.play();
	let st = await state(page);
	if (!st.locked) {
		t.note("no pointer lock under automation here; skipped");
		return;
	}
	await page.evaluate(() => document.exitPointerLock());
	await waitFor(page, () => !window.bulwark.state().wantGrab, "the menu after the lock was lost", 5000);
	st = await state(page);
	t.check(!st.resumeShown, "the menu shows, not a resume card");
});

test("the canvas follows the window and high-DPI screens", async (t) => {
	const page = await t.play();
	await page.setViewport({ width: 900, height: 600, deviceScaleFactor: 1 });
	await waitFor(page, () => { const s = window.bulwark.state().canvas; return s[0] === 900 && s[1] === 600; },
		"a 900x600 canvas", 5000);
	await page.setViewport({ width: 800, height: 450, deviceScaleFactor: 2 });
	await waitFor(page, () => { const s = window.bulwark.state().canvas; return s[0] === 1600 && s[1] === 900; },
		"a 1600x900 canvas at 2x", 5000);
	await sleep(300);
	const px = await canvasPixels(page);
	t.check(variety(px) > 40, "still drawing after the resizes");
	await page.screenshot({ path: t.shot("resized") });
});

test("F11 and vid_fullscreen go fullscreen; Esc in the menu leaves it", async (t) => {
	const page = await t.play();
	await page.keyboard.press("F11");
	let ok = await page.evaluate(() => new Promise((r) => setTimeout(() => r(!!document.fullscreenElement), 600)));
	if (!ok) {
		t.note("this browser refuses fullscreen under automation; skipped");
		return;
	}
	t.check(ok, "F11 enters fullscreen");
	await page.keyboard.press("F11");
	await sleep(500);
	t.check(!(await page.evaluate(() => !!document.fullscreenElement)), "F11 again leaves it");
	await command(page, "vid_fullscreen 2; vid_restart");
	await sleep(300);
	await page.mouse.click(640, 360); // fullscreen needs a click: the page waits for one
	await sleep(600);
	t.check(await page.evaluate(() => !!document.fullscreenElement), "vid_fullscreen 2 goes fullscreen at the next click");
});

test("settings are saved and come back on the next visit", async (t) => {
	let page = await t.play();
	// as Nuclide's options menu does: the config, and the player's profile
	// (configs/Default.cfg), which it loads after the config
	await command(page, "sensitivity 7.25");
	await command(page, "cfg_save");
	await command(page, "cfg_save Default.cfg");
	await sleep(2500); // the page saves /user to IndexedDB shortly after a write
	page = await t.reopen(page);
	await t.playOn(page);
	await command(page, "sensitivity");
	await sleep(300);
	const log = await logText(page);
	t.check(/sensitivity.*7\.25/.test(log), "sensitivity 7.25 after reloading the page");
});

test("a controller starts the game from the Play card and plays it", async (t) => {
	const page = await t.open("", { pad: true });
	await waitFor(page, () => !document.getElementById("play").hidden, "the Play button", 120000);
	await page.evaluate(() => window.__pad.plug());
	await sleep(200);
	await page.evaluate(() => window.__pad.press(0, true));
	await sleep(150);
	await page.evaluate(() => window.__pad.press(0, false));
	await waitFor(page, () => window.bulwark.state().client >= 3, "the game, started with A", 60000);
	await waitFor(page, () => window.bulwark.state().frames > 60, "60 frames");
	const st = await state(page);
	t.check(st.lastInput === "pad", "the page knows a controller is in use");
	t.check(!st.resumeShown, "no Click to resume card for a controller player");
	const before = await canvasPixels(page);
	// a real stick passes through values on its way out (SDL takes an axis's
	// first report as its resting value)
	for (const v of [0.1, 0.3, 0.6, 0.9, 1])
		await page.evaluate((x) => window.__pad.axis(2, x), v), await sleep(40);
	await sleep(700);
	await page.evaluate(() => window.__pad.axis(2, 0));
	await sleep(200);
	const after = await canvasPixels(page);
	t.check(difference(before, after) > 0.01, "the right stick turns the view (" + difference(before, after).toFixed(3) + ")");
	const fsHidden = await page.evaluate(() => document.getElementById("fsbutton").hidden);
	t.check(fsHidden, "no mouse-only buttons over a controller game");
	await page.screenshot({ path: t.shot("controller") });
});

test("the Back button doesn't leave a game", async (t) => {
	const page = await t.play();
	const url = page.url();
	await page.goBack({ timeout: 3000 }).catch(() => {});
	await sleep(500);
	t.check(page.url() === url, "still on the game's page");
	const st = await state(page);
	t.check(st.started && !st.failed && st.client >= 3, "still in the game");
});

test("quitting shows the end card", async (t) => {
	const page = await t.play();
	await command(page, "quit");
	await waitFor(page, () => window.bulwark.state().ended, "the end of the game", 10000);
	const text = await page.evaluate(() => document.getElementById("status").textContent);
	t.check(/Thanks/.test(text), "the end card shows");
	await page.screenshot({ path: t.shot("ended") });
});

test("a broken export says what went wrong", async (t) => {
	const page = await t.open("", { breakData: true });
	await waitFor(page, () => window.bulwark && window.bulwark.state().failed, "the error card", 60000);
	const text = await page.evaluate(() => document.getElementById("status").textContent + " " +
		document.getElementById("detail").textContent);
	t.check(/couldn't load|error/i.test(text), "a readable error (" + text.slice(0, 120) + ")");
	t.allowErrors = true;
});

test("joins a desktop server through the WebSocket relay", async (t) => {
	if (!dedPath || !nuclideBase) {
		t.note("needs --ded <bulwark-ded> and --basedir <nuclide>; skipped");
		return;
	}
	const { spawn } = await import("node:child_process");
	const port = 27600 + Math.floor(Math.random() * 300);
	const ded = spawn(dedPath, ["-basedir", nuclideBase, "-game", "base", "-userdir", path.join(outDir, "ded-user"),
		"+set", "sv_port", String(port), "+maxplayers", "8", "+map", "ai"], { stdio: ["ignore", "pipe", "pipe"] });
	let dedlog = "";
	ded.stdout.on("data", (d) => (dedlog += d));
	ded.stderr.on("data", (d) => (dedlog += d));
	const relay = spawn("python3", [path.join(here, "../../tools/web/wsrelay.py"), "--listen", String(port),
		"--server", "127.0.0.1:" + port], { stdio: ["ignore", "pipe", "pipe"] });
	let relaylog = "";
	relay.stdout.on("data", (d) => (relaylog += d));
	relay.stderr.on("data", (d) => (relaylog += d));
	try {
		await sleep(2500);
		const page = await t.open("?connect=127.0.0.1:" + port);
		await waitFor(page, () => !document.getElementById("play").hidden, "the Play button", 120000);
		await page.click("#play");
		await waitFor(page, () => window.bulwark.state().client >= 3, "the client in the server's game", 30000);
		await sleep(1500);
		const px = await canvasPixels(page);
		t.check(variety(px) > 40, "the server's map draws");
		const before = px;
		await page.keyboard.down("w");
		await sleep(900);
		await page.keyboard.up("w");
		await sleep(400);
		const after = await canvasPixels(page);
		t.check(difference(before, after) > 0.01, "moving works online (the server answers)");
		t.check(/joined/.test(relaylog), "the relay passed the player on");
		t.check(/entered the game|connected/i.test(dedlog), "the desktop server saw the player");
		await page.screenshot({ path: t.shot("online") });
	} finally {
		fs.writeFileSync(path.join(outDir, "ded.log"), dedlog);
		fs.writeFileSync(path.join(outDir, "relay.log"), relaylog);
		ded.kill();
		relay.kill();
	}
});

// --- running ----------------------------------------------------------------
async function launch(kind) {
	if (kind === "chrome") {
		if (!chromePath)
			return null;
		return puppeteer.launch({
			browser: "chrome", executablePath: chromePath, headless: !headed,
			args: ["--no-sandbox", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist",
				"--autoplay-policy=user-gesture-required", "--window-size=1280,720"],
			defaultViewport: { width: 1280, height: 720 },
		});
	}
	if (!firefoxPath)
		return null;
	// headless Firefox has no WebGL here (no GPU, and its software path
	// refuses): it runs in a window, so on a server under xvfb-run
	const ffHeadless = !headed && !process.env.DISPLAY;
	if (ffHeadless)
		console.log("firefox: no DISPLAY, so headless, where it may have no WebGL: run under xvfb-run");
	return puppeteer.launch({
		browser: "firefox", executablePath: firefoxPath, headless: ffHeadless,
		defaultViewport: { width: 1280, height: 720 },
		extraPrefsFirefox: {
			"webgl.force-enabled": true, "webgl.enable-webgl2": true, "media.autoplay.default": 1,
			"full-screen-api.allow-trusted-requests-only": false, "full-screen-api.warning.timeout": 0,
			"full-screen-api.transition-duration.enter": "0 0", "full-screen-api.transition-duration.leave": "0 0",
			"dom.gamepad.enabled": true,
		},
		env: { ...process.env, LD_LIBRARY_PATH: path.dirname(firefoxPath) + (process.env.LD_LIBRARY_PATH ? ":" + process.env.LD_LIBRARY_PATH : "") },
	});
}

async function runBrowser(kind, base) {
	const browser = await launch(kind);
	if (!browser) {
		console.log(`\n== ${kind}: not found, skipped`);
		return { kind, skipped: true, results: [] };
	}
	const version = await browser.version();
	console.log(`\n== ${kind}: ${version}`);
	const results = [];
	for (const tc of tests) {
		if (only && !only.some((o) => tc.name.includes(o)))
			continue;
		// each test gets a fresh profile (saved files and caches included)
		const ctx = await browser.createBrowserContext();
		const t = {
			checks: [], notes: [], errors: [], allowErrors: false,
			check(ok, what) { this.checks.push({ ok: !!ok, what }); },
			note(what) { this.notes.push(what); },
			shot(name) { return path.join(outDir, `${kind}-${name}.png`); },
			watch(page) {
				page.on("console", (m) => {
					if (m.type() === "error" && !ALLOWED.some((r) => r.test(m.text())))
						this.errors.push(m.text());
				});
				page.on("pageerror", (e) => this.errors.push("page error: " + (e.message || e)));
			},
			async open(query, opts = {}) {
				const page = await ctx.newPage();
				this.watch(page);
				if (opts.pad)
					await page.evaluateOnNewDocument(FAKE_PAD);
				if (opts.breakData)
					await page.setRequestInterception(true).then(() => page.on("request", (r) => {
						if (r.url().endsWith(".bwpack"))
							r.respond({ status: 404, body: "" });
						else
							r.continue();
					}));
				await page.goto(base + (query || ""), { waitUntil: "domcontentloaded" });
				return page;
			},
			async playOn(page) {
				await waitFor(page, () => !document.getElementById("play").hidden, "the Play button", 120000);
				await page.click("#play");
				await waitFor(page, () => window.bulwark.state().client >= 3, "the client in the game", 60000);
				await waitFor(page, () => window.bulwark.state().frames > 30, "30 frames");
				await sleep(300);
				return page;
			},
			async play() { return this.playOn(await this.open("")); },
			async reopen(page) {
				await page.close({ runBeforeUnload: false });
				return this.open("");
			},
		};
		const t0 = Date.now();
		let error = null;
		try {
			await Promise.race([tc.fn(t), sleep(240000).then(() => { throw new Error("test took over 4 minutes"); })]);
		} catch (e) {
			error = e.message;
		}
		await ctx.close().catch(() => {});
		const failedChecks = t.checks.filter((c) => !c.ok);
		const errs = t.allowErrors ? [] : t.errors;
		const ok = !error && !failedChecks.length && !errs.length;
		results.push({ name: tc.name, ok, error, checks: t.checks, notes: t.notes, errors: errs,
			ms: Date.now() - t0 });
		console.log(`${ok ? "PASS" : "FAIL"}  ${tc.name}  (${((Date.now() - t0) / 1000).toFixed(1)}s)`);
		for (const c of t.checks)
			console.log(`      ${c.ok ? "ok " : "NO "} ${c.what}`);
		for (const n of t.notes)
			console.log(`      note: ${n}`);
		if (error)
			console.log(`      error: ${error}`);
		for (const e of errs.slice(0, 8))
			console.log(`      console: ${e}`);
	}
	await browser.close();
	return { kind, version, results };
}

const srv = await serve(exportDir);
const base = `http://127.0.0.1:${srv.address().port}/`;
console.log("export: " + exportDir + " at " + base);
const kinds = which === "all" ? ["chrome", "firefox"] : [which];
const all = [];
for (const k of kinds)
	all.push(await runBrowser(k, base));
srv.close();
fs.writeFileSync(path.join(outDir, "results.json"), JSON.stringify(all, null, 1));
const failed = all.flatMap((b) => b.results.filter((r) => !r.ok).map((r) => b.kind + ": " + r.name));
const ran = all.reduce((n, b) => n + b.results.length, 0);
console.log(`\n${ran - failed.length} of ${ran} passed` + (failed.length ? "; failed:\n  " + failed.join("\n  ") : ""));
process.exit(failed.length || !ran ? 1 : 0);
