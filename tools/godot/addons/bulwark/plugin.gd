@tool
extends EditorPlugin
## Bulwark editor tools. See tools/godot/README.md.

const Paths := preload("paths.gd")
const Convert := preload("convert.gd")
const Defs := preload("defs.gd")
const SceneIO := preload("scene_io.gd")
const Layout := preload("layout.gd")
const WorldPreview := preload("world_preview.gd")
const Gizmo := preload("gizmo.gd")
const Inspector := preload("inspector.gd")
const Browser := preload("browser.gd")
const EntityScript := preload("nodes/entity.gd")
const SceneScript := preload("nodes/scene_root.gd")

const STILETTO_EXPORTER := "res://addons/stiletto_tools/exporter.gd"

enum { MENU_NEW, MENU_IMPORT, MENU_EXPORT, MENU_PLAY, MENU_FIND, MENU_LAYOUT, MENU_SINGLE, MENU_PROFILE, MENU_PROFILE_OFF, MENU_RELOAD, MENU_SETTINGS }

var defs := Defs.new()
var gizmo: Gizmo
var inspector: Inspector
var browser: Browser
var menu: PopupMenu
var toolbar: HBoxContainer
var grid_button: OptionButton
var readout: Label
var finder: ConfirmationDialog

var grid_units := 8
var placing := ""
var _hit_cache := {}


func _enter_tree() -> void:
	Paths.register_settings()
	reload_definitions()
	gizmo = Gizmo.new(defs)
	add_node_3d_gizmo_plugin(gizmo)
	inspector = Inspector.new()
	inspector.plugin = self
	inspector.defs = defs
	inspector.pick_class = func(cb: Callable): browser.pick(cb); make_visible_dock()
	add_inspector_plugin(inspector)
	browser = Browser.new()
	browser.defs = defs
	browser.place_requested.connect(start_placing)
	add_control_to_dock(DOCK_SLOT_LEFT_BR, browser)
	browser.refresh()
	_build_toolbar()
	_build_menu()
	set_input_event_forwarding_always_enabled()
	scene_changed.connect(_on_scene_changed)
	set_process(true)


func _exit_tree() -> void:
	remove_node_3d_gizmo_plugin(gizmo)
	remove_inspector_plugin(inspector)
	remove_control_from_docks(browser)
	browser.queue_free()
	remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, toolbar)
	toolbar.queue_free()
	remove_tool_menu_item("Bulwark")
	if is_instance_valid(finder):
		finder.queue_free()


func make_visible_dock() -> void:
	var parent := browser.get_parent()
	if parent is TabContainer:
		parent.current_tab = browser.get_index()


func reload_definitions() -> void:
	defs.clear()
	for p in Paths.definition_paths():
		defs.load_path(p)
	for e in defs.errors:
		push_warning("Bulwark: " + e)
	if is_instance_valid(browser):
		browser.refresh()
		browser.set_status("%d entity classes loaded." % defs.classes.size())
	if is_instance_valid(gizmo):
		update_overlays()


# --- UI -----------------------------------------------------------------------

func _build_toolbar() -> void:
	toolbar = HBoxContainer.new()
	toolbar.add_child(VSeparator.new())
	var grid_label := Label.new()
	grid_label.text = "Grid"
	toolbar.add_child(grid_label)
	grid_button = OptionButton.new()
	grid_button.tooltip_text = "Snap grid in Bulwark units. [ and ] halve and double it, as in Hammer."
	for g in Layout.GRID_SIZES:
		grid_button.add_item(str(g))
	grid_button.select(Layout.GRID_SIZES.find(grid_units))
	grid_button.item_selected.connect(func(i): set_grid(Layout.GRID_SIZES[i]))
	toolbar.add_child(grid_button)
	var place := Button.new()
	place.text = "Entity"
	place.tooltip_text = "Place the class selected in the Entities dock (Shift+E)"
	place.pressed.connect(func(): start_placing(browser.selected_class()))
	toolbar.add_child(place)
	var play := Button.new()
	play.text = "Run map"
	play.tooltip_text = "Export and play in Bulwark (F9)"
	play.pressed.connect(play_current)
	toolbar.add_child(play)
	readout = Label.new()
	readout.modulate = Color(1, 1, 1, 0.75)
	toolbar.add_child(readout)
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, toolbar)


