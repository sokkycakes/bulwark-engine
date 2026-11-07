#!/usr/bin/env python3
"""EVENT test client: joins a bulwark server as a DS would, takes EVENT records
(docs/spec/link.md, "Events") and acknowledges them with INPUT's 'E', and
triggers prints with rcon "say".

  linktest_events.py [host] [link port] [-rcon PORT] [-scenario NAME] [-v2]

-v2 sends INPUT as the DS does with prediction on: 'C' with one command, 'W' and
'H' before 'E' (the layout f902f74 fixed); without it 'E' is at offset 32.

Scenarios:
  order     20 chat lines: each arrives once, in order
  loss      the first 3 EVENT packets are ignored (as if lost): the server sends
            them again, and all 20 lines still arrive once, in order
  overflow  nothing is acknowledged while 80 lines are said: the server keeps the
            last 64, and the next EVENT starts past what we have (a gap of 16)
Prints a summary line and exits 1 on a failure.
"""
import socket, struct, sys, time

MAGIC = b'BWL1'
JOIN, INPUT = 2, 3
WELCOME, REFUSE, EVENT = 0x82, 0x83, 0x90

args = sys.argv[1:]
def opt(name, default=None):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
rport = int(opt('-rcon', '27500')); scenario = opt('-scenario', 'order')
v2 = '-v2' in args
if v2:
    args.remove('-v2')
host = args[0] if args else '127.0.0.1'; port = int(args[1]) if len(args) > 1 else 27901

def rcon(cmd):
    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    r.sendto(b'\xff\xff\xff\xffrcon rig ' + cmd.encode() + b'\n', (host, rport)); r.close()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.05)
session = 0x5eed0001 + int(time.time()) % 1000
s.sendto(MAGIC + bytes([JOIN]) + struct.pack('<IH16sB', session, 1, b'evtest', 2), (host, port))
spawn = None; t0 = time.time()
while spawn is None and time.time() - t0 < 5:
    try:
        d, _ = s.recvfrom(2000)
    except socket.timeout:
        continue
    if d[:4] == MAGIC and d[4] == WELCOME:
        spawn = struct.unpack('<H', d[10:12])[0]
    elif d[:4] == MAGIC and d[4] == REFUSE:
        sys.exit('refused: ' + d[5:].split(b'\0')[0].decode())
if spawn is None:
    sys.exit('no WELCOME')

have = 0; seq = 0; packets = 0; ignored = 0; gaps = 0; lines = []; acking = True; newest = 0
partials = 0; partial_dropped = 0; maxcount = 0
def send_input():
    global seq, newest
    seq = (seq + 1) & 0xffff
    p = struct.pack('<IHHBBBbbbhhHBBHHHHBB', session, seq, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 30, 30, 0, 0, spawn, 0, 0, 0)
    if v2: # one command (link.md 'C'), WEAPONINFO and HUDINFO versions, then 'E'
        newest += 1
        p += b'C' + bytes([2]) + struct.pack('<IBBHH', newest, 1, 14, 0, 0) + bytes([0] * 12 + [33, 0])
        p += b'W' + struct.pack('<HH', 0, 0) + b'H' + struct.pack('<H', 0)
    p += b'E' + struct.pack('<H', have if acking else 0)
    s.sendto(MAGIC + bytes([INPUT]) + p, (host, port))

def pump(secs):
    global have, packets, ignored, gaps, partials, partial_dropped, maxcount
    end = time.time() + secs; nxt = 0
    while time.time() < end:
        if time.time() >= nxt:
            send_input(); nxt = time.time() + 0.05
        try:
            d, _ = s.recvfrom(2000)
        except socket.timeout:
            continue
        if d[:4] != MAGIC or d[4] != EVENT:
            continue
        packets += 1
        if scenario == 'loss' and ignored < 3:
            ignored += 1; continue
        if not acking:
            continue
        first, count = struct.unpack('<HB', d[9:12]); i = 12
        partial = count & 0x80; count &= 0x7f; maxcount = max(maxcount, count)
        if partial:
            partials += 1
            if first != (have + 1) & 0xffff and ((first - (have + 1)) & 0xffff) < 0x8000:
                partial_dropped += 1; continue # the resend brings the ones before
        if ((first - (have + 1)) & 0xffff) < 0x8000 and first != (have + 1) & 0xffff:
            gaps += (first - have - 1) & 0xffff; have = (first - 1) & 0xffff
        for k in range(count):
            t, n = d[i], d[i + 1]; body = d[i + 2:i + 2 + n]; i += 2 + n
            if (first + k) & 0xffff == (have + 1) & 0xffff:
                have = (first + k) & 0xffff
                if t == 1 and b'evtest line' in body:
                    lines.append(int(body.split(b'evtest line ')[1].split()[0]))

pump(1.0)
n = 80 if scenario == 'overflow' else 20
if scenario == 'overflow':
    acking = False
for i in range(n):
    rcon('say evtest line %d' % i); time.sleep(0.02)
pump(1.0)
acking = True
pump(2.0)
s.sendto(MAGIC + bytes([4]) + struct.pack('<I', session), (host, port))
ok = lines == list(range(n)) if scenario != 'overflow' else (lines == list(range(n - len(lines), n)) and gaps > 0 and len(lines) <= 64)
print('%s: %s, %d lines (%s..%s), %d EVENT packets (%d new-only, %d of those waited), %d ignored, gap %d, last record %d' % (
    scenario, 'ok' if ok else 'FAIL', len(lines), lines[0] if lines else '-', lines[-1] if lines else '-', packets, partials,
    partial_dropped, ignored, gaps, have))
sys.exit(0 if ok else 1)
