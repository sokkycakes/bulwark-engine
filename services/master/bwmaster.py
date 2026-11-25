#!/usr/bin/env python3
"""bwmaster: a small master server for Bulwark and Stiletto servers.

It speaks the DarkPlaces master protocol as dpmaster documents it, which is
what Bulwark's sv_master.c and cl_master.c use, and what FTEQW and
DarkPlaces servers and clients speak too:

  server -> master   "heartbeat DarkPlaces\\n"
  master -> server   "getinfo <challenge>"
  server -> master   "infoResponse\\n\\\\key\\\\value..."  (echoes the challenge)
  client -> master   "getservers <game> <protocol> [empty] [full]"
                     "getserversExt <game> <protocol> [empty] [full] [ipv4] [ipv6]"
  master -> client   "getserversResponse" + ("\\\\" + 4 byte ip + 2 byte port)...
                     + "\\\\EOT\\0\\0\\0", split over several packets if needed
                     ("getserversExtResponse" also uses "/" + 16 byte ip6 + port)

A server is listed only after it has answered a getinfo with our challenge,
so a forged heartbeat can't list someone else's address. It is dropped
when it stops answering (no heartbeat or info for --timeout seconds).

ISC licence, see LICENSE. Written from dpmaster's protocol notes, no code
taken from any engine or master.
"""
import argparse
import ipaddress
import json
import os
import secrets
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OOB = b"\xff\xff\xff\xff"
MAX_PACKET = 1400


def parse_info(text):
    """'\\k\\v\\k2\\v2' -> dict"""
    parts = text.split("\\")
    if parts and parts[0] == "":
        parts = parts[1:]
    return {parts[i]: parts[i + 1] for i in range(0, len(parts) - 1, 2)}


class Server:
    __slots__ = ("addr", "info", "challenge", "challenge_at", "seen", "listed")

    def __init__(self, addr):
        self.addr = addr
        self.info = {}
        self.challenge = ""
        self.challenge_at = 0.0
        self.seen = 0.0  # last valid infoResponse
        self.listed = False


