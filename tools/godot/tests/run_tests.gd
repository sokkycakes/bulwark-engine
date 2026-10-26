extends SceneTree
## Headless tests for the Bulwark addon:
##   godot --headless --path tools/godot --script res://tests/run_tests.gd -- <repo root> [out dir]
## Optional environment: BULWARK_DEFS (a Nuclide base_entities.def or FGD),
## BULWARK_BSP (a Q3-family BSP to preview).

const Convert := preload("res://addons/bulwark/convert.gd")
const Defs := preload("res://addons/bulwark/defs.gd")
const SceneIO := preload("res://addons/bulwark/scene_io.gd")
const WorldPreview := preload("res://addons/bulwark/world_preview.gd")
const Paths := preload("res://addons/bulwark/paths.gd")
const EntityScript := preload("res://addons/bulwark/nodes/entity.gd")
const SceneScript := preload("res://addons/bulwark/nodes/scene_root.gd")
const PrefabScript := preload("res://addons/bulwark/nodes/prefab.gd")

var checks := 0
var failures := 0
var repo := ""
var out_dir := ""


func check(ok: bool, what: String) -> void:
	checks += 1
	if not ok:
		failures += 1
		printerr("FAIL: " + what)


func near(a: Vector3, b: Vector3, eps: float = 1e-3) -> bool:
	return a.distance_to(b) < eps


func _init() -> void:
	var args := OS.get_cmdline_user_args()
	repo = args[0] if args.size() > 0 else ProjectSettings.globalize_path("res://").path_join("../..").simplify_path()
	out_dir = args[1] if args.size() > 1 else OS.get_cache_dir().path_join("bulwark-godot-tests")
	DirAccess.make_dir_recursive_absolute(out_dir)
	test_convert()
	test_defs()
	test_scene_roundtrip()
	test_build_and_export()
	test_prefab_file()
	test_world_preview()
	print("%d checks, %d failed" % [checks, failures])
	quit(1 if failures > 0 else 0)


func test_convert() -> void:
	ProjectSettings.set_setting("bulwark/units_per_meter", 32.0)
	check(near(Convert.point_to_quake(Vector3(1, 2, 3)), Vector3(32, -96, 64)), "point mapping matches Stiletto")
	check(near(Convert.point_from_quake(Vector3(32, -96, 64)), Vector3(1, 2, 3)), "point mapping inverts")
	var t := Transform3D(Basis(Vector3(0.3, 1, 0.2).normalized(), 0.7).scaled(Vector3(1, 2, 3)), Vector3(1, -2, 0.5))
	var back := Convert.from_quake(Convert.to_quake(t))
	check(near(back.origin, t.origin) and near(back.basis.x, t.basis.x) and near(back.basis.z, t.basis.z), "transform round trip")
	for a in [Vector3(0, 90, 0), Vector3(30, -45, 10), Vector3(-80, 170, -20)]:
		var b := Convert.angles_from_basis(Convert.basis_from_angles(a))
		check(near(b, a, 1e-2), "angles round trip %s -> %s" % [a, b])
	# An unrotated entity node faces Godot -Z, which is Bulwark yaw 90.
	var frame := Convert.to_quake(Transform3D.IDENTITY) * Convert.FACING
	check(near(Convert.angles_from_basis(frame.basis), Vector3(0, 90, 0)), "entity facing matches Stiletto")
	var s := Convert.transform_to_strings(Convert.transform_from_strings("1,2,3", "0,0,0.7071068,0.7071068", "2,2,2"))
	check(s.Position == "1,2,3" and s.Scale == "2,2,2" and s.Rotation == "0,0,0.707107,0.707107", "GameObject strings round trip: %s" % s)


