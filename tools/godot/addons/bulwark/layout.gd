@tool
extends RefCounted
## Editor layout for map work: Hammer's four views, snapping in Quake units,
## and an editor feature profile without the parts of Godot a map author
## doesn't use. The view and snap changes need EditorInterface's 3D editor
## state calls, which Bulwark's Godot build adds (sokkycakes/godot); on a
## stock Godot they report that and do nothing.

const Convert := preload("convert.gd")

const PROFILE := "Bulwark"

## Grid sizes offered in the toolbar, in Quake units.
const GRID_SIZES := [1, 2, 4, 8, 16, 32, 64, 128, 256]


static func can_set_state() -> bool:
	return EditorInterface.has_method("set_node_3d_editor_state")


static func set_state(state: Dictionary) -> bool:
	if not can_set_state():
		return false
	EditorInterface.call("set_node_3d_editor_state", state)
	return true


static func get_state() -> Dictionary:
	return EditorInterface.call("get_node_3d_editor_state") if EditorInterface.has_method("get_node_3d_editor_state") else {}


## Turns snapping on with the grid size in Quake units.
static func set_grid(units: int) -> bool:
	return set_state({"snap_enabled": true, "translate_snap": float(units) / Convert.units(), "rotate_snap": 15.0, "scale_snap": 10.0})


## Top, front and side views in orthographic, and a perspective camera, as
## in Hammer. Godot's top view looks down Bulwark's -Z, its front view along
## Bulwark's +Y and its left view along Bulwark's -X.
static func hammer_layout() -> bool:
	var views := [
		{"orthogonal": false, "view_type": 0, "x_rotation": 0.5, "y_rotation": -0.6},
		{"orthogonal": true, "view_type": 1, "x_rotation": PI / 2, "y_rotation": 0.0},
		{"orthogonal": true, "view_type": 5, "x_rotation": 0.0, "y_rotation": 0.0},
		{"orthogonal": true, "view_type": 3, "x_rotation": 0.0, "y_rotation": PI / 2},
	]
	return set_state({"viewport_mode": 4, "viewports": views, "show_grid": true})


static func single_view() -> bool:
	return set_state({"viewport_mode": 1})


## Points every 3D view at a position (Godot space), keeping each view's
## angle, at a distance that shows about `radius` metres around it.
static func look_at(position: Vector3, radius: float = 4.0) -> bool:
	var state := get_state()
	var views: Array = state.get("viewports", [])
	if views.is_empty():
		return false
	var out: Array = []
	for v in views:
		out.append({"position": position, "distance": maxf(radius * 2.5, 1.0)})
	return set_state({"viewports": out})


## Writes and selects the Bulwark editor profile: no Asset Library or Game
## view, and no 2D or UI nodes in the Create Node dialog.
static func install_profile() -> String:
	var p := EditorFeatureProfile.new()
	p.set_disable_feature(EditorFeatureProfile.FEATURE_ASSET_LIB, true)
	p.set_disable_feature(EditorFeatureProfile.FEATURE_GAME, true)
	for cls in ["Node2D", "Control", "CanvasLayer", "CanvasModulate", "ParallaxBackground", "Parallax2D"]:
		p.set_disable_class(cls, true)
	var dir := EditorInterface.get_editor_paths().get_config_dir().path_join("feature_profiles")
	DirAccess.make_dir_recursive_absolute(dir)
	var path := dir.path_join(PROFILE + ".profile")
	var err := p.save_to_file(path)
	if err != OK:
		return "Could not write %s: %s" % [path, error_string(err)]
	EditorInterface.set_current_feature_profile(PROFILE)
	return ""


static func remove_profile() -> void:
	EditorInterface.set_current_feature_profile("")
