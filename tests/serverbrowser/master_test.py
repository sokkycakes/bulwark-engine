#!/usr/bin/env python3
"""bwmaster on its own: fake servers heartbeat it, a fake client lists them.

  python3 tests/serverbrowser/master_test.py

Checks: a server is listed only after answering getinfo with the master's
challenge, game and protocol filters, the empty / full flags, a forged
answer is ignored, and a long list is split over several packets.
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MASTER = os.path.join(HERE, "..", "..", "services", "master", "bwmaster.py")
OOB = b"\xff\xff\xff\xff"
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FakeServer:
    def __init__(self, info, answer=True, forge=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(2)
        self.port = self.sock.getsockname()[1]
        self.info, self.answer, self.forge = info, answer, forge

    def heartbeat(self, master):
        self.sock.sendto(OOB + b"heartbeat DarkPlaces\n", master)
        data, addr = self.sock.recvfrom(2048)
        assert data.startswith(OOB + b"getinfo "), data
        challenge = data[len(OOB) + 8:].decode().strip()
        if not self.answer:
            return
        if self.forge:
            challenge = "not-the-challenge"
        info = "".join(f"\\{k}\\{v}" for k, v in self.info.items()) + f"\\challenge\\{challenge}"
        self.sock.sendto(OOB + b"infoResponse\n" + info.encode(), master)


def list_servers(master, query, ext=False):
    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    c.settimeout(1)
    c.sendto(OOB + query.encode(), master)
    ports, packets = set(), 0
    while True:
        try:
            data, _ = c.recvfrom(4096)
        except socket.timeout:
            break
        packets += 1
        head = b"getserversExtResponse" if ext else b"getserversResponse"
        assert data.startswith(OOB + head), data[:40]
        body = data[len(OOB) + len(head):]
        i = 0
        while i < len(body):
            if body[i:i + 4] == b"\\EOT":
                break
            if body[i:i + 1] == b"\\":
                ports.add(int.from_bytes(body[i + 5:i + 7], "big"))
                i += 7
            elif body[i:i + 1] == b"/":
                ports.add(int.from_bytes(body[i + 17:i + 19], "big"))
                i += 19
            else:
                break
        if b"\\EOT" in body:
            break
    return ports, packets


def main():
    port = free_port()
    proc = subprocess.Popen([sys.executable, MASTER, "--port", str(port), "--bind", "127.0.0.1", "--quiet",
                             "--max-per-ip", "500"])
    master = ("127.0.0.1", port)
    time.sleep(0.5)
    try:
        base = {"gamename": "StilettoSwift", "protocol": "2", "clients": "1", "sv_maxclients": "8", "hostname": "a"}
        duel = FakeServer(dict(base))
        empty = FakeServer(dict(base, clients="0"))
        full = FakeServer(dict(base, clients="8"))
        other = FakeServer(dict(base, gamename="OtherGame"))
        oldproto = FakeServer(dict(base, protocol="1"))
        silent = FakeServer(dict(base), answer=False)
        forged = FakeServer(dict(base), forge=True)
        for s in (duel, empty, full, other, oldproto, silent, forged):
            s.heartbeat(master)
        time.sleep(0.3)

        got, _ = list_servers(master, "getservers StilettoSwift 2")
        check(got == {duel.port}, "plain getservers: only the server with players, right game and protocol")
        got, _ = list_servers(master, "getservers StilettoSwift 2 empty full")
        check(got == {duel.port, empty.port, full.port}, "empty full: adds the empty and the full server")
        check(silent.port not in got, "a server that never answered getinfo isn't listed")
        check(forged.port not in got, "an answer with the wrong challenge isn't listed")
        got, _ = list_servers(master, "getservers OtherGame 2 empty full")
        check(got == {other.port}, "another game's list")
        got, _ = list_servers(master, "getservers StilettoSwift 1 empty full")
        check(got == {oldproto.port}, "protocol filter")
        got, _ = list_servers(master, "getserversExt StilettoSwift 2 empty full ipv4", ext=True)
        check(got == {duel.port, empty.port, full.port}, "getserversExt with ipv4")

        many = [FakeServer(dict(base)) for _ in range(250)]
        for s in many:
            s.heartbeat(master)
        time.sleep(0.5)
        got, packets = list_servers(master, "getservers StilettoSwift 2 empty full")
        check(len(got) == 253 and packets >= 2, f"253 servers over {packets} packets")
    finally:
        proc.terminate()
    print("master_test: %s" % ("all passed" if not failures else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
