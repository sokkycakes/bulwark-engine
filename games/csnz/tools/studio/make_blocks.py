#!/usr/bin/env python3
"""Builds the Studio block table the studio rules progs read.

Inputs (client files, proprietary, never committed):
  <layer>/voxel/voxelprop.json   block properties (CSO VoxelProps)
  <layer>/voxel/voxel_list.csv   palette: category, cost, signal, attribute, theme
Output (derived, also never committed):
  mod/csnz/data/studio/blocks.txt  one tab-separated line per block

Columns: id name shape contents rgba flags texture cost category entity
         size(x y z) group signal attribute display model use_offset param_i
         exit_type hp damage(to percent) light_type light(r g b)

Evidence levels (see STUDIO_FEATURE_MATRIX.md):
  OBSERVED  ids, names, move/display/entity types, sizes, groups, costs,
            categories, signal kinds, texture paths, light types and light
            intensities come from the files.
  SOURCE    light_type is CSO's VoxelLightType number; light_intensity is
            one number for all three channels or [r, g, b] (hw.dll
            FUN_021f8000 reads either), 0..15 per channel.
  DESIGN    the shape mapping from move_type to Bulwark's collision shapes and
            every colour: the client's block textures are not acquired, so
            colours come from the block name (COLORBOX_<colour>_<shade>) or a
            keyword table. Replace with texture averages once textures exist.
Run: make_blocks.py <voxelprop.json> <voxel_list.csv> <out.txt>
"""
import csv, io, json, sys

SHAPE = {  # move_type -> (shape, contents) ; DESIGN mapping, see the matrix
    'NONE': (7, 0), 'BOX': (1, 1), 'SLOPE': (2, 1), 'REVERSED_SLOPE': (3, 1),
    'BOX_UNDER_SLOPE': (1, 1), 'BOX_BEHIND_SLOPE': (1, 1),
    'LADDER': (7, 16384), 'LADDER_ONOFF': (7, 16384), 'WATER': (7, 32),
    'SLAB': (4, 1), 'SLAB_0': (4, 1), 'SLAB_1': (5, 1), 'HATCH': (5, 1),
    'DOOR': (6, 1),
    'FENCE_A0': (6, 1), 'FENCE_A1': (6, 1), 'FENCE_B0': (6, 1), 'FENCE_B1': (6, 1),
    'FENCE_C0': (6, 1), 'FENCE_C1': (6, 1), 'FENCE_B0S1': (6, 1), 'FENCE_B1S1': (6, 1),
    'FENCE_C0S1': (6, 1), 'FENCE_C1S1': (6, 1),
    'SURF23_0': (4, 1), 'SURF23_1': (4, 1), 'SURF23_2': (4, 1), 'SURF23_3': (4, 1),
}

COLOURS = {
    'RED': (200, 50, 45), 'ORANGE': (230, 130, 40), 'YELLOW': (235, 210, 60), 'GREEN': (70, 170, 70),
    'BLUE': (50, 90, 200), 'VIOLET': (130, 70, 180), 'PINK': (230, 130, 170), 'CYAN': (60, 190, 200),
    'BROWN': (120, 80, 45), 'GRAY': (128, 128, 128), 'WHITE': (235, 235, 235), 'BLACK': (35, 35, 35),
}
KEYWORDS = [  # first match wins; DESIGN placeholder colours
    ('WATER', (60, 110, 200)), ('GLASS', (170, 210, 230)), ('WINDOW', (170, 210, 230)),
    ('GRASS', (90, 150, 60)), ('LEAF', (70, 130, 50)), ('TREE', (80, 120, 50)),
    ('SAND', (205, 185, 130)), ('DUST', (195, 170, 120)), ('WOOD', (140, 100, 60)),
    ('BRICK', (150, 75, 55)), ('STONE', (120, 120, 115)), ('CEMENT', (150, 150, 145)),
    ('CONCRETE', (145, 145, 140)), ('ASPHALT', (60, 60, 62)), ('METAL', (110, 115, 120)),
    ('MATAL', (110, 115, 120)), ('IRON', (100, 100, 105)), ('STEEL', (110, 115, 120)),
    ('SNOW', (240, 245, 250)), ('ICE', (190, 220, 240)), ('LAVA', (220, 80, 20)),
    ('ROOF', (120, 60, 50)), ('TILE', (175, 170, 160)), ('CONTAINER', (150, 60, 50)),
]


