@tool
extends VBoxContainer
## The entity browser dock: every class from the loaded definitions, grouped
## by prefix and searchable. Pick a class and press Place (or double-click
## it), then click in the 3D view to put the entity on the surface under the
## mouse. Shift keeps placing; Escape or right-click stops.

signal place_requested(classname: String)
signal picked(classname: String)

var defs
var _search: LineEdit
var _tree: Tree
var _info: RichTextLabel
var _place: Button
var _status: Label
var _pick_callback: Callable


func _init() -> void:
	name = "Entities"
	custom_minimum_size = Vector2(220, 300)
	_search = LineEdit.new()
	_search.placeholder_text = "Search entities (name, key or text)"
	_search.clear_button_enabled = true
	_search.text_changed.connect(func(_t): refresh())
	_search.text_submitted.connect(func(_t): _activate())
	add_child(_search)
	_tree = Tree.new()
	_tree.hide_root = true
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.item_selected.connect(_show_info)
	_tree.item_activated.connect(_activate)
	add_child(_tree)
	_info = RichTextLabel.new()
	_info.bbcode_enabled = true
	_info.fit_content = false
	_info.scroll_active = true
	_info.custom_minimum_size.y = 110
	_info.selection_enabled = true
	add_child(_info)
	var row := HBoxContainer.new()
	_place = Button.new()
	_place.text = "Place"
	_place.tooltip_text = "Click in the 3D view to place the selected entity (Shift-click to keep placing)"
	_place.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_place.pressed.connect(_activate)
	row.add_child(_place)
	add_child(row)
	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.modulate = Color(1, 1, 1, 0.7)
	add_child(_status)


func selected_class() -> String:
	var item := _tree.get_selected()
	return item.get_metadata(0) if item != null and item.get_metadata(0) is String else ""


func set_status(text: String) -> void:
	_status.text = text


## Puts the dock in pick mode: the next activated class goes to `callback`
## instead of starting placement.
func pick(callback: Callable) -> void:
	_pick_callback = callback
	_place.text = "Use class"
	set_status("Pick a class for the selected entity, then double-click it or press Use class.")
	_search.grab_focus()


func refresh() -> void:
	_tree.clear()
	var root := _tree.create_item()
	if defs == null or defs.classes.is_empty():
		_info.text = "No entity definitions loaded. Point Project Settings > Bulwark > Entity Definitions at Nuclide's Tools/base_entities.def, an FGD, or decls/def, then use Bulwark > Reload Entity Definitions."
		return
	var query := _search.text.strip_edges().to_lower()
	var groups := {}
	var first: TreeItem = null
	for n in defs.names():
		var def: Dictionary = defs.classes[n]
		if not query.is_empty() and not _matches(def, query):
			continue
		var g: String = def.group
		if not groups.has(g):
			var gi := _tree.create_item(root)
			gi.set_text(0, g)
			gi.set_selectable(0, false)
			gi.collapsed = query.is_empty()
			groups[g] = gi
		var item := _tree.create_item(groups[g])
		item.set_text(0, n)
		item.set_metadata(0, n)
		item.set_custom_color(0, def.color.lightened(0.3))
		item.set_tooltip_text(0, str(def.description).left(300))
		if def.kind == "solid":
			item.set_suffix(0, "brush")
		if first == null:
			first = item
	if not query.is_empty() and first != null:
		first.select(0)


static func _matches(def: Dictionary, q: String) -> bool:
	if def.name.to_lower().contains(q):
		return true
	if q.length() >= 3:
		if str(def.description).to_lower().contains(q):
			return true
		for k in def.keys:
			if k.name.to_lower() == q:
				return true
	return false


func _show_info() -> void:
	var n := selected_class()
	if n.is_empty():
		return
	var def: Dictionary = defs.classes[n]
	var t := "[b]%s[/b]  [color=#888](%s, %s)[/color]\n%s" % [n, def.kind, def.source.get_file(), str(def.description).replace("[", "[lb]")]
	if not def.keys.is_empty():
		var names: PackedStringArray = []
		for k in def.keys:
			names.append(k.name)
		t += "\n[color=#8cf]Keys:[/color] " + ", ".join(names)
	if not def.inputs.is_empty():
		var names: PackedStringArray = []
		for k in def.inputs:
			names.append(k.name)
		t += "\n[color=#8cf]Inputs:[/color] " + ", ".join(names)
	_info.text = t


func _activate() -> void:
	var n := selected_class()
	if n.is_empty():
		return
	if _pick_callback.is_valid():
		var cb := _pick_callback
		_pick_callback = Callable()
		_place.text = "Place"
		set_status("")
		cb.call(n)
		picked.emit(n)
		return
	place_requested.emit(n)
