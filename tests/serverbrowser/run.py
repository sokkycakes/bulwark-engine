#!/usr/bin/env python3
"""The server browser end to end: a master, real dedicated servers and a
windowed client driven through the Panorama page with controller keys.

  python3 tests/serverbrowser/run.py --build build --basedir /path/to/bw \
      [--games base,stbuilt] [--display :99] [--shots out-dir]

--basedir is a Stiletto test base dir as docs/handoff-stiletto.md sets up
(base linked to the nuclide checkout, stbuilt holding compiled progs). The
script makes its own write dirs under it (sbtest-*) and removes nothing
else. It needs Xvfb running on --display, or a real display.

Checks: the master lists our game's servers only, the bridge's sb_list and
sb_details carry the rows, players and rules, the page joins a server with
GP_A, a locked server bounces a wrong password back to the browser with the
reason and lets the right one in, favorites and history are kept.
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures += 1


def free_udp(start):
    p = start
    while True:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("0.0.0.0", p))
            s.close()
            return p
        except OSError:
            p += 1


def json_lines(log, marker):
    """the JSON object printed after an 'echo <marker>' line"""
    out = []
    lines = log.splitlines()
    for i, l in enumerate(lines):
        if l.strip() != marker:
            continue
        # the next line that is a JSON object (other output can come between)
        for nxt in lines[i + 1:i + 40]:
            if nxt.startswith("{"):
                try:
                    out.append(json.loads(nxt))
                    break
                except ValueError:
                    pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--basedir", required=True)
    ap.add_argument("--games", default="base,stbuilt")
    ap.add_argument("--display", default=os.environ.get("DISPLAY", ":99"))
    ap.add_argument("--shots", default="")
    a = ap.parse_args()

    ded = os.path.join(a.build, "bulwark-ded")
    cli = os.path.join(a.build, "bulwark")
    B = os.path.abspath(a.basedir)
    games = []
    for g in a.games.split(","):
        games += ["-game", g]
    procs = []
    mport = free_udp(27960)
    sport = free_udp(27600)
    master = subprocess.Popen([sys.executable, os.path.join(ROOT, "services", "master", "bwmaster.py"),
                               "--port", str(mport), "--bind", "127.0.0.1"],
                              stdout=open(os.path.join(B, "sbtest-master.log"), "w"), stderr=subprocess.STDOUT)
    procs.append(master)
    time.sleep(0.4)
    common = ["+set", "com_protocolname", "StilettoSwift", "+set", "sv_public", "1",
              "+set", "sv_master1", f"127.0.0.1:{mport}", "+set", "sv_link_port", "0"]
    servers = [
        ("duel", f"SBTest Duel", sport, ["+set", "g_gametype", "duel", "+map", "duel_alley"]),
        ("locked", "SBTest Locked", sport + 1, ["+set", "g_gametype", "deathmatch", "+set", "sv_password", "hunter2",
                                                "+map", "aerowalk"]),
        ("bots", "SBTest Bots", sport + 2, ["+set", "g_gametype", "deathmatch", "+map", "aerowalk",
                                            "+in", "3", "sv addBot Marco", "+in", "4", "sv addBot Max"]),
        ("other", "SBTest Other Game", sport + 3, ["+set", "com_protocolname", "OtherGame", "+map", "duel_alley"]),
    ]
    for key, name, port, extra in servers:
        wd = f"sbtest-{key}"
        os.makedirs(os.path.join(B, wd), exist_ok=True)
        args = [ded, "-basedir", B] + games + ["-game", wd] + common + ["+set", "hostname", name,
                                                                         "+set", "sv_port", str(port)] + extra
        procs.append(subprocess.Popen(args, stdout=open(os.path.join(B, wd, "log.txt"), "w"),
                                      stderr=subprocess.STDOUT, cwd=B))
    try:
        time.sleep(8)
        mlog = open(os.path.join(B, "sbtest-master.log")).read()
        check(mlog.count("listed ") >= 4, "all four servers heartbeat the master and answer its getinfo")

        duel, locked, bots = (f"127.0.0.1:{sport}", f"127.0.0.1:{sport + 1}", f"127.0.0.1:{sport + 2}")
        cdir = os.path.join(B, "sbtest-client")
        shutil.rmtree(cdir, ignore_errors=True)
        os.makedirs(cdir)
        # the page lists by ping, so the test picks rows by filters instead:
        # Mode is the first chip; the locked server is the only deathmatch
        # one without bots, so "Empty: Hide" then Mode leaves predictable rows
        cfg = f"""
