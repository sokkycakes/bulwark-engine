#!/usr/bin/env python3
"""Writes a PCF file from a BDF file, in a chosen byte order, bit order,
glyph padding and scan unit, optionally with compressed metrics or gzipped,
to test the PCF reader (tests/bmfont). Written from the X.Org PCF format
description. ISC licence, see LICENSE.

  mkpcf.py in.bdf out.pcf [--msbyte] [--msbit] [--pad N] [--unit N] [--compressed] [--gzip]
"""
import gzip, struct, sys

def parse_bdf(path):
    props, glyphs, fbb = {}, [], (0, 0, 0, 0)
    lines = iter(open(path).read().splitlines())
    for line in lines:
        w = line.split(None, 1)
        if not w:
            continue
        if w[0] == 'FONTBOUNDINGBOX':
            fbb = tuple(int(x) for x in w[1].split())
        elif w[0] == 'STARTPROPERTIES':
            for p in lines:
                if p.startswith('ENDPROPERTIES'):
                    break
                k, v = p.split(None, 1)
                props[k] = v[1:-1] if v.startswith('"') else int(v)
        elif w[0] == 'STARTCHAR':
            g = {'bbx': fbb}
            for c in lines:
                k = c.split()
                if k[0] == 'ENCODING':
                    g['code'] = int(k[1])
                elif k[0] == 'DWIDTH':
                    g['adv'] = int(k[1])
                elif k[0] == 'BBX':
                    g['bbx'] = tuple(int(x) for x in k[1:5])
                elif k[0] == 'BITMAP':
                    rows = []
                    for r in lines:
                        if r.startswith('ENDCHAR'):
                            break
                        rows.append(r.strip())
                    g['rows'] = rows
                    break
            glyphs.append(g)
    return props, glyphs

def main():
    a = sys.argv[1:]
    src, dst = a[0], a[1]
    msbyte, msbit = '--msbyte' in a, '--msbit' in a
    pad = int(a[a.index('--pad') + 1]) if '--pad' in a else 4
    unit = int(a[a.index('--unit') + 1]) if '--unit' in a else 1
    comp = '--compressed' in a
    props, glyphs = parse_bdf(src)
    padbits = {1: 0, 2: 1, 4: 2, 8: 3}[pad]
    unitbits = {1: 0, 2: 1, 4: 2}[unit]
    fmt = padbits | (4 if msbyte else 0) | (8 if msbit else 0) | (unitbits << 4)
    e = '>' if msbyte else '<'

    def table_props():
        strings = b''
        def s(x):
            nonlocal strings
            off = len(strings)
            strings += x.encode() + b'\0'
            return off
        recs = b''
        for k, v in props.items():
            if isinstance(v, str):
                recs += struct.pack(e + 'IbI', s(k), 1, s(v))
            else:
                recs += struct.pack(e + 'IbI', s(k), 0, v & 0xffffffff)
        n = len(props)
        body = struct.pack('<I', fmt) + struct.pack(e + 'I', n) + recs
        if n & 3:
            body += b'\0' * (4 - (n & 3))
        return body + struct.pack(e + 'I', len(strings)) + strings

    def metric(g):
        w, h, xo, yo = g['bbx']
        return xo, xo + w, g['adv'], yo + h, -yo

    def table_metrics():
        if comp:
            body = struct.pack('<I', fmt | 0x100) + struct.pack(e + 'H', len(glyphs))
            for g in glyphs:
                body += bytes(v + 0x80 for v in metric(g))
        else:
            body = struct.pack('<I', fmt) + struct.pack(e + 'I', len(glyphs))
            for g in glyphs:
                body += struct.pack(e + '6h', *metric(g), 0)
        return body

    def table_bitmaps():
        data, offsets = b'', []
        for g in glyphs:
            w, h = g['bbx'][0], g['bbx'][1]
            stride = ((w + 7) // 8 + pad - 1) // pad * pad
            offsets.append(len(data))
            for r in g['rows'][:h]:
                row = bytearray(bytes.fromhex(r.ljust(2 * ((w + 7) // 8), '0'))[: (w + 7) // 8])
                row += b'\0' * (stride - len(row))
                if not msbit:
                    row = bytearray(int('{:08b}'.format(b)[::-1], 2) for b in row)
                if msbyte != msbit and unit > 1:
                    for k in range(0, stride, unit):
                        row[k:k + unit] = row[k:k + unit][::-1]
                data += bytes(row)
        body = struct.pack('<I', fmt) + struct.pack(e + 'I', len(glyphs))
        body += b''.join(struct.pack(e + 'I', o) for o in offsets)
        body += struct.pack(e + '4I', len(data), len(data), len(data), len(data))
        return body + data

    def table_encodings():
        codes = {g['code']: i for i, g in enumerate(glyphs)}
        b1 = sorted({c >> 8 for c in codes}); b2 = [c & 0xff for c in codes]
        min1, max1, min2, max2 = b1[0], b1[-1], min(b2), max(b2)
        body = struct.pack('<I', fmt) + struct.pack(e + '5h', min2, max2, min1, max1, props.get('DEFAULT_CHAR', 0))
        for a1 in range(min1, max1 + 1):
            for a2 in range(min2, max2 + 1):
                body += struct.pack(e + 'H', codes.get(a1 << 8 | a2, 0xffff))
        return body

    def table_accel():
        # the flag bytes, the font's ascent and descent, max overlap, then the
        # min and max bounds as uncompressed metrics
        ms = [metric(g) for g in glyphs]
        lo = [min(m[i] for m in ms) for i in range(5)]
        hi = [max(m[i] for m in ms) for i in range(5)]
        body = struct.pack('<I', fmt) + bytes([0, 0, 0, 0, 0, 0, 0, 0])
        body += struct.pack(e + 'iii', props.get('FONT_ASCENT', 0), props.get('FONT_DESCENT', 0), 0)
        body += struct.pack(e + '6h', *lo, 0) + struct.pack(e + '6h', *hi, 0)
        return body

    tables = [(1 << 0, table_props()), (1 << 1, table_accel()), (1 << 2, table_metrics()), (1 << 3, table_bitmaps()),
              (1 << 5, table_encodings())]
    out = b'\1fcp' + struct.pack('<I', len(tables))
    off = 8 + 16 * len(tables)
    heads, bodies = b'', b''
    for t, body in tables:
        body += b'\0' * (-len(body) % 4)
        heads += struct.pack('<4I', t, struct.unpack('<I', body[:4])[0], len(body), off)
        bodies += body
        off += len(body)
    out += heads + bodies
    if '--gzip' in a:
        out = gzip.compress(out)
    open(dst, 'wb').write(out)

main()
