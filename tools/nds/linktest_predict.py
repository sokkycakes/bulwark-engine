#!/usr/bin/env python3
"""Link v2 test client: joins a bulwark server as a DS would, sends numbered
commands (docs/spec/link.md, INPUT extension) and decodes PSTATE, PMOVEVARS
and BRUSHINFO.

  linktest_predict.py [host] [port] [-v1] [-rcon PORT] [-scenario NAME]

Scenarios: walk (default), resend (every packet repeats the last 8 commands
and every 4th packet is dropped), gaps (only the newest command in each
packet, every 4th dropped: the server counts the lost ones), fast (100 ms
commands at 30 Hz: the movement budget clips them), mapchange (rcon "map" while commands keep
coming from the old map), teleport (rcon link_teleport: fixseq goes up).
Prints one line per PSTATE that changed something, and a summary. With
sv_link_log 1 on the server, each command it ran shows as one LINKPM cmd
line in its log; compare with the sequences printed here.
"""
import socket, struct, sys, time

MAGIC = b'BWL1'
DISCOVER, JOIN, INPUT, LEAVE = 1, 2, 3, 4
WELCOME, REFUSE, VIEW, PSTATE, PMOVEVARS, BRUSHINFO = 0x82, 0x83, 0x85, 0x8B, 0x8C, 0x8D
PMV = ('g_gravity pm_accelerate pm_airaccelerate pm_airstepsize pm_boxcenter pm_boxwidth pm_crouchheight '
       'pm_crouchspeed pm_crouchviewheight pm_edgefriction pm_friction pm_gravity pm_jumpheight pm_airjumps '
       'pm_backspeed pm_groundspeedcap pm_jumpspeedcap pm_maxviewpitch pm_minviewpitch pm_noclipaccelerate '
       'pm_noclipspeed pm_normalheight pm_normalviewheight pm_nospeedcap pm_proneheight pm_pronespeed '
       'pm_proneviewheight pm_runspeed pm_runfiring pm_stamina pm_staminarate pm_staminathreshold pm_stepsize '
       'pm_stopspeed pm_walkspeed pm_wateraccelerate pm_waterjumpheight').split()

args = [a for a in sys.argv[1:]]
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
V1 = opt('-v1', flag=True)
RCON = int(opt('-rcon', '0'))
SCEN = opt('-scenario', 'walk')
TP = opt('-tp', '0 0 64 90')  # where the teleport scenario puts the player: x y z yaw
HOST = args[0] if args else '127.0.0.1'
PORT = int(args[1]) if len(args) > 1 else 27901

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setblocking(False)
session = int(time.time() * 1000) & 0xffffffff


def rcon(cmd):
    if not RCON:
        return
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(b'\xff\xff\xff\xffrcon rig ' + cmd.encode() + b'\n', (HOST, RCON))
    s.close()


def send(t, payload):
    sock.sendto(MAGIC + bytes([t]) + payload, (HOST, PORT))


def f32s(d, o, n):
    return struct.unpack_from('<%df' % n, d, o)


class State:
    spawncount = None
    linkver = None
    pstates = 0
    vars = {}
    brush = {}
    last = None
    views = []
    sendtime = {}   # command sequence: when first sent
    rtt = []        # command sent .. PSTATE that acked it


st = State()