class Master:
    def __init__(self, sock4, sock6, timeout, max_servers, max_per_ip, log):
        self.sock4, self.sock6 = sock4, sock6
        self.timeout = timeout
        self.max_servers = max_servers
        self.max_per_ip = max_per_ip
        self.log = log
        self.verbose = False
        self.servers = {}  # (ip, port) -> Server
        self.lock = threading.Lock()

    # --- sending ----------------------------------------------------------
    def send(self, addr, payload):
        sock = self.sock6 if ":" in addr[0] and self.sock6 else self.sock4
        try:
            sock.sendto(OOB + payload, addr)
        except OSError as e:
            self.log(f"send to {addr[0]}:{addr[1]} failed: {e}")

    # --- servers ----------------------------------------------------------
    def heartbeat(self, addr):
        with self.lock:
            s = self.servers.get(addr)
            if s is None:
                same_ip = sum(1 for k in self.servers if k[0] == addr[0])
                if len(self.servers) >= self.max_servers or same_ip >= self.max_per_ip:
                    self.log(f"heartbeat from {addr[0]}:{addr[1]} refused: full")
                    return
                s = self.servers[addr] = Server(addr)
            # one outstanding challenge at a time, renewed after 2 s
            now = time.time()
            if not s.challenge or now - s.challenge_at > 2:
                s.challenge = secrets.token_hex(6)
                s.challenge_at = now
            challenge = s.challenge
        self.send(addr, f"getinfo {challenge}".encode())

    def info_response(self, addr, text):
        info = parse_info(text)
        with self.lock:
            s = self.servers.get(addr)
            if s is None or not s.challenge or info.get("challenge") != s.challenge:
                return  # not asked for, or stale
            info.pop("challenge", None)
            s.info = info
            s.seen = time.time()
            if not s.listed:
                s.listed = True
                self.log(
                    f"listed {addr[0]}:{addr[1]} game={info.get('gamename','?')} "
                    f"protocol={info.get('protocol','?')} \"{info.get('hostname','')}\""
                )

    def expire(self):
        now = time.time()
        with self.lock:
            for k in [k for k, s in self.servers.items()
                      if (s.listed and now - s.seen > self.timeout)
                      or (not s.listed and now - s.challenge_at > 10)]:
                if self.servers[k].listed:
                    self.log(f"dropped {k[0]}:{k[1]} (silent)")
                del self.servers[k]

    # --- clients ----------------------------------------------------------
    def matching(self, game, protocol, empty, full, want4, want6):
        out = []
        with self.lock:
            for s in self.servers.values():
                if not s.listed:
                    continue
                i = s.info
                if game and i.get("gamename", "").lower() != game.lower():
                    continue
                if protocol is not None and i.get("protocol") != str(protocol):
                    continue
                try:
                    clients = int(i.get("clients", "0"))
                    maxc = int(i.get("sv_maxclients", i.get("maxclients", "0")))
                except ValueError:
                    clients, maxc = 0, 0
                if not empty and clients == 0:
                    continue
                if not full and maxc and clients >= maxc:
                    continue
                ip = ipaddress.ip_address(s.addr[0])
                if ip.version == 6 and ip.ipv4_mapped:
                    ip = ip.ipv4_mapped
                if ip.version == 4 and not want4:
                    continue
                if ip.version == 6 and not want6:
                    continue
                out.append((ip, s.addr[1]))
        return out

    def getservers(self, addr, args, ext):
        # getservers[Ext] <game> <protocol> [empty] [full] [ipv4] [ipv6] [gametype=x]
        game = args[0] if args else ""
        protocol = None
        rest = args[1:]
        if rest and rest[0].isdigit():
            protocol = int(rest[0])
            rest = rest[1:]
        elif args and args[0].isdigit():  # Quake 3 style: no game name
            game, protocol, rest = "", int(args[0]), args[1:]
        flags = {r.lower() for r in rest}
        want4 = "ipv4" in flags or "ipv6" not in flags or not ext
        want6 = ext and ("ipv6" in flags or "ipv4" not in flags)
        rows = self.matching(game, protocol, "empty" in flags, "full" in flags, want4, want6)
        head = b"getserversExtResponse" if ext else b"getserversResponse"
        packet = bytearray(head)
        sent = 0
        for ip, port in rows:
            if ip.version == 4:
                entry = b"\\" + ip.packed + port.to_bytes(2, "big")
            else:
                entry = b"/" + ip.packed + port.to_bytes(2, "big")
            if len(packet) + len(entry) + 7 > MAX_PACKET:
                self.send(addr, bytes(packet))
                packet = bytearray(head)
            packet += entry
            sent += 1
        packet += b"\\EOT\0\0\0"
        self.send(addr, bytes(packet))
        self.log(f"{addr[0]}:{addr[1]} asked for game={game or '*'} protocol={protocol}: {sent} server(s)")

    # --- dispatch ---------------------------------------------------------
    def packet(self, data, addr):
        if not data.startswith(OOB):
            return
        body = data[4:]
        # the command is the first word; infoResponse is followed by \n
        text = body.decode("latin-1", "replace")
        if text.startswith("heartbeat"):
            self.heartbeat(addr)
        elif text.startswith("infoResponse"):
            nl = text.find("\n")
            self.info_response(addr, text[nl + 1:] if nl >= 0 else "")
        elif text.startswith("getserversExt"):
            self.getservers(addr, text.split()[1:], True)
        elif text.startswith("getservers"):
            self.getservers(addr, text.split()[1:], False)
        elif self.verbose:
            self.log(f"unknown packet from {addr[0]}:{addr[1]}: {body[:60]!r}")

    def snapshot(self):
        with self.lock:
            return [
                {"addr": f"{s.addr[0]}:{s.addr[1]}", "age": round(time.time() - s.seen, 1), "info": s.info}
                for s in self.servers.values() if s.listed
            ]


def serve_udp(master, sock):
    while True:
        try:
            data, addr = sock.recvfrom(4096)
        except OSError:
            continue
        master.packet(data, addr[:2])


def main():
    ap = argparse.ArgumentParser(description="DarkPlaces-protocol master server for Bulwark")
    ap.add_argument("--bind", default="0.0.0.0", help="IPv4 address to listen on")
    ap.add_argument("--bind6", default="", help="IPv6 address to listen on too (for example ::)")
    ap.add_argument("--port", type=int, default=27950)
    ap.add_argument("--http", default="", help="host:port for a JSON list of servers (keep it local)")
    ap.add_argument("--timeout", type=float, default=900, help="seconds before a silent server is dropped")
    ap.add_argument("--max-servers", type=int, default=4096)
    ap.add_argument("--max-per-ip", type=int, default=32)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--verbose", action="store_true", help="log packets it doesn't understand")
    a = ap.parse_args()

    def log(msg):
        if not a.quiet:
            print(time.strftime("%H:%M:%S"), msg, flush=True)

    s4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s4.bind((a.bind, a.port))
    s6 = None
    if a.bind6:
        s6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        s6.bind((a.bind6, a.port))
    m = Master(s4, s6, a.timeout, a.max_servers, a.max_per_ip, log)
    m.verbose = a.verbose
    for s in (s4, s6):
        if s:
            threading.Thread(target=serve_udp, args=(m, s), daemon=True).start()

    if a.http:
        host, port = a.http.rsplit(":", 1)

        class H(BaseHTTPRequestHandler):
            def do_GET(self):
                body = json.dumps(m.snapshot(), indent=1).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        threading.Thread(target=ThreadingHTTPServer((host, int(port)), H).serve_forever, daemon=True).start()

    log(f"bwmaster on udp {a.bind}:{a.port}" + (f" and [{a.bind6}]:{a.port}" if s6 else ""))
    try:
        while True:
            time.sleep(5)
            m.expire()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
