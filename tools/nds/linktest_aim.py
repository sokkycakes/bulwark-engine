#!/usr/bin/env python3
"""Link v2 autoaim test client: joins a bulwark server as a DS would and fires
at monsters a few degrees beside them (docs/spec/link.md, "Autoaim"),
counting where its own shots land (FX) and the server's choice (VIEW 'A').

  linktest_aim.py [host] [port] [-rcon PORT] [-pairs N] [-cone DEG] [-off DEG]
                  [-noaim] [-dur S] [-move] [-class NAME] [-name NAME]

For each of N monsters (default 3), nearest first, 300 to 800 units away,
whose classname has -class in it (default zombie; ent_info by rcon, password
rig): one volley with sv_link_autoaim 0, then one with sv_link_autoaim -cone
(default 5), each holding attack for -dur seconds (default 0.4) aimed -off
degrees of yaw beside the monster's middle (default: its half-width as seen
from here plus 1.5, so the straight shot passes beside it), on the side with
no other monster within 10 degrees. A shot hits when its impact is inside the
monster's box grown by 8 units. -noaim: the device's autoaim=0 (INPUT 'A'
flags 0). With developer 1 the server logs "link: NAME fires with autoaim on
edict N". Without -rcon the cvar is left alone and any monster counts.
Each volley aims for 0.33 s before it fires: a command's shot leaves along the
angles of the command before it (Nuclide's XR space, see sv_link.c).
-move: strafe (side 400) while firing, left and right by turns, for the
prediction logs (sv_link_log 1, tools/nds/pmtest and weptest).
A shotgun hero (the co-op game picks one by name) joins again as NAME0, NAME1...
"""
import math, socket, struct, sys, time

MAGIC = b'BWL1'
JOIN, INPUT, LEAVE = 2, 3, 4
WELCOME, REFUSE, VIEW, ENTS, STATS, FX, PSTATE = 0x82, 0x83, 0x85, 0x87, 0x89, 0x8A, 0x8B

args = sys.argv[1:]


def opt(name, default=None, flag=False):
    if name in args:
        i = args.index(name)
        if flag:
            del args[i]
            return True
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return default


NOAIM = opt('-noaim', flag=True)
OFF = opt('-off')
PAIRS = int(opt('-pairs', '3'))
CONE = opt('-cone', '5')
DUR = float(opt('-dur', '0.4'))
SIDE = 400 if opt('-move', flag=True) else 0
NAME = opt('-name', 'aimtest').encode()
RCON = int(opt('-rcon', '0'))
CLASS = opt('-class', 'zombie')
HOST = args[0] if args else '127.0.0.1'
PORT = int(args[1]) if len(args) > 1 else 27901

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setblocking(False)
session = int(time.time() * 1000) & 0xffffffff


def send(t, payload):
    sock.sendto(MAGIC + bytes([t]) + payload, (HOST, PORT))


classes = {}