def parse(d):
    t = d[4]
    p = d[5:]
    if t == WELCOME:
        sc = struct.unpack_from('<H', p, 5)[0]
        st.linkver = p[71] if len(p) > 71 else 0
        if sc != st.spawncount:
            print('WELCOME spawncount %d map %s linkver %d' % (sc, p[7:71].split(b'\0')[0].decode(), st.linkver))
        st.spawncount = sc
    elif t == VIEW:
        st.views.append((time.time(), tuple(v / 8 for v in struct.unpack_from('<iii', p, 6))))
    elif t == REFUSE:
        print('REFUSE', p.split(b'\0')[0])
        sys.exit(1)
    elif t == PMOVEVARS:
        sc, ver, cnt = struct.unpack_from('<HHB', p, 4)
        g = f32s(p, 10, cnt)
        pl = f32s(p, 10 + 4 * cnt, cnt)
        if st.vars.get('ver') != ver:
            print('PMOVEVARS spawn %d ver %d len %d: g_gravity %g walk %g/%g run %g/%g crouch %g/%g jumpheight %g'
                  % (sc, ver, len(p), g[0], g[PMV.index('pm_walkspeed')], pl[PMV.index('pm_walkspeed')],
                     g[PMV.index('pm_runspeed')], pl[PMV.index('pm_runspeed')], g[PMV.index('pm_crouchspeed')],
                     pl[PMV.index('pm_crouchspeed')], g[PMV.index('pm_jumpheight')]))
        if sc == st.spawncount:
            st.vars = {'ver': ver, 'g': g, 'p': pl}
    elif t == BRUSHINFO:
        sc, ver, total, first, cnt = struct.unpack_from('<HHHHB', p, 4)
        b = st.brush.setdefault((sc, ver), {})
        for i in range(cnt):
            e, m, so, sk = struct.unpack_from('<HHBb', p, 13 + 18 * i)
            b[first + i] = (e, m, so, sk, f32s(p, 13 + 18 * i + 6, 3))
        if len(b) == total and not st.brush.get(('done', sc, ver)):
            st.brush[('done', sc, ver)] = True
            st.brushack = ver
            print('BRUSHINFO spawn %d ver %d: %d entries (%s)' % (sc, ver, total, ' '.join(
                '%d:*%d/%d/%d' % v[:4] for _, v in sorted(b.items())[:12])))
    elif t == PSTATE:
        (sess, ack, pseq, status, clipped, gaps, sc, varsver, brushver, fixseq, nmov,
         fixp, fixy) = struct.unpack_from('<IIHBBHHHHBBhh', p, 0)
        org = f32s(p, 26, 3)
        vel = f32s(p, 38, 3)
        view = f32s(p, 74, 3)
        mins = f32s(p, 86, 3)
        maxs = f32s(p, 98, 3)
        flags, vflags, gflags = struct.unpack_from('<III', p, 110)
        movetype, solid, wl, wt, ground = struct.unpack_from('<BBbbH', p, 122)
        sc_f = f32s(p, 128, 12)
        health, nbox = struct.unpack_from('<hB', p, 176)
        st.pstates += 1
        rec = dict(ack=ack, pseq=pseq, status=status, clipped=clipped, gaps=gaps, sc=sc, varsver=varsver,
                   brushver=brushver, fixseq=fixseq, fixp=fixp, fixy=fixy, org=org, vel=vel, flags=flags,
                   vflags=vflags, movetype=movetype, ground=ground, nmov=nmov, nbox=nbox, len=len(p),
                   view=view, mins=mins, maxs=maxs, health=health, f=sc_f)
        prev = st.last
        if not prev or prev['ack'] != ack and (nmov or prev['nmov']) or prev['fixseq'] != fixseq or prev['status'] != status or st.pstates % 20 == 0:
            print('PSTATE pseq %5d ack %5d st %02x clip %d gaps %d sc %d vars %d brush %d fix %d (%d %d) '
                  'org %.2f %.2f %.2f vel %.1f %.1f %.1f fl %x vfl %x mt %d gnd %d hp %d view %.0f '
                  'box %g..%g mov %d boxes %d len %d grav %g fric %g maxsp %g stam %g spd %g'
                  % (pseq, ack, status, clipped, gaps, sc, varsver, brushver, fixseq, fixp, fixy, *org, *vel, flags,
                     vflags, movetype, ground, health, view[2], mins[2], maxs[2], nmov, nbox, len(p), sc_f[0],
                     sc_f[1], sc_f[2], sc_f[5], sc_f[7]))
        for i in range(nmov):
            e = struct.unpack_from('<H', p, 180 + 26 * i)[0]
            print('  mover %d org %.2f %.2f %.2f vel %.1f %.1f %.1f' % (e, *f32s(p, 182 + 26 * i, 6)))
        st.last = rec
        for q in [q for q in st.sendtime if q <= ack]:
            st.rtt.append(time.time() - st.sendtime.pop(q))


def drain():
    while True:
        try:
            d, _ = sock.recvfrom(2048)
        except BlockingIOError:
            return
        if d[:4] == MAGIC:
            parse(d)


# --- join ---
send(JOIN, struct.pack('<IH16sB', session, 1, b'linktest', 2))
t0 = time.time()
while st.spawncount is None and time.time() - t0 < 5:
    drain()
    time.sleep(0.01)
if st.spawncount is None:
    print('no WELCOME')
    sys.exit(1)

pkt = 0
seq = 0           # newest command made
acked = 0
sent_cmds = []    # (seq, cmd bytes, spawncount it was made on)
floor = 0         # commands at or below this are never sent again (new map)
devspawn = st.spawncount
ms_last = 0
t_start = time.time()
sent_seqs = []


def make_cmd(msec, pitch, yaw, fwd, side, up, buttons, impulse=0):
    return struct.pack('<BhhhhhHB', msec, pitch, yaw, fwd, side, up, buttons, impulse)


