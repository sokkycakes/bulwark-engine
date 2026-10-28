# Bulwark editor for Godot

`addons/bulwark` turns the Godot editor into a map editor for Bulwark: you
place Nuclide entities against a preview of the world, edit their keys with
proper widgets, and export s&box-format `.scene` and `.prefab` files
([scene format](../../docs/spec/scene-format.md)) that Bulwark loads with
`map <name>`.

It needs Godot 4.5 or newer. Two editor features (setting the four-view
layout and the snap grid from the plugin) need a small engine change that
is in Bulwark's Godot build, `sokkycakes/godot`. On a stock Godot they say
so and everything else works.

## Setting up

Either open this folder as a Godot project, or copy `addons/bulwark` into an
existing project (Nuclide's `worldsrc`, say) and enable **Bulwark** under
Project Settings > Plugins. Then, under Project Settings > Bulwark (turn on
Advanced Settings):

| Setting | Meaning |
|---|---|
| Game Directory | The folder that holds `maps/` and `prefabs/`, e.g. Nuclide's `base`. Scenes are written here, prefabs and worlds are read from here |
| Engine Executable | The `bulwark` binary, for Run map |
| Base Directory, Games | `-basedir` and `-game` for Run map. Left empty, the game directory's parent and name are used |
| Entity Definitions | Files or folders to read classes from. Left empty, the plugin looks for `../Tools/base_entities.def`, `decls/def` and `*.fgd` beside the game |
| Launch Arguments | Extra arguments before `+map` |
| Units Per Meter | 32, as in Stiletto. Godot metres times this are Bulwark units |

Entity definitions can be Radiant `QUAKED` comments (`Tools/base_entities.def`,
which Nuclide generates from its source), Valve FGD files, and Nuclide's
`entityDef` decls.

## Working

**Project > Tools > Bulwark** has everything:

- **New Bulwark Scene** makes a scene whose root is a `BulwarkScene3D`.
  Set its **World** to a BSP or FTEW (`ai`, `lab.ftew`): the world is drawn
  in the editor as a preview and becomes the scene's `MapInstance`.
- **Import .scene / .prefab** opens any Bulwark or s&box scene for editing,
  including ones written by `bwscene frombsp`, so a BSP's entities can be
  edited as a scene.
- **Export Scene** writes `maps/<name>.scene` (or `prefabs/<name>.prefab`).
- **Export and Run Map** (F9 in the 3D view, as in Hammer) exports and
  starts Bulwark on the map.
- **Find Entity** (Ctrl+Shift+F) is Hammer's entity report: type to filter
  by class, targetname, name or any key value; Enter selects the entity and
  centres the views on it.
- **Hammer Layout** sets top, front and side orthographic views and a
  perspective view. **Single View** goes back.
- **Use Bulwark Editor Profile** hides what map work doesn't need: the Asset
  Library, the Game view, and 2D and UI nodes in Create Node.

The **Entities** dock lists every class, grouped by prefix, with a search
box that also matches descriptions and key names. Select a class and press
**Place** (or Shift+E in the 3D view), then click a surface: the entity's
box sits on it, snapped to the grid. Shift-click keeps placing; Escape or
right-click stops. With a group node selected, new entities go inside it.

The 3D toolbar shows the grid size in Bulwark units; `[` and `]` halve and
double it, and moves snap to it. It also shows the selection's class and
origin in Bulwark units.

Selecting an entity shows its keys laid out from its class: choices as
menus, spawnflags as checkboxes, colours with a picker, targets with a menu
of the scene's targetnames, models and sounds with a file picker, and
outputs as `target,Input,parameter,delay,times`. Origin and angles can be
typed in Bulwark units and degrees. Keys the class doesn't define are listed
under **Other keys**. **Select targets** and **Select users** follow links
both ways, and the 3D view draws an orange line from each entity to the
entities it targets.

### Nodes

| Node | Becomes |
|---|---|
| `BulwarkScene3D` | the file; for a prefab, its root object, which can be an entity too |
| `BulwarkEntity3D` | a GameObject with a `Bulwark.Entity` component. The node's -Z is the facing |
| `BulwarkPrefab3D` | a prefab instance. The prefab is drawn under it as a preview |
| any other node holding those | a plain GameObject, so groups survive |

Meshes, CSG and lights are not written to the scene: they are previews, or
world geometry for Stiletto.

### With Stiletto

In a project that also has Stiletto's `stiletto_tools`, a scene whose root
is an `FTEWorld3D` exports both ways: Stiletto writes `maps/<map>.ftew`
from the geometry and its entities, and the Bulwark scene names that world
and holds the Bulwark entities. Bulwark entities set `fte_ignore`, so
Stiletto doesn't export them a second time.

### What is kept

Importing keeps what the editor doesn't show in node metadata: other
components, tags and network settings, file properties, prefab instance
patches, and each spawn key's JSON type (number, bool, array). Exporting
writes them back, so a file goes through Godot unchanged apart from what was
edited, except that Godot stores spawn keys in alphabetical order.
References to other objects import as `@<guid>` and export as references
again.

## Tests

```sh
godot --headless --path tools/godot --script res://tests/run_tests.gd -- "$PWD"
```

Set `BULWARK_DEFS` to Nuclide's `Tools/base_entities.def` and `BULWARK_BSP`
to a Q3-family BSP to include them. `tests/check_with_bwscene.sh` also runs
the exported files through `bwscene ents` and compares the entities with
the originals'.

## Not done yet

- Brush entities need geometry from the world (Stiletto `FTEEntity3D`
  colliders, or the BSP's models); the plugin places them as points.
- Q1-family BSPs have no preview. Q3-family patches are tessellated
  coarsely and lightmaps are not drawn.
- Model previews for `model` keys.
- Applying prefab instance patches in the preview.