def merged(entry, default):
    out = json.loads(json.dumps(default))
    for k, v in entry.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            for k2, v2 in v.items():
                if isinstance(v2, dict) and isinstance(out[k].get(k2), dict):
                    out[k][k2].update(v2)
                else:
                    out[k][k2] = v2
        else:
            out[k] = v
    return out


def colour(name, display, alpha_hint):
    parts = name.split('_')
    if parts[0] == 'COLORBOX' and len(parts) >= 3 and parts[1] in COLOURS:
        r, g, b = COLOURS[parts[1]]
        shade = 1.15 - 0.1 * (int(parts[2]) if parts[2].isdigit() else 3)
        return tuple(max(0, min(255, int(c * shade))) for c in (r, g, b)) + (255,)
    if parts[0] == 'NUMBER' and len(parts) >= 2:
        return COLOURS.get(parts[1], (128, 128, 128)) + (255,)
    for key, rgb in KEYWORDS:
        if key in name:
            a = 150 if key in ('WATER', 'GLASS', 'WINDOW', 'ICE') else 255
            return rgb + (a,)
    return (150, 140, 125, alpha_hint)


def main(prop_path, list_path, out_path):
    d = json.load(open(prop_path, encoding='utf-8'))
    props = d['VoxelProps']
    default = props[0]
    ids = d['VoxelID']
    raw = open(list_path, 'rb').read().decode('cp949', errors='replace')
    palette = {row[1]: row for row in list(csv.reader(io.StringIO(raw)))[1:] if len(row) > 8}
    lines = []
    for e in props[1:]:
        p = merged(e, default)
        name = p['id']
        if name not in ids:
            continue
        mt = p.get('move_type') or 'NONE'
        shape, contents = SHAPE.get(mt, (1, 1))
        if p.get('is_clip'):
            contents |= 65536
        disp = p.get('display_type') or 'NONE'
        flags = 0
        alpha = 255
        if shape == 7 and contents == 0:
            # decorations drawn by models in CSO: a faint marker cube here
            alpha = 90
        model = (p.get('mdl') or {}).get('filename', '')
        if name.startswith('__') or (disp == 'NONE' and not model):
            flags |= 1  # VXF_NODRAW
        if p.get('light_type') == 'POINTLIGHT':
            flags |= 2  # VXF_FULLBRIGHT
        ltype = d['VoxelLightType'].get(p.get('light_type') or 'NONE', 0)
        li = p.get('light_intensity') or 0
        lrgb = li if isinstance(li, list) else [li, li, li]
        lrgb = ' '.join(str(max(0, min(15, int(c)))) for c in (list(lrgb) + [0, 0, 0])[:3])
        r, g, b, a = colour(name, disp, alpha)
        if alpha < 255:
            a = min(a, alpha)
        tex = (p.get('mat', {}).get('top', {}) or {}).get('texture1', '')
        row = palette.get(name)
        cost = row[5] if row else '0'
        cat = row[2] if row else '-1'
        signal = row[6] if row else '0'
        attr = row[7] if row else '0'
        size = p.get('size') or [1, 1, 1]
        group = ','.join(p.get('group') or [])
        uo = ' '.join(str(x) for x in (p.get('use_offset') or [0, 0, 0]))
        pi = ' '.join(str(x) for x in (p.get('param_i') or []))
        ent = p.get('entity_type') or 'NONE'
        exit_type = p.get('exit_type') or 'PLAYING'
        hp = p.get('hp') or 0
        dmg = p.get('damage') or {}
        dmg_s = '%s %s' % (dmg.get('damage_to', 'TO_NONE'), dmg.get('damage_percent', 0))
        lines.append('\t'.join(str(x) for x in [
            ids[name], name, shape, contents, f'{r} {g} {b} {a}', flags, tex or '-', cost, cat,
            ent, ' '.join(str(s) for s in size), group or '-', signal, attr, disp, model or '-', uo, pi or '-',
            exit_type, hp, dmg_s, ltype, lrgb]))
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('// generated by tools/studio/make_blocks.py from voxelprop.json + voxel_list.csv; do not commit\n')
        f.write('\n'.join(lines) + '\n')
    print(f'{len(lines)} blocks -> {out_path}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
