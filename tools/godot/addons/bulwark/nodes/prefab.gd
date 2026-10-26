@tool
class_name BulwarkPrefab3D
extends Node3D
## An instance of a .prefab. The editor shows the prefab's objects under it
## as a preview; they are not saved here. Edits made to an instance in s&box
## (its patch) are kept and written back unchanged.

const Convert := preload("../convert.gd")
const Paths := preload("../paths.gd")

## Game-relative path, e.g. prefabs/crate.prefab.
@export var prefab: String = "":
	set(value):
		prefab = value
		refresh_preview()

var _preview: Node3D


func _init() -> void:
	set_meta(&"fte_ignore", true)


func _ready() -> void:
	refresh_preview()


func refresh_preview() -> void:
	if not is_inside_tree() or not Engine.is_editor_hint():
		return
	if is_instance_valid(_preview):
		_preview.queue_free()
		_preview = null
	if prefab.is_empty():
		return
	_preview = build_preview(prefab)
	if _preview != null:
		add_child(_preview, false, Node.INTERNAL_MODE_FRONT)


## The prefab's objects, placed as they sit under an instance node. The
## instance's own transform stands for the prefab root's.
static func build_preview(path: String) -> Node3D:
	var file := Paths.game_file(path)
	if not FileAccess.file_exists(file):
		return null
	var io = load("res://addons/bulwark/scene_io.gd").new()
	var tree: Node3D = io.import_file(file)
	if tree == null:
		return null
	var holder := Node3D.new()
	holder.name = "PrefabPreview"
	# An instance node's Bulwark frame is the prefab root's turned back by the
	# entity facing, so the root's frame is the node's frame times FACING.
	holder.transform = Convert.from_quake(Convert.FACING)
	if not tree.classname.is_empty():
		var marker: Node3D = load("res://addons/bulwark/nodes/entity.gd").new()
		marker.classname = tree.classname
		marker.properties = tree.properties
		marker.name = tree.name
		marker.transform = Convert.from_quake(Convert.FACING.affine_inverse())
		holder.add_child(marker)
	for child in tree.get_children():
		tree.remove_child(child)
		_clear_owner(child)
		holder.add_child(child)
	tree.free()
	return holder


static func _clear_owner(n: Node) -> void:
	n.owner = null
	for c in n.get_children():
		_clear_owner(c)