func _build_menu() -> void:
	menu = PopupMenu.new()
	menu.add_item("New Bulwark Scene…", MENU_NEW)
	menu.add_item("Import .scene / .prefab…", MENU_IMPORT)
	menu.add_separator()
	menu.add_item("Export Scene", MENU_EXPORT)
	menu.add_item("Export and Run Map (F9)", MENU_PLAY)
	menu.add_item("Find Entity… (Ctrl+Shift+F)", MENU_FIND)
	menu.add_separator()
	menu.add_item("Hammer Layout (four views)", MENU_LAYOUT)
	menu.add_item("Single View", MENU_SINGLE)
	menu.add_item("Use Bulwark Editor Profile", MENU_PROFILE)
	menu.add_item("Use Default Editor Profile", MENU_PROFILE_OFF)
	menu.add_separator()
	menu.add_item("Reload Entity Definitions", MENU_RELOAD)
	menu.add_item("Bulwark Settings…", MENU_SETTINGS)
	menu.id_pressed.connect(_on_menu)
	add_tool_submenu_item("Bulwark", menu)


func _on_menu(id: int) -> void:
	match id:
		MENU_NEW: new_scene()
		MENU_IMPORT: import_dialog()
		MENU_EXPORT: export_current()
		MENU_PLAY: play_current()
		MENU_FIND: find_entity()
		MENU_LAYOUT: _layout_result(Layout.hammer_layout())
		MENU_SINGLE: _layout_result(Layout.single_view())
		MENU_PROFILE:
			var err := Layout.install_profile()
			notify(err if not err.is_empty() else "Bulwark editor profile on. Editor > Manage Editor Features switches it off.")
		MENU_PROFILE_OFF: Layout.remove_profile()
		MENU_RELOAD: reload_definitions()
		MENU_SETTINGS:
			notify("Bulwark's settings are in Project > Project Settings under Bulwark (turn on Advanced Settings).")


func _layout_result(ok: bool) -> void:
	if not ok:
		notify("This Godot build can't change the 3D views from a plugin. Use Bulwark's Godot build (sokkycakes/godot), or set them from the View menu.")


func notify(text: String, error: bool = false) -> void:
	print("Bulwark: " + text)
	if is_instance_valid(browser):
		browser.set_status(text)
	var toaster := EditorInterface.get_editor_toaster()
	toaster.push_toast(text, EditorToaster.SEVERITY_ERROR if error else EditorToaster.SEVERITY_INFO)


func set_grid(units: int) -> void:
	grid_units = clampi(units, Layout.GRID_SIZES[0], Layout.GRID_SIZES[-1])
	grid_button.select(Layout.GRID_SIZES.find(grid_units))
	if not Layout.set_grid(grid_units):
		readout.text = "  Grid %d (set Snap to %s m by hand)" % [grid_units, Convert.num(grid_units / Convert.units(), 5)]


func _process(_delta: float) -> void:
	if not is_instance_valid(readout):
		return
	var sel := EditorInterface.get_selection().get_selected_nodes()
	var text := ""
	if placing != "":
		text = "  Placing %s: click a surface. Shift keeps placing, Esc stops." % placing
	elif sel.size() == 1 and sel[0] is Node3D:
		var n: Node3D = sel[0]
		var f := Convert.to_quake(n.global_transform)
		text = "  %s  (%s)" % [n.classname if n is EntityScript else n.name, Convert.vec_text(f.origin, " ", 1)]
	elif sel.size() > 1:
		text = "  %d selected" % sel.size()
	if readout.text != text:
		readout.text = text


func _on_scene_changed(root: Node) -> void:
	if root is SceneScript or (root != null and root.get_script() != null and root.get("map_name") != null):
		EditorInterface.set_main_screen_editor("3D")


# --- Input --------------------------------------------------------------------

