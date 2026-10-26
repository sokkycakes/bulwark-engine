@tool
extends EditorInspectorPlugin
## The entity inspector: an entity's keys laid out from its class definition,
## with an editor for each key type (choices, spawnflags, colours, targets,
## file paths), its origin and angles in Quake units, and the keys the class
## doesn't define. Every change is one undoable action.

const Convert := preload("convert.gd")
const Paths := preload("paths.gd")
const Gizmo := preload("gizmo.gd")
const EntityScript := preload("nodes/entity.gd")
const SceneScript := preload("nodes/scene_root.gd")

const TARGET_TYPES := ["target_destination", "target_source", "target_name_or_class", "pointentityclass", "filterclass", "node_dest"]
const FILE_TYPES := {"studio": "*.iqm,*.mdl,*.vvm,*.md3,*.obj,*.gltf,*.glb", "sprite": "*.spr,*.spr32,*.png,*.tga",
	"sound": "*.wav,*.ogg,*.mp3,*.sndshd", "decal": "*.png,*.tga,*.mat", "material": "*.mat,*.png,*.tga"}

var plugin: EditorPlugin
var defs
var pick_class: Callable # opens the class picker; called with a Callable(String)


func _can_handle(object: Object) -> bool:
	return object is EntityScript or (object is SceneScript and object.kind == 1)


func _parse_begin(object: Object) -> void:
	if object is SceneScript and object.classname.is_empty():
		return
	add_custom_control(_panel(object))


func _parse_property(object: Object, _type: Variant.Type, name: String, _hint: PropertyHint, _hint_text: String, _usage: int, _wide: bool) -> bool:
	# The panel replaces the raw dictionary editor.
	return name == "properties" and (object is EntityScript or not object.classname.is_empty())


# --- Changes ------------------------------------------------------------------

func _set_key(node: Node, key: String, value: String) -> void:
	var old: Dictionary = node.properties
	var new := old.duplicate()
	if value.is_empty():
		new.erase(key)
	else:
		new[key] = value
	if new == old:
		return
	var ur := plugin.get_undo_redo()
	ur.create_action("Set %s to \"%s\"" % [key, value], UndoRedo.MERGE_DISABLE, node)
	ur.add_do_property(node, "properties", new)
	ur.add_undo_property(node, "properties", old)
	ur.add_do_method(node, "notify_property_list_changed")
	ur.add_undo_method(node, "notify_property_list_changed")
	ur.commit_action()


func _rename_key(node: Node, key: String, to: String) -> void:
	if to.is_empty() or to == key or node.properties.has(to):
		return
	var old: Dictionary = node.properties
	var new := {}
	for k in old:
		new[to if k == key else k] = old[k]
	var typed: Dictionary[String, String] = {}
	typed.assign(new)
	var ur := plugin.get_undo_redo()
	ur.create_action("Rename key %s to %s" % [key, to], UndoRedo.MERGE_DISABLE, node)
	ur.add_do_property(node, "properties", typed)
	ur.add_undo_property(node, "properties", old)
	ur.add_do_method(node, "notify_property_list_changed")
	ur.add_undo_method(node, "notify_property_list_changed")
	ur.commit_action()


func set_classname(node: Node, classname: String) -> void:
	if classname == node.classname:
		return
	var ur := plugin.get_undo_redo()
	ur.create_action("Change class to " + classname, UndoRedo.MERGE_DISABLE, node)
	ur.add_do_property(node, "classname", classname)
	ur.add_undo_property(node, "classname", node.classname)
	# Nodes still named after their class follow it.
	if String(node.name).begins_with(node.classname) and node.get_parent() != null:
		var new_name := classname + String(node.name).substr(node.classname.length())
		ur.add_do_method(node, "set_name", new_name)
		ur.add_undo_method(node, "set_name", String(node.name))
	ur.add_do_method(node, "notify_property_list_changed")
	ur.add_undo_method(node, "notify_property_list_changed")
	ur.commit_action()


## The entity's Bulwark frame: world transform turned to face its +X.
static func entity_frame(node: Node3D) -> Transform3D:
	return Convert.to_quake(node.global_transform) * Convert.FACING


func _set_frame(node: Node3D, frame: Transform3D) -> void:
	var global := Convert.from_quake(frame * Convert.FACING.affine_inverse())
	var parent := node.get_parent() as Node3D
	var local := parent.global_transform.affine_inverse() * global if parent != null else global
	var ur := plugin.get_undo_redo()
	ur.create_action("Move " + node.name, UndoRedo.MERGE_DISABLE, node)
	ur.add_do_property(node, "transform", local)
	ur.add_undo_property(node, "transform", node.transform)
	ur.add_do_method(node, "notify_property_list_changed")
	ur.add_undo_method(node, "notify_property_list_changed")
	ur.commit_action()


