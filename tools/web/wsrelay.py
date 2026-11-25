#!/usr/bin/env python3
"""Let browser players join a Bulwark server.

A web page can't send UDP: the web engine sends each game packet as a
WebSocket message to ws://<host>:<port>/ instead (wss:// when the page is
https). This relay takes those connections and passes the packets to and
from the game server's UDP port, so browser and desktop players share one
server.

    # the server on UDP 27500, the relay on TCP 27500 of the same machine
    bulwark-ded +map ai &
    python3 tools/web/wsrelay.py --listen 27500 --server 127.0.0.1:27500

Browser players then type `connect <host>` (or `connect <host>:27500`) as
desktop players do. For an https page, put the relay behind a TLS proxy
(Caddy, nginx) on the same port, since browsers refuse ws:// from https.

One UDP socket per browser player, so the server sees each as its own
address. Only the standard library is used.
"""
import argparse
import asyncio
import base64
import hashlib
import os
import socket
import struct
import sys
import time

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
# Emscripten announces a socket's local port in a first message of its own
PORT_MSG = b"\xff\xff\xff\xffport"
MAX_PACKET = 65507


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Udp(asyncio.DatagramProtocol):
    def __init__(self, client):
        self.client = client

    def datagram_received(self, data, addr):
        self.client.to_browser(data)

    def error_received(self, exc):
        pass


class Client:
    def __init__(self, relay, reader, writer, peer):
        self.relay = relay
        self.reader = reader
        self.writer = writer
        self.peer = peer
        self.udp = None
        self.closed = False
        self.sent = self.received = 0

    def to_browser(self, data):
        if self.closed:
            return
        n = len(data)
        if n < 126:
            head = struct.pack("!BB", 0x82, n)
        elif n < 65536:
            head = struct.pack("!BBH", 0x82, 126, n)
        else:
            head = struct.pack("!BBQ", 0x82, 127, n)
        self.writer.write(head + data)
        self.received += 1

    async def read_frame(self):
        b = await self.reader.readexactly(2)
        op = b[0] & 0x0F
        masked = b[1] & 0x80
        n = b[1] & 0x7F
        if n == 126:
            n = struct.unpack("!H", await self.reader.readexactly(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", await self.reader.readexactly(8))[0]
        if n > MAX_PACKET + 16:
            raise ValueError("frame too big")
        mask = await self.reader.readexactly(4) if masked else None
        data = await self.reader.readexactly(n)
        if mask:
            data = bytes(c ^ mask[i & 3] for i, c in enumerate(data))
        return b[0] & 0x80, op, data

    async def run(self):
        loop = asyncio.get_running_loop()
        self.udp, _ = await loop.create_datagram_endpoint(lambda: Udp(self), remote_addr=self.relay.server)
        log("%s joined (%d browser players)" % (self.peer, len(self.relay.clients)))
        partial = b""
        try:
            while True:
                fin, op, data = await self.read_frame()
                if op == 0x8:  # close
                    self.writer.write(b"\x88\x00")
                    break
                if op == 0x9:  # ping
                    self.writer.write(struct.pack("!BB", 0x8A, len(data[:125])) + data[:125])
                    continue
                if op == 0xA:
                    continue
                if op in (0x1, 0x2):
                    partial = data
                elif op == 0x0:
                    partial += data
                else:
                    break
                if not fin:
                    continue
                packet, partial = partial, b""
                if packet.startswith(PORT_MSG) and len(packet) == 10:
                    continue
                if packet:
                    self.udp.sendto(packet)
                    self.sent += 1
        except (asyncio.IncompleteReadError, ConnectionError, ValueError):
            pass
        finally:
            self.close()

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.udp:
            self.udp.close()
        try:
            self.writer.close()
        except Exception:
            pass
        self.relay.clients.discard(self)
        log("%s left (%d packets in, %d out)" % (self.peer, self.sent, self.received))


class Relay:
    def __init__(self, server, origins, maxclients):
        self.server = server
        self.origins = origins
        self.maxclients = maxclients
        self.clients = set()

    async def handle(self, reader, writer):
        peer = "%s:%d" % writer.get_extra_info("peername")[:2]
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        except Exception:
            writer.close()
            return
        lines = head.decode("latin-1").split("\r\n")
        headers = {}
        for l in lines[1:]:
            if ":" in l:
                k, v = l.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        key = headers.get("sec-websocket-key")
        if not key or "websocket" not in headers.get("upgrade", "").lower():
            body = b"Bulwark WebSocket relay: join from a Bulwark web game.\n"
            writer.write(b"HTTP/1.1 426 Upgrade Required\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                         b"Connection: close\r\n\r\n%s" % (len(body), body))
            writer.close()
            return
        origin = headers.get("origin", "")
        if self.origins and origin not in self.origins:
            log("%s refused: origin %r isn't allowed" % (peer, origin))
            writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            writer.close()
            return
        if len(self.clients) >= self.maxclients:
            writer.write(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            writer.close()
            return
        accept = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
        reply = ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n" % accept)
        # the engine asks for "binary"; a browser drops the connection unless it's echoed
        protos = [p.strip() for p in headers.get("sec-websocket-protocol", "").split(",") if p.strip()]
        if protos:
            reply += "Sec-WebSocket-Protocol: %s\r\n" % ("binary" if "binary" in protos else protos[0])
        writer.write((reply + "\r\n").encode())
        sock = writer.get_extra_info("socket")
        if sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c = Client(self, reader, writer, peer)
        self.clients.add(c)
        await c.run()


def parse_addr(s, default_port):
    host, _, port = s.rpartition(":") if ":" in s else (s, "", str(default_port))
    return host or "127.0.0.1", int(port)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="27500", help="[address:]port for WebSockets (default 27500)")
    ap.add_argument("--server", default="127.0.0.1:27500", help="the game server's UDP address (default 127.0.0.1:27500)")
    ap.add_argument("--origin", action="append", default=[],
                    help="only pages from this origin may join (https://example.com); repeat for more")
    ap.add_argument("--max", type=int, default=64, help="most browser players at once (default 64)")
    a = ap.parse_args()
    lhost, lport = parse_addr(a.listen, 27500) if ":" in a.listen else ("0.0.0.0", int(a.listen))
    shost, sport = parse_addr(a.server, 27500)
    server = (socket.gethostbyname(shost), sport)
    relay = Relay(server, set(a.origin), a.max)
    srv = await asyncio.start_server(relay.handle, lhost, lport)
    log("relaying ws://%s:%d/ to udp %s:%d" % (lhost, lport, server[0], server[1]))
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