func _forward_3d_gui_input(camera: Camera3D, event: InputEvent) -> int:
	if event is InputEventKey and event.pressed and not event.echo:
		var k := event as InputEventKey
		if k.keycode == KEY_BRACKETLEFT and not k.ctrl_pressed:
			set_grid(grid_units / 2)
			return AFTER_GUI_INPUT_STOP
		if k.keycode == KEY_BRACKETRIGHT and not k.ctrl_pressed:
			set_grid(grid_units * 2)
			return AFTER_GUI_INPUT_STOP
		if k.keycode == KEY_F9:
			play_current()
			return AFTER_GUI_INPUT_STOP
		if k.keycode == KEY_E and k.shift_pressed and not k.ctrl_pressed:
			start_placing(browser.selected_class())
			return AFTER_GUI_INPUT_STOP
		if k.keycode == KEY_F and k.shift_pressed and k.is_command_or_control_pressed():
			find_entity()
			return AFTER_GUI_INPUT_STOP
		if k.keycode == KEY_ESCAPE and placing != "":
			placing = ""
			return AFTER_GUI_INPUT_STOP
	if placing == "":
		return AFTER_GUI_INPUT_PASS
	if event is InputEventMouseButton and event.pressed:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_RIGHT:
			placing = ""
			return AFTER_GUI_INPUT_STOP
		if mb.button_index == MOUSE_BUTTON_LEFT:
			var from := camera.project_ray_origin(mb.position)
			var dir := camera.project_ray_normal(mb.position)
			place_entity(placing, surface_point(from, dir))
			if not mb.shift_pressed:
				placing = ""
			return AFTER_GUI_INPUT_STOP
	return AFTER_GUI_INPUT_PASS


func start_placing(classname: String) -> void:
	if classname.is_empty():
		notify("Pick a class in the Entities dock first.")
		make_visible_dock()
		return
	if EditorInterface.get_edited_scene_root() == null:
		notify("Open or create a scene first (Project > Tools > Bulwark > New Bulwark Scene).")
		return
	placing = classname
	EditorInterface.set_main_screen_editor("3D")


## Where a ray from the camera meets the scene's meshes (world previews
## included), or a point 4 m ahead when it meets nothing.
func surface_point(from: Vector3, dir: Vector3) -> Dictionary:
	var best := {}
	var root := EditorInterface.get_edited_scene_root()
	if root != null:
		_ray(root, from, dir, best)
	if best.is_empty():
		var plane := Plane(Vector3.UP, 0)
		var hit = plane.intersects_ray(from, dir)
		if hit != null and from.distance_to(hit) < 200:
			return {"position": hit, "normal": Vector3.UP}
		return {"position": from + dir * 4.0, "normal": Vector3.UP}
	return best


func _ray(n: Node, from: Vector3, dir: Vector3, best: Dictionary) -> void:
	if n is MeshInstance3D and n.mesh != null and n.is_visible_in_tree():
		var key: int = n.mesh.get_rid().get_id()
		if not _hit_cache.has(key):
			var tm := TriangleMesh.new()
			tm.create_from_faces(n.mesh.get_faces())
			_hit_cache[key] = tm
		var inv: Transform3D = n.global_transform.affine_inverse()
		var hit: Dictionary = _hit_cache[key].intersect_ray(inv * from, (inv.basis * dir).normalized())
		if not hit.is_empty():
			var p: Vector3 = n.global_transform * hit.position
			if best.is_empty() or from.distance_squared_to(p) < from.distance_squared_to(best.position):
				best.position = p
				best.normal = (n.global_transform.basis.inverse().transposed() * hit.normal).normalized()
	for c in n.get_children(true):
		_ray(c, from, dir, best)


func place_entity(classname: String, hit: Dictionary) -> Node3D:
	var root := EditorInterface.get_edited_scene_root()
	if root == null:
		return null
	var parent: Node = root
	var sel := EditorInterface.get_selection().get_selected_nodes()
	if sel.size() == 1 and sel[0] != root and not (sel[0] is EntityScript) and sel[0].owner == root and sel[0] is Node3D:
		parent = sel[0] # place into the selected group
	var node: Node3D = EntityScript.new()
	node.classname = classname
	var def: Dictionary = defs.get_class_def(classname)
	# Sit the box on the surface, as Hammer does, then snap to the grid.
	var q := Convert.point_to_quake(hit.position)
	if hit.normal.dot(Vector3.UP) > 0.7 and def.get("kind", "point") == "point":
		q.z -= def.get("mins", Vector3(-8, -8, -8)).z
	var g := float(grid_units)
	q = Vector3(snappedf(q.x, g), snappedf(q.y, g), snappedf(q.z, 1.0 if hit.normal.dot(Vector3.UP) > 0.7 else g))
	var world := Transform3D(Basis.IDENTITY, Convert.point_from_quake(q))
	node.transform = (parent as Node3D).global_transform.affine_inverse() * world if parent is Node3D else world
	node.name = classname
	node.set_meta(SceneIO.M_GUID, SceneIO.random_guid())
	var ur := get_undo_redo()
	ur.create_action("Place " + classname)
	ur.add_do_method(parent, "add_child", node, true)
	ur.add_do_method(node, "set_owner", root)
	ur.add_do_reference(node)
	ur.add_undo_method(parent, "remove_child", node)
	ur.commit_action()
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(node)
	return node


