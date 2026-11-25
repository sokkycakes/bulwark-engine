#!/usr/bin/env python3
"""Bulwark Live on Steam: parties, room codes, invites, guests, 2v2 with a
party, cancelling, a full party, and a drop-in match, through bwlive-steam
and the steam-mock hub. The bridges are driven by their stdin commands, as
a game's Live messages would drive them.

Usage: steam_party.py path/to/services/live/target/release
"""
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time

BIN = os.path.abspath(sys.argv[1])
PORT = int(os.environ.get("STEAM_PORT", "29150"))
HUB = f"127.0.0.1:{PORT + 49}"
TMP = tempfile.mkdtemp(prefix="steamparty")
procs = []
failures = 0


class Bridge:
    def __init__(self, n, name, game):
        self.n, self.name = n, name
        self.id = 2000 + n
        d = os.path.join(TMP, name)
        os.makedirs(d)
        env = dict(os.environ, STEAM_MOCK_HUB=HUB, STEAM_MOCK_ACCOUNT=str(self.id), STEAM_MOCK_NAME=name)
        self.p = subprocess.Popen(
            [f"{BIN}/bwlive-steam", "--steam-lib", f"{BIN}/libsteam_mock.so", "--bind", f"127.0.0.1:{PORT + n}", "--game", game],
            cwd=d, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        procs.append(self.p)
        self.lines = []
        self.q = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip())
            self.q.put(line.rstrip())

    def cmd(self, c):
        self.p.stdin.write(c + "\n")
        self.p.stdin.flush()

    def wait(self, pattern, secs=20):
        """The first line from now on matching pattern, or None."""
        rx = re.compile(pattern)
        end = time.time() + secs
        while time.time() < end:
            try:
                line = self.q.get(timeout=0.2)
            except queue.Empty:
                continue
            m = rx.search(line)
            if m:
                return m
        return None

    def status(self, pattern, secs=10):
        end = time.time() + secs
        while time.time() < end:
            self.cmd("status")
            m = self.wait(pattern, 1.0)
            if m:
                return m
        return None


def check(what, ok):
    global failures
    print(f"steam party: {what}: {'ok' if ok else 'FAIL'}")
    if not ok:
        failures += 1


hub = subprocess.Popen([f"{BIN}/steam-mock-hub", HUB], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
procs.append(hub)
time.sleep(0.3)
try:
    # --- party by room code, then 2v2 against two solo players ---------------
    ann, bo, cy, di = (Bridge(i, n, "stiletto") for i, n in enumerate(["ann", "bo", "cy", "di"]))
    ann.cmd("party 4 1")
    m = ann.wait(r"in party ([A-Z2-9]{6}) \(1 of 4\)")
    check("a party gets a room code", m is not None)
    code = m.group(1) if m else "XXXXXX"
    bo.cmd(f"join {code.lower()}")
    check("joining by room code (any case)", bo.wait(rf"in party {code} \(2 of 4\)") is not None)
    check("the leader sees the member", ann.status(rf"party {code} \(2 consoles\), leader {ann.id}") is not None)
    bo.cmd("queue 2v2")
    check("only the leader may queue", bo.wait("only the party leader") is not None)
    ann.cmd("queue 2v2")
    time.sleep(1)
    cy.cmd("queue 2v2")
    di.cmd("queue 2v2")
    got = [x.wait(r"match (\d+) on 2v2.*4 seats", 30) for x in (ann, bo, cy, di)]
    check("2v2: all four matched, the party's member through the party lobby", all(got))
    if all(got):
        check("2v2: one match", len({g.group(1) for g in got}) == 1)
        m = ann.status(r"seats (.*)")
        seats = dict((int(a), int(t)) for a, _, t in (s.split(":") for s in m.group(1).split())) if m else {}
        check("2v2: the party plays on one team", seats.get(ann.id) is not None and seats.get(ann.id) == seats.get(bo.id))
        check("2v2: the solo players make the other", seats.get(cy.id) == seats.get(di.id) != seats.get(ann.id))
        check("2v2: the longest-waiting party's leader hosts", ann.status(rf"host {ann.id},") is not None)

    # --- invites into a friends-only party; a full party ----------------------
    ed, fay, gus = (Bridge(i, n, "stiletto") for i, n in [(4, "ed"), (5, "fay"), (6, "gus")])
    ed.cmd("party 2 0")
    m = ed.wait(r"in party ([A-Z2-9]{6})")
    code = m.group(1) if m else "XXXXXX"
    gus.cmd(f"join {code}")
    check("a friends-only party isn't found by its code alone", gus.wait("no such room code") is not None)
    ed.cmd(f"invite {fay.id}")
    check("the invite arrives with the code", fay.wait(rf"ed invited you to party {code}") is not None)
    fay.cmd(f"join {code}")
    check("an invite lets you in", fay.wait(rf"in party {code} \(2 of 2\)") is not None)
    ed.cmd("open 1")
    time.sleep(0.5)
    gus.cmd(f"join {code}")
    # Steam's lobby search leaves full lobbies out, so the code isn't found
    check("a full party turns the next one away", gus.wait("full|no such room code") is not None)

    # --- guests count toward the party's size ---------------------------------
    hal, ivy = Bridge(7, "hal", "stiletto"), Bridge(8, "ivy", "stiletto")
    hal.cmd("party 3 1")
    m = hal.wait(r"in party ([A-Z2-9]{6})")
    code = m.group(1) if m else "XXXXXX"
    ivy.cmd("guests ivy2 ivy3")
    time.sleep(0.3)
    ivy.cmd(f"join {code}")
    check("a console with two guests doesn't fit a party of 3 with one there", ivy.wait("full") is not None)

    # --- cancelling a search --------------------------------------------------
    jo, kim = Bridge(9, "jo", "stiletto"), Bridge(10, "kim", "stiletto")
    jo.cmd("queue duel")
    jo.wait("waiting in duel")
    jo.cmd("cancel")
    time.sleep(0.5)
    kim.cmd("queue duel")
    check("a cancelled search isn't matched", kim.wait(r"match \d+ on duel", 8) is None)
    jo.cmd("queue duel")
    check("searching again is", jo.wait(r"match \d+ on duel", 15) is not None and kim.wait(r"match \d+ on duel", 5) is not None)

    # --- drop-in: Horde starts alone, a second player drops in, then leaves ----
    lou, max_ = Bridge(11, "lou", "coop"), Bridge(12, "max", "coop")
    lou.cmd("queue horde")
    m = lou.wait(r"match (\d+) on horde.*1 seats", 25)
    check("Horde starts alone after its last stage", m is not None)
    max_.cmd("queue horde")
    check("a later player drops into the running match", max_.wait(rf"match {m.group(1) if m else 0} on horde.*2 seats", 15) is not None)
    check("the host hears about it", lou.wait(r"on horde.*2 seats", 10) is not None)
    max_.cmd("left")
    check("leaving frees the seat", lou.wait(r"on horde.*1 seats", 10) is not None)

    # --- leaving a party ------------------------------------------------------
    bo.cmd("leave")
    check("the leader sees a member leave", ann.status(r"\(1 consoles\)") is not None)
finally:
    for p in procs:
        p.kill()
print(f"steam party: {failures} failed" if failures else "steam party: all ok")
sys.exit(1 if failures else 0)
