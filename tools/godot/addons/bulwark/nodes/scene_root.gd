@tool
class_name BulwarkScene3D
extends Node3D
## The root of a Bulwark .scene or .prefab. Its children are the GameObjects.
## Godot meshes and CSG under it are previews or Stiletto world geometry and
## are not written to the scene file.

const WorldPreview := preload("../world_preview.gd")

@export_enum("Scene", "Prefab") var kind: int = 0

## File name without extension: maps/<name>.scene or prefabs/<name>.prefab.
## Empty uses the Godot scene's file name.
@export var scene_name: String = ""

@export var title: String = ""

@export_group("World")
## The BSP or FTEW world, as a MapInstance MapName: "ai" for maps/ai.bsp,
## "lab.ftew" for maps/lab.ftew. Empty for a prefab.
@export var world: String = "":
	set(value):
		world = value
		refresh_preview()

## Spawn the world's own entities too. Off for scenes that hold the map's
## entities themselves, as `bwscene frombsp` writes them.
@export var spawn_map_entities: bool = true

@export var world_collision: bool = true

## Show the world in the editor. The preview is never saved or exported.
@export var preview_world: bool = true:
	set(value):
		preview_world = value
		refresh_preview()

@export_group("Prefab root entity")
## For a prefab whose root object is an entity. Leave empty otherwise.
@export var classname: String = ""
@export var properties: Dictionary[String, String] = {}

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
	if not preview_world or world.is_empty():
		return
	_preview = WorldPreview.build(world)
	if _preview != null:
		add_child(_preview, false, Node.INTERNAL_MODE_FRONT)