# --- Panel --------------------------------------------------------------------

func _panel(node: Node) -> Control:
	var def: Dictionary = defs.get_class_def(node.classname) if defs != null else {}
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)

	var head := HBoxContainer.new()
	var title := Label.new()
	title.text = node.classname
	title.add_theme_font_size_override("font_size", 18)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title.tooltip_text = def.get("source", "Not in the loaded entity definitions")
	head.add_child(title)
	if node is EntityScript:
		var change := Button.new()
		change.text = "Change class…"
		change.pressed.connect(func(): if pick_class.is_valid(): pick_class.call(func(c): set_classname(node, c)))
		head.add_child(change)
	box.add_child(head)

	if def.is_empty():
		box.add_child(_note("This class isn't in the loaded definitions, so its keys are listed as plain text. Set definition files under Project Settings > Bulwark > Entity Definitions."))
	elif not str(def.description).is_empty():
		var desc := _note(def.description)
		desc.max_lines_visible = 4
		desc.tooltip_text = def.description
		box.add_child(desc)

	if node is EntityScript:
		box.add_child(_transform_rows(node))
	var used := {}
	if not def.get("keys", []).is_empty():
		box.add_child(_section("Keys"))
		var grid := _grid()
		for key in def.keys:
			if key.name in ["origin", "angles", "angle", "classname", "spawnflags"] and node is EntityScript:
				used[key.name] = true
				continue
			used[key.name] = true
			_key_row(grid, node, key)
		box.add_child(grid)
	if not def.get("flags", []).is_empty():
		used["spawnflags"] = true
		box.add_child(_section("Spawnflags"))
		box.add_child(_flags(node, def.flags))
	if not def.get("outputs", []).is_empty():
		box.add_child(_section("Outputs  (target,Input,parameter,delay,times)"))
		var grid := _grid()
		for o in def.outputs:
			used[o.name] = true
			_key_row(grid, node, {"name": o.name, "type": "output", "label": o.name, "default": "", "description": o.description, "choices": []})
		box.add_child(grid)
	box.add_child(_section("Other keys"))
	var other := _grid()
	for k in node.properties:
		if not used.has(k):
			_raw_row(other, node, k)
	_add_row(other, node)
	box.add_child(other)
	box.add_child(_link_buttons(node))
	box.add_child(HSeparator.new())
	return box


static func _note(text: String) -> Label:
	var l := Label.new()
	l.text = text
	l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	l.custom_minimum_size.x = 120
	l.modulate = Color(1, 1, 1, 0.75)
	return l


static func _section(text: String) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_color_override("font_color", Color(0.55, 0.8, 1))
	return l


static func _grid() -> GridContainer:
	var g := GridContainer.new()
	g.columns = 2
	return g


func _label(grid: GridContainer, text: String, tip: String) -> void:
	var l := Label.new()
	l.text = text
	l.tooltip_text = tip
	l.mouse_filter = Control.MOUSE_FILTER_PASS
	l.clip_text = true
	l.custom_minimum_size.x = 90
	l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	l.size_flags_stretch_ratio = 0.6
	grid.add_child(l)


## A LineEdit that commits on Enter or when it loses focus.
func _line(value: String, placeholder: String, commit: Callable) -> LineEdit:
	var e := LineEdit.new()
	e.text = value
	e.placeholder_text = placeholder
	e.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	e.select_all_on_focus = true
	e.text_submitted.connect(func(t): commit.call(t))
	e.focus_exited.connect(func(): if e.text != value: commit.call(e.text))
	return e


func _transform_rows(node: Node3D) -> Control:
	var grid := _grid()
	var f := entity_frame(node)
	var origin := Convert.vec_text(f.origin, " ", 2)
	var angles := Convert.vec_text(Convert.angles_from_basis(f.basis), " ", 2)
	_label(grid, "Origin", "World position in Bulwark units (x y z), as the game sees it.")
	grid.add_child(_line(origin, "x y z", func(t):
		var v := Convert.parse_floats(t)
		if v.size() == 3:
			var nf := entity_frame(node)
			nf.origin = Vector3(v[0], v[1], v[2])
			_set_frame(node, nf)))
	_label(grid, "Angles", "Pitch yaw roll in degrees. Positive pitch looks down. The node's -Z axis is the facing.")
	grid.add_child(_line(angles, "pitch yaw roll", func(t):
		var v := Convert.parse_floats(t)
		if v.size() == 1:
			v = PackedFloat64Array([0, v[0], 0])
		if v.size() == 3:
			var nf := entity_frame(node)
			var s := nf.basis.get_scale()
			nf.basis = Convert.basis_from_angles(Vector3(v[0], v[1], v[2])).scaled_local(s)
			_set_frame(node, nf)))
	return grid