func test_defs() -> void:
	var d := Defs.new()
	d.parse_quaked("""/*QUAKED func_door (0 .5 .8) ? SF_MOV_OPEN x SF_MOV_UNLINK
# OVERVIEW
A sliding door.

# KEYS
- "targetname" : Name
- "speed" : Movement speed in game-units per second.
- "snd_open" : Sound shader to play for when the door opens.

# SPAWNFLAGS
- SF_MOV_OPEN (1) : Swaps the positions.
- SF_MOV_USE (256): Players can use it.

# INPUTS
- "Open" : Opens it.

@ingroup brushentity
*/
/*QUAKED info_player_start (1 0 0) (-16 -16 -36) (16 16 36) GOD
# OVERVIEW
Player spawn.
*/""", "test.def")
	var door := d.get_class_def("func_door")
	check(door.kind == "solid" and door.description == "A sliding door.", "QUAKED brush class")
	check(door.keys.size() == 3 and d.find_key(door, "snd_open").type == "sound" and d.find_key(door, "targetname").type == "target_source", "QUAKED keys and guessed types")
	var bits := []
	for f in door.flags:
		bits.append([f.name, f.bit])
	check(bits == [["SF_MOV_OPEN", 1], ["SF_MOV_UNLINK", 4], ["SF_MOV_USE", 256]], "QUAKED spawnflags %s" % [bits])
	check(door.inputs.size() == 1 and door.inputs[0].name == "Open", "QUAKED inputs")
	var start := d.get_class_def("info_player_start")
	check(start.kind == "point" and start.mins == Vector3(-16, -16, -36) and start.maxs == Vector3(16, 16, 36) and start.color == Color(1, 0, 0), "QUAKED point bounds and colour")
	d.parse_fgd("""// comment
@BaseClass = Targetname [ targetname(target_source) : "Name" : : "Its name" ]
@BaseClass color(204 204 204) size(-8 -8 -8, 8 8 8) = Phong
[
	_phong(choices) : "Phong shading" : 0 =
	[
		0 : "Disabled"
		1 : "Smooth shading"
	]
]
@PointClass base(Targetname, Phong) color(255 128 64) studio("models/x.mdl") = misc_model : "Arbitrary" +
	" model"
[
	model(studio) : "Model path"
	modelscale(float) : "Uniform Scale" : "1.0" : "Scale"
	spawnflags(flags) = [ 1 : "Start off" : 1  2 : "Solid" : 0 ]
	input Enable(void) : "Turns it on"
	output OnUse(void) : "Used"
]""", "test.fgd")
	var mm := d.get_class_def("misc_model")
	check(not mm.is_empty() and mm.description == "Arbitrary model" and mm.model == "models/x.mdl" and mm.color == Color8(255, 128, 64), "FGD class header")
	var names := []
	for k in mm.keys:
		names.append(k.name)
	check(names == ["targetname", "_phong", "model", "modelscale"], "FGD keys with bases %s" % [names])
	check(d.find_key(mm, "_phong").choices == [["0", "Disabled"], ["1", "Smooth shading"]], "FGD choices")
	check(d.find_key(mm, "modelscale").default == "1.0" and d.find_key(mm, "modelscale").description == "Scale", "FGD default and description")
	check(mm.flags.size() == 2 and mm.flags[0].default and mm.flags[1].bit == 2, "FGD flags")
	check(mm.inputs.size() == 1 and mm.outputs.size() == 1 and mm.outputs[0].name == "OnUse", "FGD inputs and outputs")
	check(mm.mins == Vector3(-8, -8, -8), "FGD size from base")
	d.parse_entitydefs("""entityDef item_suit
{
	"spawnclass"	"ncItem"
	"model"			"models/w_suit.mdl"
	"mins"			"-16 -16 0"
	"maxs"			"16 16 16"
}""", "items.def")
	var suit := d.get_class_def("item_suit")
	check(suit.model == "models/w_suit.mdl" and suit.maxs == Vector3(16, 16, 16) and suit.group == "decl", "entityDef decls")
	var env := OS.get_environment("BULWARK_DEFS")
	if not env.is_empty():
		var real := Defs.new()
		var n := real.load_path(env)
		var door2 := real.get_class_def("func_door")
		print("  %s: %d classes" % [env.get_file(), n])
		check(n > 50 and not door2.is_empty() and door2.kind == "solid" and door2.flags.size() >= 4, "real definitions load")


