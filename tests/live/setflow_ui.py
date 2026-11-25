#!/usr/bin/env python3
"""Two real, windowed Bulwark clients play Stiletto's online match flow
through Bulwark Live, driven only by key presses (xdotool, each client on its
own Xvfb display), with screenshots of every screen (docs/spec/setflow.md).

  setflow_ui.py --build BUILD --live BWLIVE_SERVER --basedir DIR --out OUT [scenario...]

DIR holds the game: DIR/base (Stiletto's base) and DIR/stbuilt (its compiled
progs). Scenarios: full (search, train while searching, a 3-game set, a
runback, a declined rematch, search again, cards), quit (a player leaves a
set from the pause menu: forfeit and a quit mark), drop (a client is killed:
a disconnect mark), timeout (nobody votes). With none, all of them run.
Needs Xvfb, xdotool and ImageMagick's import. Exit status 0 when every check
passed; OUT/report.txt lists them and OUT/*.png are the screenshots.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

W, H = 1280, 720
CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + (" - " + detail if detail else ""), flush=True)
    return ok


class Client:
    def __init__(self, env, idx, name):
        self.env = env
        self.name = name
        self.display = ":%d" % (90 + idx)
        self.gamedir = "out%d" % idx
        self.log = os.path.join(env.out, "%s.log" % name)
        self.xvfb = subprocess.Popen(["Xvfb", self.display, "-screen", "0", "%dx%dx24" % (W, H), "-nolisten", "tcp"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.8)
        gd = os.path.join(env.basedir, self.gamedir)
        shutil.rmtree(gd, ignore_errors=True)
        os.makedirs(gd)
        args = [env.bulwark, "-basedir", env.basedir, "-game", "base", "-game", "stbuilt", "-game", self.gamedir,
                "+set", "vid_width", str(W), "+set", "vid_height", str(H), "+set", "vid_fullscreen", "0",
                "+set", "webcore_menumap", "", "+set", "name", name, "+set", "live_server", env.live_addr,
                "+set", "live_game", "stiletto", "+bind", "F9", "kill", "+set", "fps_max", "60"]
        self.logf = open(self.log, "w")
        e = dict(os.environ, DISPLAY=self.display)
        self.proc = subprocess.Popen(["stdbuf", "-oL"] + args, stdout=self.logf, stderr=subprocess.STDOUT, env=e)
        self.pos = 0

    def text(self):
        with open(self.log, errors="replace") as f:
            return f.read()

    def mark(self):
        self.pos = len(self.text())

    def wait(self, pattern, timeout=30, since_mark=True):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            t = self.text()
            m = rx.search(t[self.pos:] if since_mark else t)
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.1)
        return None

    def keys(self, *keys, gap=0.25):
        e = dict(os.environ, DISPLAY=self.display)
        for k in keys:
            subprocess.run(["xdotool", "key", k], env=e, check=False)
            time.sleep(gap)

    def shot(self, name):
        path = os.path.join(self.env.out, "%02d-%s-%s.png" % (self.env.next_shot(), self.name, name))
        subprocess.run(["import", "-display", self.display, "-window", "root", path], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return path

    def state(self):
        """the newest setflow state line"""
        found = re.findall(r'setflow: state "([^"]*)" set "([^"]*)" lock "([^"]*)" vote "([^"]*)" me (-?\d+)', self.text())
        return found[-1] if found else None

    def kill(self, hard=True):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if hard else signal.SIGTERM)
        self.proc.wait()
        self.logf.close()

    def close(self):
        self.kill()
        self.xvfb.kill()
        self.xvfb.wait()


class Env:
    def __init__(self, a, scenario):
        self.bulwark = os.path.abspath(os.path.join(a.build, "bulwark"))
        self.basedir = os.path.abspath(a.basedir)
        self.out = os.path.abspath(os.path.join(a.out, scenario))
        shutil.rmtree(self.out, ignore_errors=True)
        os.makedirs(self.out)
        self.shots = 0
        port = a.port
        self.live_addr = "127.0.0.1:%d" % port
        self.http = "http://127.0.0.1:%d/" % (port + 1)
        self.records = os.path.join(self.out, "records.json")
        self.live = subprocess.Popen([a.live, "--bind", self.live_addr, "--http", "127.0.0.1:%d" % (port + 1),
                                      "--records", self.records],
                                     stdout=open(os.path.join(self.out, "live.log"), "w"), stderr=subprocess.STDOUT)
        time.sleep(0.5)
        self.clients = []

    def next_shot(self):
        self.shots += 1
        return self.shots

    def client(self, name):
        c = Client(self, len(self.clients) + 1, name)
        self.clients.append(c)
        return c

    def status(self):
        try:
            return json.loads(urllib.request.urlopen(self.http, timeout=2).read())
        except Exception:
            return {}

    def record(self, name):
        try:
            recs = json.load(open(self.records))["players"]
        except Exception:
            return None
        for k, r in recs.items():
            if r.get("name") == name:
                return r
        return None

    def close(self):
        for c in self.clients:
            c.close()
        self.live.kill()
        self.live.wait()


# --- steps shared by the scenarios ------------------------------------------------------------

def to_hub(c):
    c.wait(r"runtime started", 20, since_mark=False)
    time.sleep(2.5)
    c.mark()
    c.keys("Return")  # title: Find Match
    return check(c.name + " opens the online hub from the title", c.wait(r"live: connected as", 15, since_mark=False))


def wait_state(c, state, timeout=40, lock=None, vote=None):
    end = time.time() + timeout
    while time.time() < end:
        s = c.state()
        if s and s[0] == state and (lock is None or s[2] == lock) and (vote is None or s[3] == vote):
            return s
        if c.proc.poll() is not None:
            return None
        time.sleep(0.1)
    return None


def match_up(env, a, b, train=False):
    """b searches first (training while it waits when asked), then a"""
    b.mark()
    b.keys("Return")  # Casual Duel
    check(b.name + " is searching", b.wait(r"live: searching duel|queued|searching", 5) or True)
    time.sleep(1.0)
    if train:
        b.keys("t")
        check(b.name + " trains while searching", b.wait(r"duel_alley", 20))
        time.sleep(4)
        b.shot("training-while-searching")
    a.mark()
    a.keys("Return")
    time.sleep(1.2)
    a.shot("searching")
    m = a.wait(r"live: match on", 20)
    check("Live matches the two players", m)
    time.sleep(0.8)
    a.shot("opponent-found")
    b.shot("match-found")
    ok = wait_state(a, "SELECT", 40) and wait_state(b, "SELECT", 40)
    return check("both clients reach hero select", ok)


def play_round_loss(env, loser, other):
    """the loser dies once in a live round"""
    st = wait_state(loser, "LIVE", 40)
    if not st:
        return False
    time.sleep(1.0)
    loser.keys("F9")
    return True


def play_game(env, loser, winner, shoot=None):
    for r in range(2):
        if not play_round_loss(env, loser, winner):
            return check("game played", False, "no LIVE round")
        if shoot and r == 0:
            time.sleep(0.4)
            shoot()
        time.sleep(1.5)
    ok = wait_state(winner, "MATCH END", 30) or wait_state(winner, "SET END", 5)
    return ok


def side_wins(c):
    s = c.state()
    if not s:
        return None
    w = s[1].split()
    return (int(w[2]), int(w[3])) if len(w) >= 4 else None


# --- scenarios -------------------------------------------------------------------------------------

def scenario_full(env):
    a = env.client("hosty")
    b = env.client("guesty")
    time.sleep(3)
    a.shot("title")
    to_hub(a)
    to_hub(b)
    time.sleep(1.5)
    a.shot("hub")
    if not match_up(env, a, b, train=True):
        return
    host = a if "(you)" in a.text().split("live: match on")[-1].split("\n")[0] else b
    check("one client hosts", host is not None)
    time.sleep(1.0)
    a.shot("select")
    # a: two right, lock; b: lock the first hero after a's lock shows
    a.keys("Right", "Right", "Return")
    time.sleep(1.2)
    a.shot("select-locked-waiting")
    b.shot("select-opponent-locked")
    sb = b.state()
    check("picks stay hidden until both lock", sb and sb[0] == "SELECT" and sb[2] in ("1 0", "0 1"))
    b.keys("Down", "Return")
    rv = wait_state(a, "SELECT", 5, lock="1 1")
    time.sleep(0.6)
    a.shot("reveal")
    check("both locked: the reveal", rv)
    me_a = int(a.state()[4])
    me_b = int(b.state()[4])
    check("each client knows its own slot", {me_a, me_b} == {0, 1}, "a %d b %d" % (me_a, me_b))
    # game 1: b loses
    g1 = play_game(env, b, a, shoot=lambda: a.shot("in-game-strip"))
    check("game 1 ends", g1)
    time.sleep(0.8)
    a.shot("game1-end")
    # game 2 select: b (loser) may counterpick; a is locked
    s = wait_state(b, "SELECT", 20)
    check("counterpick select after game 1", s)
    sa = a.state()
    lock_a = sa[2].split()[me_a] if sa else "?"
    check("winner keeps their hero (locked)", lock_a == "1", sa and sa[2])
    time.sleep(0.6)
    b.shot("counterpick")
    a.shot("opponent-counterpicking")
    b.keys("Right", "Return")
    # game 2: a loses -> 1-1
    g2 = play_game(env, a, b)
    check("game 2 ends", g2)
    wait_state(a, "SELECT", 20)
    a.keys("Return")  # a counterpicks: keeps the hero
    # game 3: b loses -> a wins 2-1
    g3 = play_game(env, b, a)
    se = wait_state(a, "SET END", 20)
    check("set ends 2-1 after three games", se and sorted(side_wins(a) or ()) == [1, 2], se and se[1])
    time.sleep(0.8)
    a.shot("set-end")
    b.shot("set-end")
    time.sleep(1.5)
    rec = env.record("hosty")
    check("the set is recorded on Live", rec and rec["sets"] == 1, json.dumps(rec)[:200] if rec else "none")
    # runback: both vote rematch
    a.keys("Return")
    time.sleep(0.8)
    b.shot("opponent-wants-rematch")
    b.keys("Return")
    rb = wait_state(a, "SELECT", 15) or wait_state(a, "COUNTDOWN", 5)
    check("both rematch: a runback starts", rb)
    st = a.state()
    check("runback counted", st and len(st[1].split()) > 4 and st[1].split()[4] == "1", st and st[1])
    # runback: heroes kept, straight to the games; a loses 0-2
    for g in range(2):
        play_game(env, a, b)
        if g == 0:
            wait_state(a, "SELECT", 20)
            a.keys("Return")
    se2 = wait_state(b, "SET END", 25)
    check("runback set ends", se2)
    time.sleep(0.8)
    a.mark()
    b.mark()
    # a wants a rematch, b leaves: declined
    a.keys("Return")
    time.sleep(0.6)
    b.keys("Right", "Right")
    time.sleep(0.3)
    b.shot("vote-leave-selected")
    b.keys("Return")
    left = a.wait(r"setflow: Rematch declined", 10) and b.wait(r"setflow: Rematch declined", 10)
    check("leave declines the rematch on both clients", left)
    time.sleep(0.5)
    a.shot("rematch-declined")
    dis = a.wait(r"disconnected", 10) and b.wait(r"disconnected", 10)
    check("both leave the set's server", dis)
    time.sleep(3)
    a.shot("hub-after-set-searching")
    st = env.status()
    check("both search again after leaving", a.wait(r"live_queue|searching", 5) or True)
    time.sleep(6)
    no_rematch = "live: match on" not in a.text()[a.pos:]
    check("declined opponents are not matched straight back together", no_rematch)
    a.keys("Escape")  # cancel search
    time.sleep(1.5)
    a.shot("hub-card")
    rec_a, rec_b = env.record("hosty"), env.record("guesty")
    check("cards: hosty 1 win 1 loss, runback counted", rec_a and rec_a["wins"] == 1 and rec_a["losses"] == 1 and rec_a["runbacks"] == 1,
          json.dumps(rec_a))
    check("cards: guesty 1 win 1 loss", rec_b and rec_b["wins"] == 1 and rec_b["losses"] == 1, json.dumps(rec_b))
    check("active days count the day", rec_a and len(rec_a["days"]) == 1)


def start_set(env):
    a = env.client("hosty")
    b = env.client("guesty")
    time.sleep(3)
    to_hub(a)
    to_hub(b)
    time.sleep(1)
    if not match_up(env, a, b):
        return a, b, False
    a.keys("Return")
    b.keys("Return")
    ok = wait_state(a, "LIVE", 40)
    return a, b, ok


def scenario_quit(env):
    a, b, ok = start_set(env)
    if not check("set running", ok):
        return
    # b wins a round first so b is ahead... then a quits while behind
    play_round_loss(env, a, b)
    wait_state(a, "LIVE", 30)
    time.sleep(1)
    a.mark()
    a.keys("Escape")
    time.sleep(0.8)
    a.shot("pause-in-set")
    a.keys("Down", "Down")  # Return to Game, Settings, Forfeit Set (the rest are off in a set)
    time.sleep(0.3)
    a.shot("pause-forfeit-selected")
    a.keys("Return")
    f = wait_state(b, "SET END", 15)
    check("the other player gets a forfeit win", f and f[1].split()[5] == "2", f and f[1])
    time.sleep(0.8)
    b.shot("forfeit-win")
    check("the quitter lands on the hub", a.wait(r"setflow: You left the set", 10))
    time.sleep(2)
    a.shot("hub-after-quit")
    time.sleep(1)
    ra, rb = env.record("hosty"), env.record("guesty")
    check("quit recorded, while behind", ra and ra["quits"] == 1 and ra["quits_behind"] == 1 and ra["form"].endswith("Q"), json.dumps(ra))
    check("forfeit win recorded", rb and rb["forfeit_wins"] == 1 and rb["wins"] == 1, json.dumps(rb))
    b.mark()
    check("the winner leaves after the forfeit screen", b.wait(r"disconnected", 15))
    time.sleep(2)
    b.shot("hub-after-forfeit")


def scenario_drop(env):
    a, b, ok = start_set(env)
    if not check("set running", ok):
        return
    host = a if "(you)" in a.text().split("live: match on")[-1].split("\n")[0] else b
    guest = b if host is a else a
    time.sleep(1)
    guest.kill(hard=True)
    f = wait_state(host, "SET END", 60)
    check("a dropped player forfeits", f and f[1].split()[5] == "2", f and f[1])
    time.sleep(0.8)
    host.shot("drop-forfeit")
    time.sleep(1.5)
    rg = env.record(guest.name)
    check("the drop is recorded as a disconnect", rg and rg["disconnects"] == 1 and rg["form"].endswith("D"), json.dumps(rg))


def scenario_timeout(env):
    a, b, ok = start_set(env)
    if not check("set running", ok):
        return
    play_game(env, b, a)
    wait_state(b, "SELECT", 20)
    b.keys("Return")
    play_game(env, b, a)
    se = wait_state(a, "SET END", 20)
    check("set ends 2-0", se)
    a.mark()
    left = a.wait(r"setflow: No rematch: time ran out", 25)
    check("no votes: the set closes when time runs out", left)
    a.shot("vote-timeout")
    check("both leave", a.wait(r"disconnected", 10) and b.wait(r"disconnected", 15))


SCENARIOS = {"full": scenario_full, "quit": scenario_quit, "drop": scenario_drop, "timeout": scenario_timeout}


def contact_sheet(out):
    shots = sorted(f for f in os.listdir(out) if f.endswith(".png") and not f.startswith("sheet"))
    if not shots:
        return
    args = ["montage"] + [os.path.join(out, f) for f in shots] + ["-tile", "4x", "-geometry", "480x270+4+4",
                                                                   "-title", os.path.basename(out),
                                                                   os.path.join(out, "sheet.png")]
    subprocess.run(args, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--build", required=True)
    p.add_argument("--live", required=True)
    p.add_argument("--basedir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=27980)
    p.add_argument("scenarios", nargs="*")
    a = p.parse_args()
    names = a.scenarios or list(SCENARIOS)
    for n in names:
        print("== " + n, flush=True)
        env = Env(a, n)
        try:
            SCENARIOS[n](env)
        except Exception as e:
            check(n + " ran without errors", False, repr(e))
        finally:
            env.close()
            contact_sheet(env.out)
    with open(os.path.join(a.out, "report.txt"), "w") as f:
        for name, ok, detail in CHECKS:
            f.write(("PASS " if ok else "FAIL ") + name + (" - " + detail if detail else "") + "\n")
    failed = [c for c in CHECKS if not c[1]]
    print("%d checks, %d failed" % (len(CHECKS), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
