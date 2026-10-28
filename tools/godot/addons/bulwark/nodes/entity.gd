@tool
class_name BulwarkEntity3D
extends Node3D
## A Nuclide entity in a Bulwark scene: a GameObject with a Bulwark.Entity
## component. The node's -Z axis is the way the entity faces. Spawn keys go in
## `properties`; origin and angles come from the node's transform.

signal definition_changed

@export var classname: String = "info_notnull":
	set(value):
		if value == classname:
			return
		classname = value
		update_gizmos()
		definition_changed.emit()

## Spawn keys and their values. Use "@<guid>" to point at another object by
## its GUID, the way s&box references are imported.
@export var properties: Dictionary[String, String] = {}:
	set(value):
		properties = value
		update_gizmos()

## Unchecked: the entity and everything under it are left out of the game.
@export var enabled: bool = true

## Kept in the scene file for the editor, never spawned.
@export var editor_only: bool = false


func _init() -> void:
	# Stiletto's FTEW exporter skips nodes with this flag, so Bulwark entities
	# can sit inside a Stiletto world without being exported twice.
	set_meta(&"fte_ignore", true)


func get_key(key: String, fallback: String = "") -> String:
	return properties.get(key, fallback)


func set_key(key: String, value: String) -> void:
	var p := properties.duplicate()
	if value.is_empty():
		p.erase(key)
	else:
		p[key] = value
	properties = p


func targetname() -> String:
	return get_key("targetname")
