# Scene and prefab files

Bulwark reads and writes s&box's scene format: `.scene` and `.prefab` JSON
files made of GameObjects with typed components. Files made by s&box's editor
load here, and files Bulwark saves stay readable by s&box.

Source: [Facepunch/sbox-public](https://github.com/Facepunch/sbox-public)
(MIT, copyright Facepunch Studios Ltd), read at commit `86f70929`. The
format was learned from its serializer (`engine/Sandbox.Engine/Scene/GameObject/GameObject.Serialize.cs`,
`Component.Serialize.cs`, `Resources/Scene/SceneFile*.cs`, `PrefabFile*.cs`),
its JSON patch code (`Utility/Json/Json.Diff.cs`) and its sample files. No
s&box code is copied: Bulwark's implementation is new C. The sample files in
`tests/scene/sbox/` are unmodified copies, kept with s&box's licence.

Code: `src/common/json.c` (JSON tree), `src/scene/` (files, prefabs,
entities), `tools/bwscene.c`, tests in `tests/scene/`.

## Files

A scene (`__version` 3):

```json
{
  "__guid": "…",
  "GameObjects": [ GameObject, … ],
  "SceneProperties": { "Metadata": { "Title": "…" }, … },
  "ResourceVersion": 3,
  "Title": null,
  "Description": null,
  "__references": [],
  "__version": 3
}
```

A prefab (`__version` 2) has one `"RootObject": GameObject` in place of
`GameObjects`, plus menu fields such as `ShowInMenu` and `MenuPath`.

A GameObject (`__version` 2):

| Key | Meaning |
|---|---|
| `__guid` | identity, lowercase `8-4-4-4-12` GUID |
| `Name` | display name; not unique |
| `Position` | `"x,y,z"`, local to the parent |
| `Rotation` | `"x,y,z,w"` quaternion, local to the parent |
| `Scale` | `"x,y,z"` |
| `Enabled` | `false` skips the object and everything under it |
| `Flags` | bits; Bulwark uses `2048` EditorOnly (not spawned) and `4096` Absolute (transform is world space) |
| `Tags` | comma separated |
| `Components` | array of components |
| `Children` | array of GameObjects |
| `Network*`, `OwnerTransfer` | s&box networking settings; kept, unused |

A component has `__type` (a type name, often with a `Sandbox.` prefix),
`__guid`, `__enabled`, `Flags`, and then its own properties. s&box writes
vectors and colours as comma-separated strings, enums as strings, and
references to other objects as:

```json
{ "_type": "gameobject", "go": "<GameObject guid>" }
{ "_type": "component", "component_id": "<guid>", "go": "<owner guid>", "component_type": "Rigidbody" }
```

Space: s&box is Z-up with X forward, measured in inches, the same as Quake
and Source maps. Nothing is converted.

### Loading and saving

Files are kept as JSON trees with members in file order and numbers as
written, so properties Bulwark does not understand survive a load and save
unchanged. Saving writes 2-space indented JSON, as s&box does.

Older files are upgraded on load, following s&box's upgraders of the same
numbers:

| Step | Change |
|---|---|
| scene/prefab v1 | `Id` (or `id`) becomes `__guid` on objects and components |
| scene v2 | top-level `Title`/`Description` move to `SceneProperties.Metadata` (s&box also inserts a `SceneInformation` object; Bulwark does not) |
| scene v3, prefab v2 | every object gets `Flags`, `Children` and `Components` |
| GameObject v2 | `NetworkInterpolation: false` becomes `NetworkFlags` bit 1 |
| (Bulwark) | objects and components with no `__guid` get a random one |

GameObject v1 converted old prefab instances that had no patch. That needs
the prefab's editor variables; Bulwark warns and uses the prefab unchanged.

## Prefab instances

An instance in a scene is a stub:

```json
{
  "__guid": "<instance guid>",
  "__version": 2,
  "__Prefab": "prefabs/crate.prefab",
  "__PrefabInstancePatch": {
    "AddedObjects": [ { "Id", "Parent", "PreviousElement", "ContainerProperty", "IsContainerArray", "Data" } ],
    "RemovedObjects": [ { "Id" } ],
    "PropertyOverrides": [ { "Target", "Property", "Value" } ],
    "MovedObjects": [ { "Id", "NewParent", "NewContainerProperty", "IsNewContainerArray", "NewPreviousElement" } ]
  },
  "__PrefabIdToInstanceId": { "<prefab guid>": "<instance guid>", … }
}
```

Ids in the patch are `{ "Type": "GameObject" | "Component", "IdValue": guid }`
and use the prefab's GUIDs. `Scene_Resolve` expands each instance:

1. Copy the prefab's root object and expand the prefab's own instances
   first, so the patch can reach objects inside them.
2. Apply the patch in s&box's order: removals, additions, moves, then
   property overrides. An addition or move goes after its `PreviousElement`
   in the target array, or first if there is none. Additions whose parent
   was removed are dropped.
3. Rename GUIDs. The prefab root maps to the instance's `__guid`, then
   `__PrefabIdToInstanceId` applies. Objects the table does not list get a
   GUID made from the instance GUID and the prefab GUID, so two instances
   never share GUIDs and the result is the same every load. Every string
   that is a mapped GUID is replaced, which also updates references between
   objects inside the prefab.
4. Objects the patch added may be instances themselves; they expand too.

A missing prefab becomes a disabled object named `[Missing Prefab] <path>`
that keeps the instance GUID. Nesting is capped at 16 levels, which stops a
prefab that contains itself.

Expansion happens when a scene loads. QuakeC never sees prefabs.

## Scenes and QuakeC entities

QuakeC entities are one object each with flat key/value spawn data, and
Nuclide's classes read those keys in `SpawnKey`. So a scene is turned into
entity lump text and spawned through the server's ordinary BSP entity path
(`CheckSpawn`, `__fullspawndata`, then the spawn function). The progs cannot
tell a scene entity from a BSP one.

### The entity component

```json
{
  "__type": "Bulwark.Entity",
  "__guid": "…",
  "__enabled": true,
  "Flags": 0,
  "classname": "func_door",
  "speed": 100,
  "spawnflags": 1,
  "target": { "_type": "gameobject", "go": "…" }
}
```

Each enabled `Bulwark.Entity` becomes one entity. `classname` is required.
Every other property becomes a spawn key, except names starting with `__`,
`Flags` and `OnComponent*` (s&box's event hooks). Values are converted like
this:

| JSON | Spawn key value |
|---|---|
| string | as written (`"` becomes `'`, line breaks become spaces) |
| number | as written |
| `true` / `false` | `1` / `0` |
| array of numbers or strings | joined with spaces: `[255, 128, 0]` → `255 128 0` |
| GameObject or component reference | the target object's targetname |
| `null`, other objects | left out |

### Keys the scene supplies

Unless the component sets the key itself:

- `origin`: the object's world position, after its parents' transforms.
- `angles`: the world rotation as `pitch yaw roll` in degrees, positive
  pitch looking down. Left out when it is zero.
- `scale`: the average world scale, when it is not 1. Nuclide only scales
  uniformly.
- `targetname`: the object's GUID, given when something references the
  object or it parents other entities. A `targetname` the component sets is
  used instead, and references to the object resolve to it.
- `parentname`: the targetname of the nearest ancestor object that has an
  entity. Nuclide's `ncEntity` reads `parentname` and attaches the entity.

An object with several entity components spawns all of them. The first gets
the object's targetname, and the others get `parentname` pointing at it.

### s&box components

So s&box's own sample scenes show something, a few of its components stand
in for Nuclide classes when the object has no `Bulwark.Entity`:

| s&box | Entity |
|---|---|
| `ModelRenderer` | `prop_static`, or `prop_physics` with a `Rigidbody` |
| `SkinnedModelRenderer` | `prop_dynamic`, or `prop_physics` with a `Rigidbody` |
| `SpawnPoint` | `info_player_start` |

They get `model` from `Model` and `rendercolor` from `Tint`. s&box's model
formats (`.vmdl`) do not load here, so these mostly mark positions for now.
Other components are kept in the file and skipped at spawn.

### Maps

A `MapInstance` component (s&box's own map component) names the BSP or
[FTEW world](ftew.md) that provides the world:

```json
{ "__type": "Sandbox.MapInstance", "MapName": "ai", "EnableCollision": true, "SpawnMapEntities": true }
```

`MapName` without a folder gets `maps/` in front, and without an extension
gets `.bsp`; name `foo.ftew` for an FTEW world. The BSP's entities spawn first, worldspawn first of all, then
the scene's. `SpawnMapEntities` is a Bulwark addition: `false` spawns only
the BSP's worldspawn, for scenes that hold the map's entities themselves.

`map <name>` loads `maps/<name>.scene` if there is one, otherwise
`maps/<name>.bsp`, otherwise `maps/<name>.ftew`. A scene needs a `MapInstance` for now, because the
server needs a world model. The map name the progs see is the scene's name,
and Nuclide uses it to find per-map files such as `maps/<name>.way`. So
name a scene after its BSP to keep those working.

`bwscene frombsp maps/ai.bsp ai.scene` writes a scene holding a BSP's
entities: one GameObject per entity, with `origin` and `angles` (or a
single `angle` other than -1 and -2) moved into the transform, and all
other keys left on a `Bulwark.Entity`. Its `MapInstance` has
`SpawnMapEntities: false`. Checked on Nuclide's `maps/ai.bsp`: running the
converted scene for 10 seconds ends with the same 275 entities in use as
running the BSP.

## Not done yet

- Spawning prefabs from QuakeC at runtime, and finding objects by GUID.
- s&box's GameObject `Tags`, networking settings, and `SceneProperties`
  (fixed update rate, time scale and so on) are kept but unused.
- Component property upgrades. s&box versions individual component types;
  Bulwark keeps their data as is.
- Saving a scene back after the game changes it, and an editor.