set net_master1 127.0.0.1:{mport}
set name "SBTester"
set sb_favorites ""
set sb_history ""
in 3 sb_query sb_refresh:internet
in 6 echo SBLIST
in 6.1 sb_query sb_list
in 6.5 sb_query sb_details:{bots}
in 7.5 echo SBDETAILS
in 7.6 sb_query sb_details:{bots}
in 8 sb_query sb_favorite:{bots}
in 8.5 menu_webcore servers
in 11 screenshot sbtest_list
// deathmatch only, empty ones hidden: the bots server is the one row
in 11.5 ui_key GP_DPAD_UP
in 11.7 ui_key GP_A
in 12 ui_key GP_DPAD_RIGHT
in 12.2 ui_key GP_A
in 12.4 ui_key GP_DPAD_DOWN
in 12.8 screenshot sbtest_bots_only
in 13 ui_key GP_A
in 19 screenshot sbtest_ingame
in 19.5 echo SBSTATE1
in 19.6 sb_query sb_list
in 20.5 disconnect
in 22 menu_webcore servers
// deathmatch, sorted by name: Bots, Locked
in 25.0 ui_key GP_DPAD_UP
in 25.2 ui_key GP_A
in 25.4 ui_key GP_DPAD_RIGHT
in 25.5 ui_key GP_DPAD_RIGHT
in 25.6 ui_key GP_DPAD_RIGHT
in 25.7 ui_key GP_DPAD_RIGHT
in 25.9 ui_key GP_A
in 26.1 ui_key GP_A
in 26.3 ui_key GP_DPAD_DOWN
in 26.5 ui_key HOME
in 26.7 ui_key GP_DPAD_DOWN
in 27.0 screenshot sbtest_locked_wrong
in 27.2 ui_key GP_A
in 27.8 ui_key x
in 27.9 ui_key x
in 28.2 screenshot sbtest_password
in 28.5 ui_key GP_A
in 33 screenshot sbtest_refused
in 34.0 ui_key GP_DPAD_UP
in 34.2 ui_key GP_A
in 34.4 ui_key GP_DPAD_RIGHT
in 34.5 ui_key GP_DPAD_RIGHT
in 34.6 ui_key GP_DPAD_RIGHT
in 34.7 ui_key GP_DPAD_RIGHT
in 34.9 ui_key GP_A
in 35.1 ui_key GP_A
in 35.3 ui_key GP_DPAD_DOWN
in 35.5 ui_key HOME
in 35.7 ui_key GP_DPAD_DOWN
in 36.0 screenshot sbtest_locked_right
in 36.2 ui_key GP_A
in 36.8 ui_key h
in 36.9 ui_key u
in 37.0 ui_key n
in 37.1 ui_key t
in 37.2 ui_key e
in 37.3 ui_key r
in 37.4 ui_key 2
in 37.7 ui_key GP_A
in 44 screenshot sbtest_locked_ingame
in 44.5 echo SBSTATE2
in 44.6 sb_query sb_list
in 45 echo SBCVARS
in 45.1 cvarlist sb_
in 46 quit
"""
        open(os.path.join(cdir, "sbtest.cfg"), "w").write(cfg)
        env = dict(os.environ, DISPLAY=a.display)
        out = subprocess.run([cli, "-basedir", B] + games + ["-game", "sbtest-client", "-width", "1280", "-height", "720",
                              "+set", "com_protocolname", "StilettoSwift", "+set", "webcore_menumap", "",
                              "+exec", "sbtest.cfg"], env=env, cwd=B, capture_output=True, text=True, timeout=120)
        log = out.stdout + out.stderr
        open(os.path.join(cdir, "log.txt"), "w").write(log)

        lst = (json_lines(log, "SBLIST") or [None])[0] or {}
        names = sorted(s["name"] for s in lst.get("servers", []) if s.get("internet"))
        check(names == ["SBTest Bots", "SBTest Duel", "SBTest Locked"], f"sb_list: our game's three servers {names}")
        check(all(s["name"] != "SBTest Other Game" for s in lst.get("servers", [])),
              "the master leaves out the other game's server")
        rows = {s["addr"]: s for s in lst.get("servers", [])}
        check(rows.get(locked, {}).get("password") is True, "the locked server says so")
        check(rows.get(duel, {}).get("gametype") == "duel", "the duel server's mode comes through")
        check(rows.get(duel, {}).get("state") == "WARMUP", "Stiletto's arena state comes through")
        det = (json_lines(log, "SBDETAILS") or [None])[0] or {}
        pl = sorted(p["name"] for p in det.get("player_list", []))
        check(pl == ["Marco", "Max"], f"sb_details: the bots server's players {pl}")
        check(det.get("rules", {}).get("hostname") == "SBTest Bots", "sb_details: rules")

        st1 = (json_lines(log, "SBSTATE1") or [None])[0] or {}
        check(st1.get("connected") and st1.get("server") == bots, f"GP_A joined the bots server ({st1.get('server')})")
        check(re.search(r"server refused connection: .*password", log) is not None, "a wrong password is refused")
        check(f"server browser: couldn't join {locked}" in log, "the refusal brings the browser back with the reason")
        st2 = (json_lines(log, "SBSTATE2") or [None])[0] or {}
        check(st2.get("connected") and st2.get("server") == locked, f"the right password gets in ({st2.get('server')})")
        cv = log[log.rfind("SBCVARS"):]
        m = re.search(r'sb_history "([^"]*)"', cv)
        hist = m.group(1).split() if m else []
        check(hist[:2] == [locked, bots], f"history, newest first: {hist}")
        m = re.search(r'sb_favorites "([^"]*)"', cv)
        check(m and m.group(1) == bots, "favorites kept")
        if a.shots:
            os.makedirs(a.shots, exist_ok=True)
            sdir = os.path.join(cdir, "screenshots")
            for f in sorted(os.listdir(sdir)) if os.path.isdir(sdir) else []:
                shutil.copy(os.path.join(sdir, f), a.shots)
    finally:
        for p in procs:
            p.terminate()
    print("serverbrowser: %s" % ("all passed" if not failures else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