# --- Scenes -------------------------------------------------------------------

func new_scene() -> void:
	_save_dialog("Create Bulwark Scene", "res://maps_src", "*.tscn", func(path: String):
		var root: Node3D = SceneScript.new()
		root.name = path.get_file().get_basename().to_pascal_case()
		root.scene_name = path.get_file().get_basename()
		_save_and_open(root, path))


func import_dialog() -> void:
	var dlg := EditorFileDialog.new()
	dlg.access = EditorFileDialog.ACCESS_FILESYSTEM
	dlg.file_mode = EditorFileDialog.FILE_MODE_OPEN_FILE
	dlg.title = "Import a Bulwark or s&box scene"
	dlg.add_filter("*.scene,*.prefab", "Scenes and prefabs")
	dlg.current_dir = Paths.game_dir()
	dlg.file_selected.connect(func(path: String):
		dlg.queue_free()
		var io := SceneIO.new()
		var root := io.import_file(path)
		for w in io.warnings:
			push_warning("Bulwark: " + w)
		if root == null:
			notify("\n".join(io.errors), true)
			return
		var out := "res://maps_src/%s.tscn" % path.get_file().get_basename()
		if path.get_extension() == "prefab":
			out = "res://maps_src/prefabs/%s.tscn" % path.get_file().get_basename()
		_save_and_open(root, out))
	dlg.canceled.connect(dlg.queue_free)
	EditorInterface.get_base_control().add_child(dlg)
	dlg.popup_file_dialog()


func _save_dialog(title: String, dir: String, filter: String, done: Callable) -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dir))
	var dlg := EditorFileDialog.new()
	dlg.access = EditorFileDialog.ACCESS_RESOURCES
	dlg.file_mode = EditorFileDialog.FILE_MODE_SAVE_FILE
	dlg.title = title
	dlg.add_filter(filter)
	dlg.current_dir = dir
	dlg.current_file = "new_map.tscn"
	dlg.file_selected.connect(func(path): dlg.queue_free(); done.call(path))
	dlg.canceled.connect(dlg.queue_free)
	EditorInterface.get_base_control().add_child(dlg)
	dlg.popup_file_dialog()


func _save_and_open(root: Node, path: String) -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(path.get_base_dir()))
	var packed := PackedScene.new()
	var err := packed.pack(root)
	root.free()
	if err == OK:
		err = ResourceSaver.save(packed, path)
	if err != OK:
		notify("Could not save %s: %s" % [path, error_string(err)], true)
		return
	EditorInterface.get_resource_filesystem().update_file(path)
	EditorInterface.open_scene_from_path(path)
	EditorInterface.set_main_screen_editor("3D")


## The edited scene's root when it can be exported.
func current_root() -> Node3D:
	var root := EditorInterface.get_edited_scene_root()
	if root is SceneScript or (root is Node3D and root.get("map_name") != null):
		return root
	notify("Open a scene whose root is a BulwarkScene3D (or a Stiletto FTEWorld3D) first.", true)
	return null


## Exports the open scene. A Stiletto world is exported to FTEW first, and the
## scene's MapInstance points at it. Returns the scene file, or "".
func export_current() -> String:
	var root := current_root()
	if root == null:
		return ""
	if root.get("map_name") != null and not root is SceneScript:
		if not ResourceLoader.exists(STILETTO_EXPORTER):
			notify("This is a Stiletto world, but the stiletto_tools addon isn't in this project.", true)
			return ""
		var report: Dictionary = load(STILETTO_EXPORTER).new().export_world(root, Paths.game_dir())
		if not report.get("ok", false):
			notify("FTEW export failed: " + "; ".join(PackedStringArray(report.get("errors", ["unknown error"]))), true)
			return ""
	var io := SceneIO.new()
	var path := io.export_file(root)
	for w in io.warnings:
		push_warning("Bulwark: " + w)
	if path.is_empty():
		notify("Export failed: " + "; ".join(io.errors), true)
		return ""
	# Export gives new objects their GUIDs; keep them with the scene.
	EditorInterface.save_scene()
	notify("Wrote " + path + ("" if io.warnings.is_empty() else "  (%d warnings in Output)" % io.warnings.size()))
	return path


