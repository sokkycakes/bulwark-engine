#!/usr/bin/env python3
"""Generate Bulwark's Steam Input files into data/steam_input/:

  steam_input_manifest.vdf   the In-Game Actions manifest Steam is pointed at
                             (ISteamInput::SetInputActionManifestFilePath)
  bulwark_<type>.vdf         a default layout per controller type
  bindings.txt               the engine command each SI_<ACTION> key runs

Everything comes from the tables below, so the actions, the layouts and the
engine's bindings can't drift apart (tests/steaminput/check_manifest.py
checks the output). A game overrides any of it by shipping its own
steam_input/ folder. The layout files follow the controller_mappings format
Steam exports (steam://dumpcontrollerconfig?appid=<id>); to change a layout,
edit it in Steam's configurator in Steam Input Layout Dev Mode, export it
and replace the file, or edit the tables here.

    python3 tools/steaminput/gen_configs.py [out dir]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# --- the actions -------------------------------------------------------------
# (action, title, engine command). Digital actions named key_<KEY> press that
# engine key instead, so menus get the controller keys they already handle.
INGAME_ANALOG = [
    ('move', 'Move', 'joystick_move'),
    ('look', 'Look', 'absolute_mouse'),
]
INGAME_BUTTONS = [
    ('jump', 'Jump', '+jump'),
    ('duck', 'Crouch', '+duck'),
    ('attack', 'Fire', '+attack'),
    ('attack2', 'Alternate fire', '+attack2'),
    ('reload', 'Reload', '+reload'),
    ('use', 'Use', '+use'),
    ('sprint', 'Sprint', '+sprint'),
    ('weapnext', 'Next weapon', 'weapnext'),
    ('weapprev', 'Previous weapon', 'weapprev'),
    ('showscores', 'Scores', '+showscores'),
    ('flashlight', 'Flashlight', 'impulse 100'),
    ('pause', 'Pause menu', 'togglemenu'),
    ('slot1', 'Weapon slot 1', 'slot1'),
    ('slot2', 'Weapon slot 2', 'slot2'),
    ('slot3', 'Weapon slot 3', 'slot3'),
    ('slot4', 'Weapon slot 4', 'slot4'),
]
MENU_ANALOG = [
    ('menu_move', 'Move through the menu', 'joystick_move'),
]
MENU_BUTTONS = [
    ('key_gp_a', 'Select', None),
    ('key_gp_b', 'Back', None),
    ('key_gp_x', 'Menu option X', None),
    ('key_gp_y', 'Menu option Y', None),
    ('key_gp_dpad_up', 'Up', None),
    ('key_gp_dpad_down', 'Down', None),
    ('key_gp_dpad_left', 'Left', None),
    ('key_gp_dpad_right', 'Right', None),
    ('key_gp_lshoulder', 'Previous tab', None),
    ('key_gp_rshoulder', 'Next tab', None),
    ('key_gp_ltrigger', 'Page left', None),
    ('key_gp_rtrigger', 'Page right', None),
    ('key_gp_menu', 'Close menu', None),
    ('key_gp_view', 'Menu view', None),
]
SETS = [
    ('InGame', 'In game', INGAME_ANALOG, INGAME_BUTTONS),
    ('Menu', 'Menus', MENU_ANALOG, MENU_BUTTONS),
]

# --- the default layouts -----------------------------------------------------
# physical button (by position) -> action, per set
LAYOUT = {
    'InGame': {
        'south': 'jump', 'east': 'duck', 'west': 'reload', 'north': 'use',
        'rt': 'attack', 'lt': 'attack2', 'rb': 'weapnext', 'lb': 'weapprev',
        'lclick': 'sprint', 'rclick': 'flashlight', 'back': 'showscores', 'start': 'pause',
        'dup': 'slot1', 'dright': 'slot2', 'ddown': 'slot3', 'dleft': 'slot4',
        'lstick': 'move', 'rstick': 'look', 'rpad': 'look',
    },
    'Menu': {
        'south': 'key_gp_a', 'east': 'key_gp_b', 'west': 'key_gp_x', 'north': 'key_gp_y',
        'rt': 'key_gp_rtrigger', 'lt': 'key_gp_ltrigger', 'rb': 'key_gp_rshoulder', 'lb': 'key_gp_lshoulder',
        'back': 'key_gp_view', 'start': 'key_gp_menu',
        'dup': 'key_gp_dpad_up', 'dright': 'key_gp_dpad_right', 'ddown': 'key_gp_dpad_down',
        'dleft': 'key_gp_dpad_left', 'lstick': 'menu_move',
    },
}

# controller type -> what it has. The original Steam Controller has one
# stick and two trackpads: its left pad stands in for the d-pad and its right
# pad for the right stick.
CONTROLLERS = {
    'controller_neptune': {'title': 'Steam Deck', 'rstick': True, 'rpad': True, 'dpad': 'dpad'},
    'controller_steamcontroller_gordon': {'title': 'Steam Controller', 'rstick': False, 'rpad': True,
                                          'dpad': 'left_trackpad'},
    'controller_xboxone': {'title': 'Xbox', 'rstick': True, 'rpad': False, 'dpad': 'dpad'},
    'controller_xbox360': {'title': 'Xbox 360', 'rstick': True, 'rpad': False, 'dpad': 'dpad'},
    'controller_ps4': {'title': 'PlayStation 4', 'rstick': True, 'rpad': False, 'dpad': 'dpad'},
    'controller_ps5': {'title': 'PlayStation 5', 'rstick': True, 'rpad': False, 'dpad': 'dpad'},
    'controller_switch_pro': {'title': 'Switch Pro', 'rstick': True, 'rpad': False, 'dpad': 'dpad'},
    'controller_generic': {'title': 'Gamepad', 'rstick': True, 'rpad': False, 'dpad': 'dpad'},
}


def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


class VDF:
    def __init__(self):
        self.lines = []
        self.depth = 0

    def kv(self, k, v):
        self.lines.append('\t' * self.depth + q(k) + '\t\t' + q(str(v)))

    def open(self, k):
        self.lines.append('\t' * self.depth + q(k))
        self.lines.append('\t' * self.depth + '{')
        self.depth += 1

    def close(self):
        self.depth -= 1
        self.lines.append('\t' * self.depth + '}')

    def text(self):
        return '\n'.join(self.lines) + '\n'


def write_actions(v):
    v.open('actions')
    for name, title, analog, buttons in SETS:
        v.open(name)
        v.kv('title', '#Set_' + name)
        v.kv('legacy_set', '0')
        v.open('StickPadGyro')
        for a, t, mode in analog:
            v.open(a)
            v.kv('title', '#Action_' + a)
            v.kv('input_mode', mode)
            v.close()
        v.close()
        v.open('Button')
        for a, t, _ in buttons:
            v.kv(a, '#Action_' + a)
        v.close()
        v.close()
    v.close()


def write_localization(v):
    v.open('localization')
    v.open('english')
    v.kv('title', 'Bulwark')
    v.kv('description', 'Bulwark default controls')
    for name, title, analog, buttons in SETS:
        v.kv('Set_' + name, title)
        for a, t, *_ in analog + buttons:
            v.kv('Action_' + a, t)
    v.close()
    v.close()


def manifest():
    v = VDF()
    v.lines.append('// Bulwark Steam Input actions. Generated by tools/steaminput/gen_configs.py;')
    v.lines.append('// see docs/spec/steam-input.md.')
    v.open('Action Manifest')
    v.open('configurations')
    for ctype in CONTROLLERS:
        v.open(ctype)
        v.open('0')
        v.kv('path', 'bulwark_%s.vdf' % ctype.replace('controller_', ''))
        v.close()
        v.close()
    v.close()
    write_actions(v)
    write_localization(v)
    v.close()
    return v.text()


def binding(setname, action):
    return 'game_action %s %s, , ' % (setname, action)


def button_input(v, name, setname, action):
    v.open(name)
    v.open('activators')
    v.open('Full_Press')
    v.open('bindings')
    v.kv('binding', binding(setname, action))
    v.close()
    v.close()
    v.close()
    v.close()


def config(ctype, caps):
    v = VDF()
    gid = [0]
    presets = []

    def group(mode, inputs, source, setname, gameaction=None, settings=None):
        my = gid[0]
        gid[0] += 1
        v.open('group')
        v.kv('id', my)
        v.kv('mode', mode)
        v.kv('name', '')
        v.kv('description', '')
        v.open('inputs')
        for inp, action in inputs:
            if action:
                button_input(v, inp, setname, action)
        v.close()
        if gameaction:
            v.open('gameactions')
            v.kv(setname, gameaction)
            v.close()
        if settings:
            v.open('settings')
            for k, val in settings.items():
                v.kv(k, val)
            v.close()
        v.close()
        presets[-1][1].append((my, source))

    v.lines.append('// Bulwark default layout for %s. Generated by tools/steaminput/gen_configs.py.' % caps['title'])
    v.open('controller_mappings')
    v.kv('version', '3')
    v.kv('revision', '1')
    v.kv('title', '#title')
    v.kv('description', '#description')
    v.kv('creator', '')
    v.kv('progenitor', '')
    v.kv('url', '')
    v.kv('export_type', 'unknown')
    v.kv('controller_type', ctype)
    v.kv('major_revision', '0')
    v.kv('minor_revision', '0')
    v.kv('Timestamp', '0')
    write_actions(v)
    write_localization(v)
    for setname, *_ in SETS:
        lay = LAYOUT[setname]
        presets.append((setname, []))
        group('four_buttons', [('button_a', lay.get('south')), ('button_b', lay.get('east')),
                               ('button_x', lay.get('west')), ('button_y', lay.get('north'))],
              'button_diamond', setname)
        dpad = [('dpad_north', lay.get('dup')), ('dpad_south', lay.get('ddown')),
                ('dpad_east', lay.get('dright')), ('dpad_west', lay.get('dleft'))]
        group('dpad', dpad, caps['dpad'], setname)
        group('joystick_move', [('click', lay.get('lclick'))], 'joystick', setname, lay.get('lstick'))
        group('trigger', [('click', lay.get('lt'))], 'left_trigger', setname)
        group('trigger', [('click', lay.get('rt'))], 'right_trigger', setname)
        group('switches', [('left_bumper', lay.get('lb')), ('right_bumper', lay.get('rb')),
                           ('button_escape', lay.get('start')), ('button_menu', lay.get('back'))],
              'switch', setname)
        if caps['rstick'] and lay.get('rstick'):
            group('joystick_camera', [('click', lay.get('rclick'))], 'right_joystick', setname, lay.get('rstick'))
        if caps['rpad'] and lay.get('rpad'):
            group('absolute_mouse', [('click', None if caps['rstick'] else lay.get('rclick'))], 'right_trackpad',
                  setname, lay.get('rpad'))
    for i, (setname, groups) in enumerate(presets):
        v.open('preset')
        v.kv('id', i)
        v.kv('name', setname)
        v.open('group_source_bindings')
        for g, source in groups:
            v.kv(str(g), source + ' active')
        v.close()
        v.close()
    v.open('settings')
    v.close()
    v.close()
    return v.text()


def bindings():
    out = ['// Steam Input digital action -> the engine command its SI_<ACTION> key runs',
           '// when nothing else binds that key. Generated by tools/steaminput/gen_configs.py.']
    for name, title, analog, buttons in SETS:
        for a, t, cmd in buttons:
            if cmd:
                out.append('%s %s' % (a, cmd))
    return '\n'.join(out) + '\n'


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'data/steam_input')
    os.makedirs(out, exist_ok=True)
    files = {'steam_input_manifest.vdf': manifest(), 'bindings.txt': bindings()}
    for ctype, caps in CONTROLLERS.items():
        files['bulwark_%s.vdf' % ctype.replace('controller_', '')] = config(ctype, caps)
    for name, text in files.items():
        with open(os.path.join(out, name), 'w', newline='\n') as f:
            f.write(text)
    print('%d files into %s' % (len(files), out))


if __name__ == '__main__':
    main()