func test_scene_roundtrip() -> void:
	ProjectSettings.set_setting("bulwark/game_directory", repo.path_join("tests/scene/data"))
	var io := SceneIO.new()
	var root := io.import_file(repo.path_join("tests/scene/data/maps/test.scene"))
	check(root != null and io.errors.is_empty(), "import test.scene: %s" % [io.errors])
	if root == null:
		return
	check(root.world == "testmap" and root.title == "Bulwark test scene", "MapInstance and title become root settings")
	var door: Node3D = root.get_node_or_null("door")
	check(door is EntityScript and door.classname == "func_door", "door imported as an entity")
	if door == null:
		return
	check(door.properties.speed == "100" and door.properties.locked == "0" and door.properties.message == "say \"hi\"", "door keys as text %s" % door.properties)
	# Yaw 90 in the file is an unrotated node facing -Z.
	check(near(Convert.angles_from_basis(Convert.to_quake(glob(door)).basis * Convert.FACING.basis), Vector3(0, 90, 0)), "door faces yaw 90")
	check(near(Convert.point_to_quake(glob(door).origin), Vector3(0, 0, 64)), "door origin")
	var button: Node3D = door.get_node_or_null("button")
	check(button is EntityScript and button.properties.target == "@dddddddd-0000-4000-8000-000000000001", "component reference imported as @guid")
	check(near(Convert.point_to_quake(glob(button).origin), Vector3(0, 32, 64)), "child position goes through the parent's rotation")
	var hidden: Node3D = root.get_node_or_null("hidden")
	check(hidden is EntityScript and not hidden.enabled, "disabled object stays disabled")
	var barrel: Node3D = root.get_node_or_null("barrel")
	check(barrel != null and not barrel is EntityScript and barrel.get_meta(SceneIO.M_COMPONENTS).size() == 2, "s&box components kept on a plain object")
	var crate: Node3D = root.get_child(0)
	check(crate is PrefabScript and crate.prefab == "prefabs/crate.prefab", "prefab instance imported")
	check(near(Convert.point_to_quake(glob(crate).origin), Vector3(100, 0, 0)), "prefab instance position from its override")
	# Export and compare with the original, JSON value by JSON value.
	var out := io.export_tree(root)
	check(io.errors.is_empty(), "export: %s" % [io.errors])
	var orig = SceneIO.read_json(repo.path_join("tests/scene/data/maps/test.scene"))
	check(out.__guid == orig.__guid and out.GameObjects.size() == orig.GameObjects.size(), "same scene GUID and object count")
	var odoor := _find(orig.GameObjects, "dddddddd-0000-4000-8000-000000000001")
	var edoor := _find(out.GameObjects, "dddddddd-0000-4000-8000-000000000001")
	check(not edoor.is_empty() and edoor.Position == "0,0,64" and edoor.Rotation == "0,0,0.707107,0.707107", "door transform written back %s %s" % [edoor.get("Position"), edoor.get("Rotation")])
	var ec: Dictionary = edoor.Components[0]
	check(ec.__guid == odoor.Components[0].__guid and ec.speed is int and ec.speed == 100 and ec.locked is bool and ec.locked == false, "spawn keys keep their JSON types")
	var ebutton: Dictionary = edoor.Children[0]
	check(ebutton.Position == "32,0,0", "child local position %s" % ebutton.Position)
	check(ebutton.Components[0].target is Dictionary and ebutton.Components[0].target.go == "dddddddd-0000-4000-8000-000000000001", "@guid written as a reference")
	var ecrate := _find(out.GameObjects, "aaaaaaaa-0000-4000-8000-000000000001")
	var pos_override := ""
	var model_override := ""
	for o in ecrate.__PrefabInstancePatch.PropertyOverrides:
		if o.Property == "Position":
			pos_override = o.Value
		if o.Property == "model":
			model_override = o.Value
	check(pos_override == "100,0,0" and model_override == "models/crate_big.iqm" and ecrate.__PrefabInstancePatch.AddedObjects.size() == 1, "prefab patch kept, position rewritten")
	var ebarrel := _find(out.GameObjects, "ffffffff-0000-4000-8000-000000000001")
	check(ebarrel.Scale == "2,2,2" and ebarrel.Components.size() == 2, "plain object keeps its scale and components")
	var map := out.GameObjects[0] as Dictionary
	check(map.Components[0].__type == "Sandbox.MapInstance" and map.Components[0].MapName == "testmap" and map.__guid == orig.GameObjects[0].__guid, "MapInstance object written back")
	_write(out_dir.path_join("roundtrip.scene"), SceneIO.to_json(out))
	# Moving a node in Godot moves the entity.
	door.position += Vector3(1, 0, 0)
	var moved := _find(io.export_tree(root).GameObjects, "dddddddd-0000-4000-8000-000000000001")
	check(moved.Position == "32,0,64", "moving the node 1 m along Godot X moves the entity 32 units along X: %s" % moved.Position)
	root.free()


func test_build_and_export() -> void:
	# A scene made in the editor: entities under a group, a target link and
	# an Absolute-free hierarchy.
	var root: Node3D = SceneScript.new()
	root.name = "Built"
	root.world = "editor_lab.ftew"
	root.preview_world = false
	var group := Node3D.new()
	group.name = "Room"
	group.position = Vector3(2, 0, -1)
	group.rotation = Vector3(0, PI / 2, 0)
	root.add_child(group)
	group.owner = root
	var light: Node3D = EntityScript.new()
	light.name = "light"
	light.classname = "light"
	var lp: Dictionary[String, String] = {"_light": "255 200 100 300", "targetname": "lamp"}
	light.properties = lp
	light.position = Vector3(0, 1, 0)
	group.add_child(light)
	light.owner = root
	var relay: Node3D = EntityScript.new()
	relay.name = "relay"
	relay.classname = "trigger_relay"
	var rp: Dictionary[String, String] = {"target": "lamp"}
	relay.properties = rp
	relay.rotation = Vector3(0, deg_to_rad(30), 0)
	root.add_child(relay)
	relay.owner = root
	var stray := MeshInstance3D.new() # geometry: not a GameObject
	root.add_child(stray)
	stray.owner = root
	var io := SceneIO.new()
	var text := io.export_text(root)
	var path := out_dir.path_join("built.scene")
	_write(path, text)
	var data = JSON.parse_string(text)
	check(data.GameObjects.size() == 3, "map object, group and relay at the top level (%d)" % data.GameObjects.size())
	check(data.GameObjects[0].Components[0].MapName == "editor_lab.ftew", "MapInstance from the root's world")
	var room := data.GameObjects[1] as Dictionary
	check(room.Name == "Room" and room.Components.is_empty() and room.Children.size() == 1, "group becomes a plain GameObject")
	check(SceneIO.is_guid(light.get_meta(SceneIO.M_GUID, "")), "export gives nodes GUIDs")
	var text2 := io.export_text(root)
	check(text == text2, "exporting twice gives the same file")
	# Duplicated nodes (Ctrl+D copies metadata) get their own GUIDs.
	var copy: Node3D = relay.duplicate()
	root.add_child(copy)
	copy.owner = root
	var data3 = JSON.parse_string(io.export_text(root))
	check(data3.GameObjects[2].__guid != data3.GameObjects[3].__guid, "a duplicated node gets a new GUID")
	root.free()


