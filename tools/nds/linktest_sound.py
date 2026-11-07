#!/usr/bin/env python3
"""SOUND test client: joins a bulwark server as a DS would, says it plays sound
(INPUT's 'S' trailer, docs/spec/link.md "Sound"), takes SOUND (0x91), AMBIENT
(0x92) and EVENT records 12-15, acknowledges EVENT and AMBIENT, and logs every
sound. With the server's log (sv_link_log 1) it checks that every record the
server made (LINKSND lines) arrived, and that every sound the server sent the
PC's way (SVSND lines: each sound()/pointsound() the engine wrote as svc_sound)
became a record for the device.

  linktest_sound.py [host] [link port] [-rcon PORT] [-secs N] [-log FILE] [-fire]
                    [-loss P] [-out FILE] [-cmd "rcon command"]...

-fire   hold attack 1.5 s, release 0.5 s (the player's weapon sounds)
-loss P ignore that fraction of SOUND packets (as if lost): the repeat must bring
        nearly all records anyway (a record is lost only when both copies are)
-out    one line per sound taken (time, kind, seq, key, ent, chan, vol, atten,
        pitch, flags, org)
-cmd    rcon commands run after the join (link_god 1 is always run)
Prints a summary and exits 1 when a record the server made never arrived
(beyond what -loss explains) or a server sound has no record.
"""
import random, re, socket, struct, sys, time

MAGIC = b'BWL1'
JOIN, INPUT, LEAVE = 2, 3, 4
WELCOME, REFUSE, WEAPONINFO, EVENT, SOUND, AMBIENT = 0x82, 0x83, 0x8e, 0x90, 0x91, 0x92
LE_SOUND, LE_SENTENCE, LE_MUSIC, LE_PRECACHE = 12, 13, 14, 15

args = sys.argv[1:]
def opt(name, default=None):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
def flag(name):
    if name in args:
        args.remove(name); return True
    return False
cmds = []
while '-cmd' in args:
    cmds.append(opt('-cmd'))
rport = int(opt('-rcon', '27500')); secs = float(opt('-secs', '30')); logf = opt('-log'); outf = opt('-out')
loss = float(opt('-loss', '0')); fire = flag('-fire'); v2 = flag('-v2')
host = args[0] if args else '127.0.0.1'; port = int(args[1]) if len(args) > 1 else 27901

def norm(n):
    n = n.replace('\\', '/').lower()
    if n.startswith('./'): n = n[2:]
    if n.startswith('sound/'): n = n[6:]
    for e in ('.wav', '.ogg', '.mp3'):
        if len(n) > 4 and n.endswith(e):
            n = n[:-4]; break
    return n
def key(n):
    h = 2166136261
    for c in norm(n).encode('latin1'):
        h = ((h ^ c) * 16777619) & 0xffffffff
    return h
assert key('misc/talk.wav') == 0x7c72dd3c and key('Sound\\Weapons\\Glock1.WAV') == 0xf61c39de  # spec §2.1

def rcon(cmd):
    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); r.settimeout(1.0)
    r.sendto(b'\xff\xff\xff\xffrcon rig ' + cmd.encode() + b'\n', (host, rport))
    out = b''
    try:
        while True:
            d, _ = r.recvfrom(65536); out += d[4:]; r.settimeout(0.3)
    except socket.timeout:
        pass
    r.close(); return out.decode('latin1')

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.02)
session = 0x50de0001 + int(time.time()) % 1000
s.sendto(MAGIC + bytes([JOIN]) + struct.pack('<IH16sB', session, 1, b'sndtest', 2), (host, port))
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

have = 0; seq = 0; ambhave = 0; buttons = 0; newest = 0; wiver = 0; wiedict = 0; witables = {}
seen = {}           # SOUND seq -> record
log = []            # (t, kind, fields)
st = dict(sndpk=0, sndbytes=0, sndrec=0, dup=0, ignored=0, evpk=0, evbytes=0, rel=0, prec=0, sent=0, music=0,
          ambpk=0, ambbytes=0, maxrec=0, partial_dropped=0)