def ent_info(ent):
    """the entity's fields by rcon ent_info, the first of each name"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.3)
    s.sendto(b'\xff\xff\xff\xffrcon rig ent_info %d\n' % ent, (HOST, RCON))
    out = b''
    try:
        while True:
            out += s.recvfrom(65536)[0][4:]
    except socket.timeout:
        pass
    s.close()
    f = {}
    for l in out.decode(errors='replace').splitlines():
        w = l.split(None, 1)
        if len(w) == 2 and w[0] not in f:
            f[w[0]] = w[1].strip()
    return f


def classname(ent):
    if ent not in classes:
        classes[ent] = ent_info(ent).get('classname', '')
    return classes[ent]


def health(ent):
    return ent_info(ent).get('health', '?') if RCON else '?'


class S:
    spawncount = None
    ack = 0
    org = None
    view = None
    health = 0
    weapon = ''
    things = {}
    aim = None
    fx = []


st = S()


def parse(d):
    t, p = d[4], d[5:]
    if t == WELCOME:
        st.spawncount = struct.unpack_from('<H', p, 5)[0]
    elif t == REFUSE:
        print('REFUSE', p.split(b'\0')[0])
        sys.exit(1)
    elif t == PSTATE:
        st.ack = struct.unpack_from('<I', p, 4)[0]
        st.org = struct.unpack_from('<3f', p, 26)
        st.view = struct.unpack_from('<3f', p, 74)
        st.health = struct.unpack_from('<h', p, 176)[0]
    elif t == STATS:
        n = p[10]
        st.weapon = p[11:11 + n].decode(errors='replace')
    elif t == VIEW:
        count = p[25]
        o = 26 + count * 9 + 6
        if len(p) >= o + 10 and p[o] == ord('A'):
            st.aim = (p[o + 1] / 10, *struct.unpack_from('<H3h', p, o + 2))
    elif t == ENTS:
        nmodels = struct.unpack_from('<H', p, 6)[0]
        o = 8 + (nmodels + 7) // 8
        moved = p[o]
        o += 1 + moved * 14
        nth, size = p[o], p[o + 1]
        o += 2
        things = {}
        for i in range(nth):
            x, y, z, yaw, half, height, kind, ent = struct.unpack_from('<3hBBBBH', p, o + i * size)
            things[ent] = (x, y, z, half, height, kind)
        st.things = things
    elif t == FX:
        for i in range(p[4]):
            f = p[5 + 25 * i:5 + 25 * (i + 1)]
            if len(f) == 25 and f[0] & 1:  # our own shot
                hit = [v / 8 for v in struct.unpack_from('<3i', f, 1)]
                st.fx.append((time.time(), hit))


def drain():
    while True:
        try:
            d, _ = sock.recvfrom(2048)
        except BlockingIOError:
            return
        if d[:4] == MAGIC:
            parse(d)


send(JOIN, struct.pack('<IH16sB', session, 1, NAME, 2))
t0 = time.time()
while st.spawncount is None and time.time() - t0 < 5:
    drain()
    time.sleep(0.01)
if st.spawncount is None:
    print('no WELCOME')
    sys.exit(1)

seq = 0
cmds = []
pkt = 0
lastweap = 0


def send_cmd(pitch, yaw, buttons, impulse=0, side=0):
    """one 17 ms command, sent with the unacknowledged ones (at most 8)"""
    global seq, pkt
    seq += 1
    cmds.append((seq, struct.pack('<BhhhhhHB', 17, int(pitch), int(yaw), 0, side, 0, buttons, impulse)))
    un = [c for c in cmds if c[0] > st.ack][-8:] or cmds[-1:]  # (an ack from before a rejoin)
    pkt = (pkt + 1) & 0xffff
    v1 = struct.pack('<IHHBBBbbbhhHBBHHHHBB', session, pkt, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 30, 30, 0, 0,
                     st.spawncount, buttons, 0, 0)
    ext = struct.pack('<BBIBBHH', ord('C'), 2, un[-1][0], len(un), 14, 0, 0) + b''.join(c for _, c in un)
    tail = struct.pack('<BHHBHBHBHBBB', ord('W'), 0, 0, ord('H'), 0, ord('E'), 0, ord('S'), 0, 0, ord('L'), lastweap)
    tail += bytes([ord('A'), 0 if NOAIM else 1])
    send(INPUT, v1 + ext + tail)


def eye():
    return [st.org[k] + st.view[k] for k in range(3)]


def angles_to(p):
    e = eye()
    d = [p[k] - e[k] for k in range(3)]
    yaw = math.degrees(math.atan2(d[1], d[0]))
    pitch = math.degrees(math.atan2(d[2], math.hypot(d[0], d[1])))
    return pitch, yaw, math.sqrt(sum(x * x for x in d))


def tick(pitch, yaw, buttons, impulse=0, side=0):
    drain()
    send_cmd(pitch * 65536 / 360, yaw * 65536 / 360, buttons, impulse, side)
    time.sleep(1 / 60)


def rcon(c):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.3)
    s.sendto(b'\xff\xff\xff\xffrcon rig ' + c.encode() + b'\n', (HOST, RCON))
    out = b''
    try:
        while True:
            out += s.recvfrom(65536)[0][4:]
    except socket.timeout:
        pass
    return out.decode(errors='replace')


for attempt in range(8):
    t0 = time.time()
    while time.time() - t0 < 30 and not (st.org and st.health > 0 and st.weapon):
        tick(0, 0, 0)
    if not any(w in st.weapon for w in ('pump', 'shotgun', 'spas')):
        break
    print('gun %s: joining again' % st.weapon)
    send(LEAVE, struct.pack('<I', session))
    time.sleep(1)
    session = (session + 1) & 0xffffffff
    st.spawncount, st.org, st.weapon, st.health, st.ack = None, None, '', 0, 0
    cmds.clear()
    seq = 0
    NAME = NAME[:12] + b'%d' % attempt  # another name may get another hero
    send(JOIN, struct.pack('<IH16sB', session, 1, NAME, 2))
    t0 = time.time()
    while st.spawncount is None and time.time() - t0 < 5:
        drain()
        time.sleep(0.01)


def on_target(h, ent):
    x, y, z, half, height, kind = st.things.get(ent, (1e9, 0, 0, 0, 0, 0))
    return abs(h[0] - x) <= half + 8 and abs(h[1] - y) <= half + 8 and z - 8 <= h[2] <= z + height + 8


def pick(tried):
    for ent, (x, y, z, half, height, kind) in sorted(st.things.items(), key=lambda t: angles_to(
            (t[1][0], t[1][1], t[1][2] + t[1][4] / 2))[2]):
        if kind != 1 or ent in tried or (RCON and CLASS not in classname(ent)):
            continue
        if 300 < angles_to((x, y, z + height / 2))[2] < 800:
            return ent
    return 0


def volley(target, cone):
    """attack for DUR s beside target; (impacts on it, elsewhere, health before, after, targets VIEW named)"""
    x, y, z, half, height, kind = st.things[target]
    pitch, yaw, dist = angles_to((x, y, z + height / 2))
    off = float(OFF) if OFF else math.degrees(math.atan2(half, dist)) + 1.5
    side = 1
    for sgn in (1, -1):  # the side with no other monster within 10 degrees of where we aim
        if all(math.hypot(op - pitch, (oyaw - yaw - sgn * off + 180) % 360 - 180) >= 10
               for e, (ox, oy, oz, oh, ht, k) in st.things.items() if e != target and k == 1
               for op, oyaw, _ in [angles_to((ox, oy, oz + ht / 2))]):
            side = sgn
            break
    if RCON and cone is not None:
        rcon('sv_link_autoaim %s' % cone)
    hp0 = health(target)
    # aim first: a command's shot leaves along the angles of the command before it
    # (Nuclide's XR space, player_pmove.qc), so the first shot would go where we looked
    for _ in range(20):
        x, y, z, half, height, kind = st.things.get(target, (x, y, z, half, height, kind))
        pitch, yaw, dist = angles_to((x, y, z + height / 2))
        tick(pitch, yaw + side * off, 0)
    st.fx = []
    aims = set()
    start = time.time()
    while time.time() - start < DUR + 0.5:  # then let go: the last impacts come in
        if target in st.things:
            x, y, z, half, height, kind = st.things[target]
        pitch, yaw, dist = angles_to((x, y, z + height / 2))
        tick(pitch, yaw + side * off, 1 if time.time() - start < DUR else 0, 0,
             SIDE * (1 if int((time.time() - start) * 4) % 2 else -1) if time.time() - start < DUR else 0)
        if st.aim:
            aims.add(st.aim[1])
    n = sum(on_target(h, target) for _, h in st.fx)
    return n, len(st.fx) - n, hp0, health(target), side * off, dist, sorted(aims)


t0 = time.time()
while time.time() - t0 < 30 and not (st.org and st.health > 0 and st.weapon):
    tick(0, 0, 0)
for _ in range(90):  # the gun comes up
    tick(0, 0, 0)
print('%s: gun %s, autoaim=%d' % (NAME.decode(), st.weapon, 0 if NOAIM else 1))
tried = set()
tot = {}
t0 = time.time()
while len(tried) < PAIRS and time.time() - t0 < 120:
    tick(0, 0, 0)
    target = pick(tried)
    if not target:
        continue
    tried.add(target)
    for cone in ('0', CONE) if RCON else (None,):
        if st.things.get(target, (0,) * 6)[5] != 1:
            print('  edict %d is dead or gone' % target)
            break
        n, m, hp0, hp1, off, dist, aims = volley(target, cone)
        tot.setdefault(cone, [0, 0])
        tot[cone][0] += n
        tot[cone][1] += m
        print('  edict %d (%s, %.0f units), aimed %+.1f degrees beside, sv_link_autoaim %s: %d impacts on it, '
              '%d elsewhere; health %s -> %s; VIEW A targets %s'
              % (target, classname(target) if RCON else '?', dist, off, cone, n, m, hp0, hp1, aims))
send(LEAVE, struct.pack('<I', session))
print('SUMMARY %s autoaim=%d: %s' % (NAME.decode(), 0 if NOAIM else 1, ', '.join(
    'sv_link_autoaim %s: %d impacts on the monsters, %d elsewhere' % (c, v[0], v[1]) for c, v in tot.items())))