func test_prefab_file() -> void:
	ProjectSettings.set_setting("bulwark/game_directory", repo.path_join("tests/scene/data"))
	var io := SceneIO.new()
	var root := io.import_file(repo.path_join("tests/scene/data/prefabs/crate.prefab"))
	check(root != null and root.kind == 1 and root.classname == "prop_physics" and root.properties.target == "@11111111-0000-4000-8000-000000000003", "prefab root entity")
	var out := io.export_tree(root)
	check(out.RootObject.__guid == "11111111-0000-4000-8000-000000000001" and out.RootObject.Components[0].classname == "prop_physics", "prefab RootObject written back")
	check(out.RootObject.Components[0].target is Dictionary and out.RootObject.Children.size() == 2, "prefab children and references")
	check(out.RootObject.Children[1].Components[0].__type == "Sandbox.Decal", "unknown component kept")
	_write(out_dir.path_join("crate.prefab"), SceneIO.to_json(out))
	var preview := PrefabScript.build_preview("prefabs/crate.prefab")
	check(preview != null and preview.get_child_count() == 3, "prefab preview holds the root entity and its children")
	if preview != null:
		var holder := Node3D.new()
		holder.add_child(preview)
		var lid: Node3D = preview.get_node("lid")
		check(near(Convert.point_to_quake(glob(lid).origin), Vector3(0, 0, 16)), "preview children sit where the prefab puts them")
		holder.free()
	root.free()


func test_world_preview() -> void:
	ProjectSettings.set_setting("bulwark/game_directory", repo.path_join("tests/ftew"))
	var w := WorldPreview.build("editor_lab.ftew")
	check(w != null, "FTEW preview: " + WorldPreview.last_error())
	if w != null:
		var mi: MeshInstance3D = w.get_child(0)
		var tris := 0
		for s in mi.mesh.get_surface_count():
			tris += mi.mesh.surface_get_arrays(s)[Mesh.ARRAY_VERTEX].size() / 3
		print("  editor_lab.ftew: %d surfaces, %d triangles" % [mi.mesh.get_surface_count(), tris])
		check(mi.mesh.get_surface_count() > 0 and tris > 10, "FTEW preview has geometry")
		var aabb := mi.mesh.get_aabb()
		check(aabb.size.length() > 2 and aabb.size.length() < 1000, "FTEW preview is in metres: %s" % aabb)
		w.free()
	var bsp := OS.get_environment("BULWARK_BSP")
	if not bsp.is_empty():
		ProjectSettings.set_setting("bulwark/game_directory", bsp.get_base_dir().get_base_dir())
		var b := WorldPreview.build(bsp.get_file())
		check(b != null, "BSP preview: " + WorldPreview.last_error())
		if b != null:
			var mi: MeshInstance3D = b.get_child(0)
			print("  %s: %d surfaces, aabb %s" % [bsp.get_file(), mi.mesh.get_surface_count(), mi.mesh.get_aabb()])
			check(mi.mesh.get_surface_count() > 0, "BSP preview has geometry")
			b.free()


static func _find(list: Array, guid: String) -> Dictionary:
	for go in list:
		if go.get("__guid", "") == guid:
			return go
		var c := _find(go.get("Children", []), guid)
		if not c.is_empty():
			return c
	return {}


static func _write(path: String, text: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(text)
	f.close()


## A node's global transform without a running tree (tests run in _init).
static func glob(n: Node) -> Transform3D:
	var t := Transform3D.IDENTITY
	while n != null:
		if n is Node3D:
			t = n.transform * t
		n = n.get_parent()
	return t