reliable = []       # EVENT 12 records in order
precache = []
ambient = {}
rates = []          # (t, records) per SOUND packet
tstart = time.time()

def rec(b, i):
    sq, lf, sf, ent, ch, k, vol, at, pit, ox, oy, oz = struct.unpack_from('<HBBHBIBBBhhh', b, i)
    r = dict(seq=sq, lflags=lf, sflags=sf, ent=ent, chan=ch, key=k, vol=vol, atten=at, pitch=pit, org=(ox, oy, oz))
    n = 20
    if lf & 2:
        r['at'] = struct.unpack_from('<H', b, i + 20)[0]; n = 22
    return r, n

def send_input():
    global seq
    seq = (seq + 1) & 0xffff
    p = struct.pack('<IHHBBBbbbhhHBBHHHHBB', session, seq, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 30, 30, 0, 0, spawn, buttons, 0, 0)
    if v2:  # one command (link.md 'C'), then the WEAPONINFO version: the server runs it and sends WEAPONINFO
        global newest
        newest += 1
        p += b'C' + bytes([2]) + struct.pack('<IBBHH', newest, 1, 14, 0, 0) + struct.pack('<BhhhhhHB', 50, 0, 0, 0, 0, 0, buttons, 0)
        p += b'W' + struct.pack('<HH', wiver, wiedict)
    p += b'E' + struct.pack('<H', have) + b'S' + struct.pack('<HB', ambhave, 1)
    s.sendto(MAGIC + bytes([INPUT]) + p, (host, port))

def weaponinfo(d):
    # docs/spec/link.md WEAPONINFO, to its 'S' table (the view model's sound events)
    sess, sc, edict, ver = struct.unpack_from('<IHHH', d, 5); i = 15
    i += 2 + 1 + 1 + 2 + 4 + 16
    nfi = d[i]; i += 1
    for _ in range(nfi):
        i += 2 + 1 + 2 + 2 + 25 * 4
        nact = d[i]; i += 1
        for _ in range(nact):
            n = d[i + 1]; i += 2 + 2 * n
    nseq = d[i]; i += 1; evs = []
    for _ in range(nseq):
        i += 4 + 4 + 2 + 1; nev = d[i]; i += 1
        for _ in range(nev):
            fr, code, o = struct.unpack_from('<HHB', d, i); i += 5
            if code in (1004, 1008, 5004): evs.append((code, o))
    tab = []
    if i < len(d) and d[i] == ord('S'):
        n = d[i + 1]; i += 2
        for _ in range(n):
            ns = d[i]; keys = struct.unpack_from('<%dI' % ns, d, i + 1); i += 1 + 4 * ns
            vol, pmin, pmax, at, fl, ch, dist, off = struct.unpack_from('<BBBBBBBh', d, i); i += 9
            tab.append(dict(keys=[hex(k) for k in keys], vol=vol, pitch=(pmin, pmax), atten=at, flags=fl, chan=ch, dist=dist, off=off))
        rel = (d[i], d[i + 1])
    else:
        rel = None
    return edict, ver, evs, tab, rel