func engine_path() -> String:
	var set_path := str(Paths.get_setting(Paths.ENGINE, ""))
	if not set_path.is_empty():
		return Paths.absolute(set_path)
	var exe := "bulwark.exe" if OS.get_name() == "Windows" else "bulwark"
	for dir in [Paths.game_dir().get_base_dir(), Paths.game_dir().get_base_dir().path_join("build"), Paths.game_dir()]:
		if FileAccess.file_exists(dir.path_join(exe)):
			return dir.path_join(exe)
	return ""


func launch_arguments(map: String) -> PackedStringArray:
	var base := str(Paths.get_setting(Paths.BASE_DIR, ""))
	var games: PackedStringArray = Paths.get_setting(Paths.GAMES, PackedStringArray())
	var game := Paths.game_dir()
	var args: PackedStringArray = []
	if base.is_empty():
		args.append_array(["-basedir", game.get_base_dir()])
		if not games.has(game.get_file()):
			games = games.duplicate()
			games.append(game.get_file())
	else:
		args.append_array(["-basedir", Paths.absolute(base)])
	for g in games:
		args.append_array(["-game", g])
	args.append_array(str(Paths.get_setting(Paths.LAUNCH_ARGS, "")).split(" ", false))
	args.append_array(["+map", map])
	return args


func play_current() -> void:
	var engine := engine_path()
	if engine.is_empty():
		notify("Set Project Settings > Bulwark > Engine Executable to your Bulwark build.", true)
		return
	var scene := export_current()
	if scene.is_empty():
		return
	var args := launch_arguments(scene.get_file().get_basename())
	var pid := OS.create_process(engine, args)
	if pid < 0:
		notify("Bulwark didn't start: " + engine, true)
	else:
		notify("Running %s %s" % [engine.get_file(), " ".join(args)])


# --- Find entity --------------------------------------------------------------

## Hammer's Entity Report: every entity in the scene, filtered as you type.
## Enter or double-click selects it and centres the views on it.
func find_entity() -> void:
	var root := EditorInterface.get_edited_scene_root()
	if root == null:
		return
	if not is_instance_valid(finder):
		finder = ConfirmationDialog.new()
		finder.title = "Find Entity"
		finder.ok_button_text = "Select"
		var box := VBoxContainer.new()
		var search := LineEdit.new()
		search.placeholder_text = "classname, targetname, node name or key value"
		search.name = "Search"
		box.add_child(search)
		var list := ItemList.new()
		list.name = "List"
		list.custom_minimum_size = Vector2(520, 320)
		list.size_flags_vertical = Control.SIZE_EXPAND_FILL
		box.add_child(list)
		finder.add_child(box)
		finder.register_text_enter(search)
		search.text_changed.connect(func(_t): _fill_finder())
		list.item_activated.connect(func(_i): finder.hide(); _finder_pick())
		finder.confirmed.connect(_finder_pick)
		EditorInterface.get_base_control().add_child(finder)
	_fill_finder()
	finder.popup_centered()
	finder.find_child("Search", true, false).grab_focus()


func _fill_finder() -> void:
	var search: LineEdit = finder.find_child("Search", true, false)
	var list: ItemList = finder.find_child("List", true, false)
	list.clear()
	var q := search.text.strip_edges().to_lower()
	var nodes: Array = []
	_entities(EditorInterface.get_edited_scene_root(), nodes)
	for n in nodes:
		var tn: String = n.properties.get("targetname", "")
		var hay: String = (n.classname + " " + tn + " " + String(n.name) + " " + " ".join(PackedStringArray(n.properties.values()))).to_lower()
		if not q.is_empty() and not hay.contains(q):
			continue
		var o := Convert.to_quake(n.global_transform).origin
		list.add_item("%s   %s   %s   (%s)" % [n.classname, tn if not tn.is_empty() else "-", n.name, Convert.vec_text(o, " ", 0)])
		list.set_item_metadata(list.item_count - 1, n)
	if list.item_count > 0:
		list.select(0)


static func _entities(n: Node, out: Array) -> void:
	if n == null:
		return
	if n is EntityScript:
		out.append(n)
	for c in n.get_children():
		_entities(c, out)


func _finder_pick() -> void:
	var list: ItemList = finder.find_child("List", true, false)
	var sel := list.get_selected_items()
	if sel.is_empty():
		return
	var n: Node3D = list.get_item_metadata(sel[0])
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(n)
	EditorInterface.edit_node(n)
	Layout.look_at(n.global_position, 2.0)
