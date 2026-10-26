@tool
extends RefCounted
## Where Bulwark content lives on disk, from the project settings.

const GAME_DIR := "bulwark/game_directory"
const ENGINE := "bulwark/engine_executable"
const BASE_DIR := "bulwark/base_directory"
const GAMES := "bulwark/games"
const DEFS := "bulwark/entity_definitions"
const LAUNCH_ARGS := "bulwark/launch_arguments"
const UNITS := "bulwark/units_per_meter"


## Adds the project settings with their defaults, so they show in Project
## Settings (with "Advanced Settings" on) under Bulwark.
static func register_settings() -> void:
	_setting(GAME_DIR, "res://", TYPE_STRING, PROPERTY_HINT_GLOBAL_DIR, "")
	_setting(ENGINE, "", TYPE_STRING, PROPERTY_HINT_GLOBAL_FILE, "")
	_setting(BASE_DIR, "", TYPE_STRING, PROPERTY_HINT_GLOBAL_DIR, "")
	_setting(GAMES, PackedStringArray(["base"]), TYPE_PACKED_STRING_ARRAY, PROPERTY_HINT_NONE, "")
	_setting(DEFS, PackedStringArray(), TYPE_PACKED_STRING_ARRAY, PROPERTY_HINT_NONE, "")
	_setting(LAUNCH_ARGS, "-window +set developer 1", TYPE_STRING, PROPERTY_HINT_NONE, "")
	_setting(UNITS, 32.0, TYPE_FLOAT, PROPERTY_HINT_RANGE, "1,1024,0.5")


static func _setting(name: String, value: Variant, type: int, hint: int, hint_string: String) -> void:
	if not ProjectSettings.has_setting(name):
		ProjectSettings.set_setting(name, value)
	ProjectSettings.set_initial_value(name, value)
	ProjectSettings.add_property_info({"name": name, "type": type, "hint": hint, "hint_string": hint_string})


static func get_setting(name: String, fallback: Variant = "") -> Variant:
	return ProjectSettings.get_setting(name, fallback)


static func absolute(path: String) -> String:
	if path.begins_with("res://") or path.begins_with("user://"):
		return ProjectSettings.globalize_path(path)
	if path.is_relative_path():
		return ProjectSettings.globalize_path("res://").path_join(path).simplify_path()
	return path


## The folder that holds maps/ and prefabs/: Bulwark's game directory.
static func game_dir() -> String:
	return absolute(str(get_setting(GAME_DIR, "res://"))).trim_suffix("/")


## A game-relative path such as "prefabs/crate.prefab" as an absolute path.
static func game_file(rel: String) -> String:
	if rel.is_absolute_path() or rel.begins_with("res://"):
		return absolute(rel)
	return game_dir().path_join(rel)


## An absolute path as game-relative, or "" when it is outside the game dir.
static func game_relative(path: String) -> String:
	var abs_path := absolute(path)
	var dir := game_dir() + "/"
	return abs_path.substr(dir.length()) if abs_path.begins_with(dir) else ""


## A MapInstance MapName as a file: "ai" is maps/ai.bsp, "lab.ftew" maps/lab.ftew.
static func map_file(map_name: String) -> String:
	var rel := map_name
	if not rel.contains("/"):
		rel = "maps/" + rel
	if rel.get_extension().is_empty():
		rel += ".bsp"
	return game_file(rel)


## Where entity definitions are read from: the setting's list, or else any
## Tools/base_entities.def, *.fgd and decls/def found next to the game.
static func definition_paths() -> PackedStringArray:
	var listed: PackedStringArray = get_setting(DEFS, PackedStringArray())
	if not listed.is_empty():
		var out: PackedStringArray = []
		for p in listed:
			out.append(absolute(p))
		return out
	var found: PackedStringArray = []
	var game := game_dir()
	for candidate in [game.path_join("../Tools/base_entities.def"), game.path_join("Tools/base_entities.def"),
			game.path_join("decls/def"), game.path_join("entities.def")]:
		candidate = candidate.simplify_path()
		if FileAccess.file_exists(candidate) or DirAccess.dir_exists_absolute(candidate):
			found.append(candidate)
	if DirAccess.dir_exists_absolute(game):
		for f in DirAccess.get_files_at(game):
			if f.get_extension().to_lower() == "fgd":
				found.append(game.path_join(f))
	return found