def take(d):
    global have, ambhave, wiver, wiedict
    if d[4] == WEAPONINFO:
        edict, ver, evs, tab, rel = weaponinfo(d)
        if (edict, ver) not in witables:
            witables[(edict, ver)] = (evs, tab, rel)
        wiver, wiedict = ver, edict
        return
    t = time.time() - tstart
    if d[4] == SOUND:
        if loss and random.random() < loss:
            st['ignored'] += 1; return
        sess, sc, count = struct.unpack_from('<IHB', d, 5); i = 12
        st['sndpk'] += 1; st['sndbytes'] += len(d) + 28; st['maxrec'] = max(st['maxrec'], count)
        new = 0
        for _ in range(count):
            r, n = rec(d, i); i += n; st['sndrec'] += 1
            if r['seq'] in seen:
                st['dup'] += 1; continue
            seen[r['seq']] = r; new += 1
            log.append((t, 'snd', r))
        rates.append((t, new))
    elif d[4] == EVENT:
        st['evpk'] += 1; st['evbytes'] += len(d) + 28
        first, count = struct.unpack('<HB', d[9:12]); i = 12
        partial = count & 0x80; count &= 0x7f
        if partial and first != (have + 1) & 0xffff and ((first - (have + 1)) & 0xffff) < 0x8000:
            st['partial_dropped'] += 1; return
        if ((first - (have + 1)) & 0xffff) < 0x8000 and first != (have + 1) & 0xffff:
            have = (first - 1) & 0xffff
        for k in range(count):
            ty, n = d[i], d[i + 1]; body = d[i + 2:i + 2 + n]; i += 2 + n
            if (first + k) & 0xffff != (have + 1) & 0xffff:
                continue
            have = (first + k) & 0xffff
            if ty == LE_SOUND:
                r, _ = rec(body, 0); reliable.append(r); st['rel'] += 1; log.append((t, 'rel', r))
            elif ty == LE_PRECACHE:
                f, nk = struct.unpack_from('<HB', body, 0)
                precache.extend(struct.unpack_from('<%dI' % nk, body, 3)); st['prec'] += 1
            elif ty == LE_SENTENCE:
                ent, at, nw = struct.unpack_from('<HBB', body, 0); st['sent'] += 1
                log.append((t, 'sentence', dict(ent=ent, atten=at, keys=struct.unpack_from('<%dI' % nw, body, 4))))
            elif ty == LE_MUSIC:
                st['music'] += 1; log.append((t, 'music', dict(op=body[0], name=body[1:].decode('latin1'))))
    elif d[4] == AMBIENT:
        st['ambpk'] += 1; st['ambbytes'] += len(d) + 28
        sess, sc, ver, count = struct.unpack_from('<IHHB', d, 5); i = 14
        ents = {}
        for _ in range(count):
            ent, k, vol, at, pit, fl, ox, oy, oz = struct.unpack_from('<HIBBBBhhh', d, i); i += 16
            ents[ent] = (k, vol, at, pit, fl, (ox, oy, oz))
        if ver != ambhave:
            log.append((t, 'ambient', dict(version=ver, entries=len(ents))))
        ambhave = ver; ambient.clear(); ambient.update(ents)

def pump(secs):
    global buttons
    end = time.time() + secs; nxt = 0
    while time.time() < end:
        now = time.time()
        if now >= nxt:
            if fire:
                buttons = 1 if (now - tstart) % 2.0 < 1.5 else 0
            send_input(); nxt = now + 0.05
        try:
            d, _ = s.recvfrom(2000)
        except socket.timeout:
            continue
        if d[:4] == MAGIC:
            take(d)

pump(1.5)
rcon('link_god 1')
for c in cmds:
    rcon(c)
mark = time.time()
pump(secs)
stats = rcon('link_stats')  # marks the log: records made after it are not counted
pump(1.0)                    # what was on its way (sv_link_lag holds packets)
s.sendto(MAGIC + bytes([LEAVE]) + struct.pack('<I', session), (host, port))
dur = time.time() - mark

if outf:
    with open(outf, 'w') as f:
        for t, kind, r in log:
            f.write('%.3f %s %s\n' % (t, kind, ' '.join('%s=%s' % (k, (hex(v) if k == 'key' else v)) for k, v in r.items())))

ok = True
uniq = len(seen); spanned = max(1e-6, (rates[-1][0] - rates[0][0])) if len(rates) > 1 else 1
print('SOUND: %d packets, %d bytes (%.0f B/s incl. UDP/IP), %d records (%d new, %d repeats seen), at most %d a packet, '
      '%.1f sounds/s; %d packets ignored (-loss %.2f)' % (st['sndpk'], st['sndbytes'], st['sndbytes'] / dur, st['sndrec'], uniq,
      st['dup'], st['maxrec'], uniq / dur, st['ignored'], loss))
print('EVENT: %d packets, %d bytes; %d SOUND records (12), %d PRECACHE (15) with %d keys, %d SENTENCE, %d MUSIC' % (
    st['evpk'], st['evbytes'], st['rel'], st['prec'], len(precache), st['sent'], st['music']))