func _key_row(grid: GridContainer, node: Node, key: Dictionary) -> void:
	var name: String = key.name
	var value: String = node.properties.get(name, "")
	var label: String = key.get("label", name)
	var tip := name + ("\n" + str(key.description) if not str(key.get("description", "")).is_empty() else "")
	if str(key.get("default", "")) != "":
		tip += "\nDefault: " + str(key.default)
	_label(grid, label if label.length() <= 28 else name, tip)
	var type: String = key.get("type", "string")
	var commit := func(t): _set_key(node, name, str(t).strip_edges())
	if not key.get("choices", []).is_empty():
		var opt := OptionButton.new()
		opt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		opt.add_item("(default%s)" % ((": " + str(key.default)) if str(key.default) != "" else ""))
		opt.set_item_metadata(0, "")
		var selected := 0
		for c in key.choices:
			opt.add_item("%s  (%s)" % [c[1], c[0]] if c[1] != "" else str(c[0]))
			opt.set_item_metadata(opt.item_count - 1, str(c[0]))
			if str(c[0]) == value:
				selected = opt.item_count - 1
		if selected == 0 and not value.is_empty():
			opt.add_item(value)
			opt.set_item_metadata(opt.item_count - 1, value)
			selected = opt.item_count - 1
		opt.select(selected)
		opt.item_selected.connect(func(i): commit.call(opt.get_item_metadata(i)))
		grid.add_child(opt)
		return
	match type:
		"boolean":
			var cb := CheckBox.new()
			cb.button_pressed = value == "1" or (value.is_empty() and str(key.default) == "1")
			cb.toggled.connect(func(on): commit.call("1" if on else "0"))
			grid.add_child(cb)
		"color255", "color1":
			var row := HBoxContainer.new()
			row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			var picker := ColorPickerButton.new()
			picker.custom_minimum_size = Vector2(32, 0)
			picker.edit_alpha = false
			picker.color = _parse_color(value if not value.is_empty() else str(key.default), type)
			var line := _line(value, str(key.default), commit)
			picker.popup_closed.connect(func():
				var parts := Convert.parse_floats(line.text)
				commit.call(_color_text(picker.color, type, parts[3] if parts.size() > 3 else -1.0)))
			row.add_child(picker)
			row.add_child(line)
			grid.add_child(row)
		_:
			var row := HBoxContainer.new()
			row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			var line := _line(value, str(key.get("default", "")), commit)
			row.add_child(line)
			if type in TARGET_TYPES or type == "output" or name in Gizmo.LINK_KEYS:
				row.add_child(_target_menu(node, line, commit, type == "output"))
			elif FILE_TYPES.has(type):
				var browse := Button.new()
				browse.text = "…"
				browse.tooltip_text = "Pick a file in the game directory"
				browse.pressed.connect(func(): _browse(FILE_TYPES[type], commit))
				row.add_child(browse)
			grid.add_child(row)


func _raw_row(grid: GridContainer, node: Node, key: String) -> void:
	var k := _line(key, "key", func(t): _rename_key(node, key, str(t).strip_edges()))
	k.size_flags_stretch_ratio = 0.6
	k.tooltip_text = "Rename the key. Empty the value to remove it."
	grid.add_child(k)
	grid.add_child(_line(node.properties[key], "(removes the key)", func(t): _set_key(node, key, str(t).strip_edges())))


func _add_row(grid: GridContainer, node: Node) -> void:
	var k := LineEdit.new()
	k.placeholder_text = "new key"
	k.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	k.size_flags_stretch_ratio = 0.6
	var v := LineEdit.new()
	v.placeholder_text = "value, then Enter"
	v.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var add := func(_t = ""):
		if not k.text.strip_edges().is_empty() and not v.text.is_empty():
			_set_key(node, k.text.strip_edges(), v.text)
	k.text_submitted.connect(func(_t): v.grab_focus())
	v.text_submitted.connect(add)
	grid.add_child(k)
	grid.add_child(v)


