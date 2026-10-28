@tool
extends EditorNode3DGizmoPlugin
## Draws entities the way Hammer does: a box the size of the class's bounds
## in its editor colour, an arrow for the way it faces, and a line to each
## entity it targets.

const Convert := preload("convert.gd")
const EntityScript := preload("nodes/entity.gd")
const PrefabScript := preload("nodes/prefab.gd")

## Keys whose values name other entities.
const LINK_KEYS := ["target", "killtarget", "parentname", "netname", "message", "target_destination", "path"]

var defs # defs.gd, shared with the plugin
var _made := {}


func _init(definitions = null) -> void:
	defs = definitions
	create_material("unknown", Color(1, 0, 1))
	create_material("prefab", Color(0.4, 0.8, 1))
	create_material("link", Color(1, 0.65, 0.2))
	_made = {"unknown": true, "prefab": true, "link": true}


func _get_gizmo_name() -> String:
	return "Bulwark entities"


func _has_gizmo(node: Node3D) -> bool:
	return node is EntityScript or node is PrefabScript


func _material(color: Color, gizmo: EditorNode3DGizmo) -> StandardMaterial3D:
	var key := "c" + color.to_html(false)
	if not _made.has(key):
		create_material(key, color)
		_made[key] = true
	return get_material(key, gizmo)


func _redraw(gizmo: EditorNode3DGizmo) -> void:
	gizmo.clear()
	var node := gizmo.get_node_3d()
	if node is PrefabScript:
		var s := 0.25
		var lines := PackedVector3Array([Vector3(-s, 0, 0), Vector3(s, 0, 0), Vector3(0, -s, 0), Vector3(0, s, 0), Vector3(0, 0, -s), Vector3(0, 0, s)])
		gizmo.add_lines(lines, get_material("prefab", gizmo))
		gizmo.add_collision_segments(lines)
		return
	var def: Dictionary = defs.get_class_def(node.classname) if defs != null else {}
	var mat := get_material("unknown", gizmo) if def.is_empty() else _material(def.color, gizmo)
	var mins: Vector3 = def.get("mins", Vector3(-8, -8, -8))
	var maxs: Vector3 = def.get("maxs", Vector3(8, 8, 8))
	if def.get("kind", "point") == "solid":
		mins = Vector3(-8, -8, -8)
		maxs = Vector3(8, 8, 8)
	var lines := box_lines(mins, maxs)
	# The facing arrow, from the centre to past the box's front.
	var front := maxf(maxs.x, 8.0) + 8.0
	var c := (mins + maxs) / 2
	for seg in [[c, Vector3(front, c.y, c.z)], [Vector3(front, c.y, c.z), Vector3(front - 6, c.y + 4, c.z)], [Vector3(front, c.y, c.z), Vector3(front - 6, c.y - 4, c.z)]]:
		lines.append(to_local(seg[0]))
		lines.append(to_local(seg[1]))
	gizmo.add_lines(lines, mat)
	gizmo.add_collision_segments(lines)
	var links := PackedVector3Array()
	for target in linked_nodes(node):
		links.append(Vector3.ZERO)
		links.append(node.to_local(target.global_position))
	if not links.is_empty():
		gizmo.add_lines(links, get_material("link", gizmo))


## A point in an entity's own Bulwark frame as a point in its node's space.
static func to_local(p: Vector3) -> Vector3:
	return Convert.point_from_quake(Convert.FACING * p)


static func box_lines(mins: Vector3, maxs: Vector3) -> PackedVector3Array:
	var corners: Array = []
	for i in 8:
		corners.append(to_local(Vector3(maxs.x if i & 1 else mins.x, maxs.y if i & 2 else mins.y, maxs.z if i & 4 else mins.z)))
	var out := PackedVector3Array()
	for e in [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7], [0, 4], [1, 5], [2, 6], [3, 7]]:
		out.append(corners[e[0]])
		out.append(corners[e[1]])
	return out


## The entities in the edited scene that this one's keys point at.
static func linked_nodes(node: Node) -> Array:
	var root := node.get_tree().edited_scene_root if node.is_inside_tree() else null
	if root == null:
		return []
	var names := {}
	for k in node.properties:
		var v: String = node.properties[k]
		if v.is_empty():
			continue
		if k in LINK_KEYS or (k.begins_with("On") and v.contains(",")):
			names[v.get_slice(",", 0).strip_edges()] = true
	if names.is_empty():
		return []
	var out: Array = []
	_find(root, names, node, out)
	return out


static func _find(n: Node, names: Dictionary, skip: Node, out: Array) -> void:
	if n != skip and n is EntityScript:
		var tn: String = n.properties.get("targetname", "")
		if (not tn.is_empty() and names.has(tn)) or names.has("@" + str(n.get_meta(&"bulwark_guid", ""))):
			out.append(n)
	for c in n.get_children():
		_find(c, names, skip, out)