def send_input(cmds_to_send, fwd=0, buttons=0):
    global pkt
    pkt = (pkt + 1) & 0xffff
    v1 = struct.pack('<IHHBBBbbbhhHBBHHHHBB', session, pkt, 0, 0, 0, 0,
                     max(-127, min(127, int(fwd * 127 / 400))), 0, 0, 0, 0, 0, 30, 30, 0, 0, devspawn,
                     buttons, 0, 0)
    assert len(v1) == 32
    if V1 or not cmds_to_send:
        send(INPUT, v1)
        return
    newest = cmds_to_send[-1][0]
    ext = struct.pack('<BBIBBHH', ord('C'), 2, newest, len(cmds_to_send), 14,
                      st.vars.get('ver', 0), getattr(st, 'brushack', 0))
    send(INPUT, v1 + ext + b''.join(c for _, c, _ in cmds_to_send))
    sent_seqs.extend(s for s, _, _ in cmds_to_send)
    for s, _, _ in cmds_to_send:
        st.sendtime.setdefault(s, time.time())


def plan(t):
    """what the stick and buttons do t seconds in: (fwd, side, buttons, yaw)"""
    if t < 1.0:
        return 0, 0, 0, 0
    if t < 3.0:
        return 400, 0, 0, 0
    if t < 4.0:
        return 0, 400, 0, 0
    if t < 4.2:
        return 400, 0, 2, 16384      # jump, turned 90 degrees
    if t < 5.5:
        return 400, 0, 0x80, 16384   # crouch-walk
    return 0, 0, 0, 0


duration = {'walk': 7, 'resend': 7, 'gaps': 7, 'fast': 4, 'mapchange': 12, 'teleport': 6}[SCEN]
mapchanged = teleported = False
drop_every = 4 if SCEN in ('resend', 'gaps') else 0
period = 2 / 59.8261
next_t = time.time()
while time.time() - t_start < duration:
    drain()
    now = time.time()
    t = now - t_start
    if SCEN == 'mapchange' and t > 4 and not mapchanged:
        mapchanged = True
        print('rcon: map coop_arena (the device keeps sending commands for the old map for 1.5 s)')
        rcon('map coop_arena')
        mapchange_at = now
    if SCEN == 'mapchange' and mapchanged and st.spawncount != devspawn and now - mapchange_at > 1.5:
        print('device: loaded the new map %d; commands up to %d are never sent again' % (st.spawncount, seq))
        devspawn = st.spawncount
        floor = seq
        st.vars = {}
        st.brushack = 0
    if SCEN == 'teleport' and t > 3 and not teleported:
        teleported = True
        print('rcon: link_teleport ' + TP)
        rcon('link_teleport ' + TP)
    if now < next_t:
        time.sleep(max(0, min(0.002, next_t - now)))
        continue
    next_t += period
    vbl = int(t * 59.8261)
    now_ms = vbl * 1000000 // 59826
    msec = max(1, min(100, now_ms - ms_last))
    ms_last += msec
    fwd, side, buttons, yaw = plan(t)
    if SCEN == 'fast':
        msec = 100  # claims 3x real time: the server's movement budget cuts it (PSTATE clipped)
    seq += 1
    sent_cmds.append((seq, make_cmd(msec, 0, yaw, fwd, side, 0, buttons), devspawn))
    if st.last and st.last['sc'] == devspawn:
        acked = max(acked, st.last['ack'])
    unacked = [c for c in sent_cmds if c[0] > max(acked, floor)][-8:]
    if SCEN == 'resend':
        unacked = [c for c in sent_cmds if c[0] > floor][-8:]  # always the last 8, acked or not
    if SCEN == 'gaps':
        unacked = unacked[-1:]  # only the newest: a lost packet loses its command
    if drop_every and seq % drop_every == 0:
        continue  # "lost" packet
    send_input(unacked, fwd, buttons)

for when in (1.0, 3.0, 4.0, 5.5, duration - 0.1):
    v = [o for t, o in st.views if t - t_start <= when]
    if v:
        print('VIEW eye at %.1f s: %.2f %.2f %.2f' % (when, *v[-1]))
if st.rtt:
    r = sorted(st.rtt)
    print('ack delay: n %d min %.1f median %.1f max %.1f ms' % (len(r), r[0] * 1000, r[len(r) // 2] * 1000, r[-1] * 1000))
send(LEAVE, struct.pack('<I', session))
drain()
print('SUMMARY commands made %d, last ack %s, pstates %d, packets sent with %d command copies (%d distinct)'
      % (seq, st.last and st.last['ack'], st.pstates, len(sent_seqs), len(set(sent_seqs))))
