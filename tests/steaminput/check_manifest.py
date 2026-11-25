#!/usr/bin/env python3
"""Checks a Steam Input folder (data/steam_input, or a game's steam_input/):
the manifest parses, action names are unique across sets (Steam looks
actions up by name alone), every layout file it names exists and binds only
actions the manifest declares (digital to buttons, analog to sticks and
pads), every preset is an action set and every group sits in exactly one
preset, key_<KEY> actions name keys the engine knows, and every other
digital action has an engine command in bindings.txt.

    python3 tests/steaminput/check_manifest.py data/steam_input
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def tokens(text):
    for m in re.finditer(r'//[^\n]*|"((?:[^"\\]|\\.)*)"|([{}])|([^\s{}"]+)', text):
        if m.group(0).startswith('//'):
            continue
        if m.group(2):
            yield m.group(2), True
        else:
            yield (m.group(1) if m.group(1) is not None else m.group(3)), False


def parse(text):
    """VDF into a list of (key, value-or-list) pairs, keeping repeats."""
    toks = list(tokens(text))
    pos = 0

    def block():
        nonlocal pos
        out = []
        while pos < len(toks):
            t, brace = toks[pos]
            if brace and t == '}':
                pos += 1
                return out
            if brace:
                raise ValueError('unexpected {')
            key = t
            pos += 1
            v, vb = toks[pos]
            pos += 1
            if vb and v == '{':
                out.append((key, block()))
            elif vb:
                raise ValueError('unexpected } after ' + key)
            else:
                out.append((key, v))
        return out

    return block()


def get(block, key):
    return [v for k, v in block if k.lower() == key.lower()]


def one(block, key):
    v = get(block, key)
    if len(v) != 1:
        raise ValueError('want one %s, have %d' % (key, len(v)))
    return v[0]


def engine_keys():
    src = open(os.path.join(REPO, 'src/client/cl_input.c')).read()
    return {k.lower() for k in re.findall(r'\{"([A-Z0-9_]+)", K_', src)}


def main():
    folder = sys.argv[1]
    errors = []
    man = parse(open(os.path.join(folder, 'steam_input_manifest.vdf')).read())
    root = one(man, 'Action Manifest')
    actions = {}  # name -> (set, analog)
    sets = []
    for section in ('actions', 'action_layers'):
        for b in get(root, section):
            for setname, body in b:
                sets.append(setname)
                if not get(body, 'title') or not one(body, 'title').startswith('#'):
                    errors.append('set %s: title must be a #token' % setname)
                for group in ('StickPadGyro', 'AnalogTrigger', 'Button'):
                    for g in get(body, group):
                        for name, _ in g:
                            if name in actions:
                                errors.append('action %s is in %s and %s' % (name, actions[name][0], setname))
                            actions[name] = (setname, group != 'Button')
    loc = {k for b in get(root, 'localization') for lang in b for k, _ in lang[1]}
    for name in actions:
        if 'Action_' + name not in loc:
            errors.append('no localization for %s' % name)
    keys = engine_keys()
    bindings = {}
    bpath = os.path.join(folder, 'bindings.txt')
    if os.path.exists(bpath):
        for line in open(bpath):
            line = line.strip()
            if line and not line.startswith(('//', '#')):
                a, _, cmd = line.partition(' ')
                bindings[a] = cmd.strip()
    for name, (setname, analog) in actions.items():
        if analog:
            continue
        if name.startswith('key_'):
            if name[4:] not in keys:
                errors.append('%s: the engine has no key %s' % (name, name[4:].upper()))
        elif name not in bindings:
            errors.append('%s: no engine command in bindings.txt' % name)
    for a in bindings:
        if a not in actions:
            errors.append('bindings.txt names %s, which the manifest lacks' % a)

    configs = 0
    for b in get(root, 'configurations'):
        for ctype, entries in b:
            for _, entry in entries:
                path = os.path.join(folder, one(entry, 'path'))
                if not os.path.exists(path):
                    errors.append('%s: no %s' % (ctype, path))
                    continue
                configs += 1
                cm = one(parse(open(path).read()), 'controller_mappings')
                if one(cm, 'controller_type') != ctype:
                    errors.append('%s: controller_type is %s' % (path, one(cm, 'controller_type')))
                groups = {}
                for g in get(cm, 'group'):
                    gid = one(g, 'id')
                    if gid in groups:
                        errors.append('%s: group id %s twice' % (path, gid))
                    groups[gid] = g
                    for ga in get(g, 'gameactions'):
                        for s, a in ga:
                            if actions.get(a) != (s, True):
                                errors.append('%s: group %s binds %s %s, not an analog action of that set' % (path, gid, s, a))
                    for inputs in get(g, 'inputs'):
                        for inp, body in inputs:
                            for act in get(body, 'activators'):
                                for _, press in act:
                                    for bb in get(press, 'bindings'):
                                        for _, bind in bb:
                                            m = re.match(r'game_action (\S+) (\S+), , $', bind)
                                            if not m or actions.get(m.group(2)) != (m.group(1), False):
                                                errors.append('%s: %s binds "%s"' % (path, inp, bind))
                seen = {}
                for p in get(cm, 'preset'):
                    name = one(p, 'name')
                    if name not in sets:
                        errors.append('%s: preset %s is no action set' % (path, name))
                    for gsb in get(p, 'group_source_bindings'):
                        for gid, src in gsb:
                            if gid not in groups:
                                errors.append('%s: preset %s names group %s' % (path, name, gid))
                            if gid in seen:
                                errors.append('%s: group %s in presets %s and %s' % (path, gid, seen[gid], name))
                            seen[gid] = name
                for gid in groups:
                    if gid not in seen:
                        errors.append('%s: group %s in no preset' % (path, gid))
    for e in errors:
        print('check_manifest:', e)
    print('check_manifest: %d action sets, %d actions, %d layouts, %d problems' % (len(sets), len(actions), configs, len(errors)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