print('AMBIENT: %d packets, %d bytes, version %d, %d entries' % (st['ambpk'], st['ambbytes'], ambhave, len(ambient)))
sizes = {}
for t, kind, r in log:
    if kind in ('snd', 'rel'):
        sizes[r['lflags']] = sizes.get(r['lflags'], 0) + 1
for (e, v), (evs, tab, rel) in witables.items():
    print('WEAPONINFO edict %d version %d: %d sound events %s, table of %d, release %s' % (e, v, len(evs), evs[:8], len(tab), rel))
    for k, t in enumerate(tab):
        print('  shader %d: %s' % (k, t))
print('lflags:', ' '.join('%d:%d' % kv for kv in sorted(sizes.items())))

if logf:
    lines = open(logf, encoding='latin1').read().splitlines()
    made = {}; made_rel = []; svsnd = []; server_side = []; byname = {}; paths = {}
    # LINKSND <dev> <seq> <path> <name> ent chan vol atten pitch flags x y z [at]
    for ln in lines:
        if 'rcon from' in ln and 'link_stats' in ln:
            break  # the test stopped listening here: what comes later never had a chance
        m = re.match(r'LINKSND (\d+) (\d+) (\S+) (\S+) (-?\d+) (-?\d+) (\d+) (\d+) (\d+) (\d+) (-?\d+) (-?\d+) (-?\d+)', ln)
        if m:
            sq = int(m.group(2)); path = m.group(3); name = m.group(4)
            paths[path] = paths.get(path, 0) + 1
            r = dict(key=key(name), ent=int(m.group(5)), chan=int(m.group(6)), vol=int(m.group(7)), atten=int(m.group(8)),
                     pitch=int(m.group(9)), name=name, path=path, cs=(int(m.group(10)) >> 8) & 8)
            if path in ('snd', 'loop') and not r['cs']:
                server_side.append(name)
            if sq == 0 and path in ('snd', 'loop', 'imp', 'mev'):
                made_rel.append(r)  # EVENT 12 (amb, vm, sent and music lines describe other packets)
            elif sq:
                made[sq] = r
            continue
        m = re.match(r'SVSND (\d+) (\S+)', ln)
        if m:
            svsnd.append(m.group(2))
    lost = [sq for sq in made if sq not in seen]
    wrong = [sq for sq in made if sq in seen and (seen[sq]['key'] != made[sq]['key'] or seen[sq]['ent'] != made[sq]['ent']
             or seen[sq]['vol'] != made[sq]['vol'] or seen[sq]['pitch'] != made[sq]['pitch'])]
    extra = [sq for sq in seen if sq not in made]
    relok = [r['key'] for r in reliable] == [r['key'] for r in made_rel][:len(reliable)]
    relmiss = len(made_rel) - len(reliable)
    exp_lost = len(made) * loss * loss
    print('server log: %d SOUND records made, %d arrived, %d lost (%.1f expected from -loss), %d with other values, %d not in the log; '
          '%d reliable made, %d arrived in order: %s' % (len(made), len(made) - len(lost), len(lost), exp_lost, len(wrong),
          len(extra), len(made_rel), len(reliable), 'yes' if relok else 'NO'))
    print('by path:', ' '.join('%s:%d' % kv for kv in sorted(paths.items())))
    # E1/E2: every svc_sound the PC would get, while we were in the game, as a snd/loop record
    nsnd = len(server_side)
    unmatched = {}
    for n in svsnd:
        unmatched[n] = unmatched.get(n, 0) + 1
    for n in server_side:
        unmatched[n] = unmatched.get(n, 0) - 1
    unmatched = {k: v for k, v in unmatched.items() if v}
    print('PC-side sounds (SVSND) %d; server-side snd+loop records %d; names that differ: %s' % (len(svsnd), nsnd,
          unmatched or 'none'))
    top = {}
    for n in svsnd:
        top[n] = top.get(n, 0) + 1
    print('most heard:', ', '.join('%s %d' % kv for kv in sorted(top.items(), key=lambda kv: -kv[1])[:12]))
    if len(lost) > max(2, 3 * exp_lost) or wrong or not relok or relmiss > 2 or unmatched:
        ok = False
print(stats.strip())
print('ok' if ok else 'FAIL')
sys.exit(0 if ok else 1)