func _flags(node: Node, flags: Array) -> Control:
	var grid := GridContainer.new()
	grid.columns = 2
	var value := int(node.properties.get("spawnflags", "0"))
	for f in flags:
		var cb := CheckBox.new()
		cb.text = str(f.name) if str(f.name) != "" else str(f.bit)
		cb.tooltip_text = "%s (%d)%s" % [f.name, f.bit, ("\n" + str(f.description)) if str(f.description) != "" else ""]
		cb.button_pressed = (value & int(f.bit)) != 0
		cb.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		cb.clip_text = true
		var bit := int(f.bit)
		cb.toggled.connect(func(on):
			var cur := int(node.properties.get("spawnflags", "0"))
			cur = (cur | bit) if on else (cur & ~bit)
			_set_key(node, "spawnflags", str(cur) if cur != 0 else ""))
		grid.add_child(cb)
	return grid


## A menu of the scene's targetnames for a key that names an entity.
func _target_menu(node: Node, line: LineEdit, commit: Callable, output: bool) -> MenuButton:
	var menu := MenuButton.new()
	menu.text = "▾"
	menu.tooltip_text = "Pick an entity by targetname"
	menu.flat = false
	menu.about_to_popup.connect(func():
		var p := menu.get_popup()
		p.clear()
		var names := targetnames(node)
		for n in names:
			p.add_item(n)
		if names.is_empty():
			p.add_item("No named entities in this scene")
			p.set_item_disabled(0, true))
	menu.get_popup().index_pressed.connect(func(i):
		var picked := menu.get_popup().get_item_text(i)
		if output:
			var parts := line.text.split(",")
			parts[0] = picked
			if parts.size() == 1:
				parts.append_array(["Trigger", "", "0", "-1"])
			commit.call(",".join(parts))
		else:
			commit.call(picked))
	return menu


static func targetnames(node: Node) -> PackedStringArray:
	var out := {}
	var root := node.get_tree().edited_scene_root if node.is_inside_tree() else null
	if root != null:
		_names(root, out)
	var list := PackedStringArray(out.keys())
	list.sort()
	return list


static func _names(n: Node, out: Dictionary) -> void:
	if n is EntityScript and not str(n.properties.get("targetname", "")).is_empty():
		out[n.properties.targetname] = true
	for c in n.get_children():
		_names(c, out)


func _link_buttons(node: Node) -> Control:
	var row := HBoxContainer.new()
	var to := Button.new()
	to.text = "Select targets"
	to.tooltip_text = "Select the entities this one's keys point at"
	to.pressed.connect(func(): _select(Gizmo.linked_nodes(node)))
	row.add_child(to)
	var from := Button.new()
	from.text = "Select users"
	from.tooltip_text = "Select the entities that point at this one"
	from.pressed.connect(func(): _select(users_of(node)))
	row.add_child(from)
	return row


static func users_of(node: Node) -> Array:
	var out: Array = []
	var root := node.get_tree().edited_scene_root if node.is_inside_tree() else null
	if root != null:
		_users(root, node, out)
	return out


static func _users(n: Node, target: Node, out: Array) -> void:
	if n != target and n is EntityScript and target in Gizmo.linked_nodes(n):
		out.append(n)
	for c in n.get_children():
		_users(c, target, out)


func _select(nodes: Array) -> void:
	if nodes.is_empty():
		EditorInterface.get_editor_toaster().push_toast("Nothing linked.")
		return
	var sel := EditorInterface.get_selection()
	sel.clear()
	for n in nodes:
		sel.add_node(n)
	EditorInterface.edit_node(nodes[0])


func _browse(filters: String, commit: Callable) -> void:
	var dlg := EditorFileDialog.new()
	dlg.access = EditorFileDialog.ACCESS_FILESYSTEM
	dlg.file_mode = EditorFileDialog.FILE_MODE_OPEN_FILE
	dlg.current_dir = Paths.game_dir()
	for f in filters.split(","):
		dlg.add_filter(f)
	dlg.file_selected.connect(func(path):
		var rel := Paths.game_relative(path)
		commit.call(rel if not rel.is_empty() else path)
		dlg.queue_free())
	dlg.canceled.connect(dlg.queue_free)
	EditorInterface.get_base_control().add_child(dlg)
	dlg.popup_file_dialog()


static func _parse_color(text: String, type: String) -> Color:
	var v := Convert.parse_floats(text)
	if v.size() < 3:
		return Color.WHITE
	if type == "color1":
		return Color(v[0], v[1], v[2])
	return Color8(int(v[0]), int(v[1]), int(v[2]))


static func _color_text(c: Color, type: String, brightness: float) -> String:
	if type == "color1":
		return "%s %s %s" % [Convert.num(c.r, 3), Convert.num(c.g, 3), Convert.num(c.b, 3)]
	var s := "%d %d %d" % [c.r8, c.g8, c.b8]
	return s + (" " + Convert.num(brightness, 2) if brightness >= 0 else "")
