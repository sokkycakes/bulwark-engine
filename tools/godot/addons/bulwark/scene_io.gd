@tool
extends RefCounted
## Reads and writes Bulwark scenes and prefabs (s&box's .scene / .prefab
## JSON, described in docs/spec/scene-format.md) from Godot node trees.
##
## Mapping:
## - BulwarkScene3D root: the file. A scene's MapInstance object comes from
##   its World settings; a prefab's RootObject is the root itself.
## - BulwarkEntity3D: a GameObject with one Bulwark.Entity component.
## - BulwarkPrefab3D: a prefab instance stub.
## - Any other node that holds one of those, or was imported from a file,
##   becomes a plain GameObject so the hierarchy survives.
## Meshes, CSG, lights and the like are skipped: they are preview or
## Stiletto world geometry.
##
## Whatever Bulwark doesn't edit (other components, tags, network settings,
## file-level properties, the JSON types of spawn keys) is kept in node
## metadata on import and written back on export.

const Convert := preload("convert.gd")
const Paths := preload("paths.gd")
const EntityScript := preload("nodes/entity.gd")
const SceneScript := preload("nodes/scene_root.gd")
const PrefabScript := preload("nodes/prefab.gd")

const FLAG_EDITOR_ONLY := 2048
const FLAG_ABSOLUTE := 4096
const ENTITY_TYPE := "Bulwark.Entity"

## Metadata names.
const M_GUID := &"bulwark_guid"
const M_COMPONENT_GUID := &"bulwark_component_guid"
const M_COMPONENTS := &"bulwark_components"
const M_EXTRA := &"bulwark_extra"
const M_TYPES := &"bulwark_types"
const M_FILE := &"bulwark_file"
const M_MAP_OBJECT := &"bulwark_map_object"
const M_PATCH := &"bulwark_patch"
const M_ID_MAP := &"bulwark_id_map"
const M_ENABLED := &"bulwark_enabled"

## GameObject keys this exporter writes itself.
const GO_KEYS := ["__guid", "__version", "Flags", "Name", "Position", "Rotation", "Scale", "Enabled", "Components", "Children"]

var errors: PackedStringArray = []
var warnings: PackedStringArray = []
var _guids: Dictionary = {} # guid -> node, while exporting


# --- GUIDs and JSON -----------------------------------------------------------

static func random_guid() -> String:
	var b := PackedByteArray()
	for i in 16:
		b.append(randi() & 0xff)
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	var h := b.hex_encode()
	return "%s-%s-%s-%s-%s" % [h.substr(0, 8), h.substr(8, 4), h.substr(12, 4), h.substr(16, 4), h.substr(20, 12)]


static func is_guid(s: String) -> bool:
	if s.length() != 36:
		return false
	for i in 36:
		var c := s[i]
		if i in [8, 13, 18, 23]:
			if c != "-":
				return false
		elif not c in "0123456789abcdefABCDEF":
			return false
	return true


## JSON text the way s&box writes it: 2-space indent, members in order.
## Floats that hold whole numbers are written as integers, since Godot's
## parser reads every number as a float.
static func to_json(v: Variant, indent: String = "") -> String:
	match typeof(v):
		TYPE_NIL:
			return "null"
		TYPE_BOOL:
			return "true" if v else "false"
		TYPE_INT:
			return str(v)
		TYPE_FLOAT:
			if is_finite(v) and v == floorf(v) and absf(v) < 1e15:
				return str(int(v))
			return JSON.stringify(v)
		TYPE_STRING, TYPE_STRING_NAME:
			return JSON.stringify(String(v))
		TYPE_DICTIONARY:
			if v.is_empty():
				return "{}"
			var inner := indent + "  "
			var parts: PackedStringArray = []
			for k in v:
				parts.append(inner + JSON.stringify(String(k)) + ": " + to_json(v[k], inner))
			return "{\n" + ",\n".join(parts) + "\n" + indent + "}"
		TYPE_ARRAY, TYPE_PACKED_STRING_ARRAY, TYPE_PACKED_FLOAT32_ARRAY, TYPE_PACKED_FLOAT64_ARRAY, TYPE_PACKED_INT32_ARRAY:
			if v.is_empty():
				return "[]"
			var inner := indent + "  "
			var parts: PackedStringArray = []
			for item in v:
				parts.append(inner + to_json(item, inner))
			return "[\n" + ",\n".join(parts) + "\n" + indent + "]"
	return JSON.stringify(str(v))


static func read_json(path: String) -> Variant:
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		return null
	var json := JSON.new()
	if json.parse(text) != OK:
		push_error("%s:%d: %s" % [path, json.get_error_line(), json.get_error_message()])
		return null
	return json.data


# --- Export -------------------------------------------------------------------

## The file for a scene tree, as a Dictionary ready for to_json().
func export_tree(root: Node3D) -> Dictionary:
	errors.clear()
	warnings.clear()
	_guids.clear()
	var prefab: bool = root is SceneScript and root.kind == 1
	var file: Dictionary = root.get_meta(M_FILE, {}).duplicate(true)
	var objects: Array = []
	for child in root.get_children():
		if child is Node3D and _is_object(child, root):
			objects.append(_export_object(child, root, Transform3D.IDENTITY))
	if prefab:
		var go := _object_header(root, root.name)
		_put_transform(go, Transform3D.IDENTITY)
		go["Enabled"] = true
		go["Components"] = _components(root, root.classname, root.properties)
		go["Children"] = objects
		var out := {"RootObject": go}
		_merge_file(out, file, {"ShowInMenu": false, "MenuPath": null, "MenuIcon": null, "DontBreakAsTemplate": false,
			"ResourceVersion": 2, "__references": [], "__version": 2})
		_resolve_references(out)
		return out
	var map := _map_object(root)
	if not map.is_empty():
		objects.push_front(map)
	if not is_guid(str(file.get("__guid", ""))):
		file["__guid"] = random_guid()
		var kept: Dictionary = root.get_meta(M_FILE, {}).duplicate(true)
		kept["__guid"] = file.__guid
		root.set_meta(M_FILE, kept)
	var out := {}
	out["__guid"] = file.__guid
	out["GameObjects"] = objects
	var props: Dictionary = file.get("SceneProperties", {}).duplicate(true)
	var title: String = root.title if root is SceneScript else str(root.get("title") if root.get("title") != null else "")
	if not title.is_empty():
		var meta: Dictionary = props.get("Metadata", {})
		meta["Title"] = title
		props["Metadata"] = meta
	out["SceneProperties"] = props
	_merge_file(out, file, {"ResourceVersion": 3, "Title": null, "Description": null, "__references": [], "__version": 3})
	_resolve_references(out)
	return out


func export_text(root: Node3D) -> String:
	return to_json(export_tree(root)) + "\n"


## Writes maps/<name>.scene or prefabs/<name>.prefab under the game
## directory, or to `path` when given. Returns the absolute path written.
func export_file(root: Node3D, path: String = "") -> String:
	if path.is_empty():
		path = default_output(root)
	var text := export_text(root)
	if not errors.is_empty():
		return ""
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var tmp := path + ".tmp"
	var f := FileAccess.open(tmp, FileAccess.WRITE)
	if f == null:
		errors.append("Cannot write " + path)
		return ""
	f.store_string(text)
	f.close()
	if DirAccess.rename_absolute(tmp, path) != OK:
		errors.append("Cannot replace " + path)
		return ""
	return path


static func output_name(root: Node3D) -> String:
	var n: String = str(root.get("scene_name")) if root.get("scene_name") != null else ""
	if n.is_empty() and root.get("map_name") != null:
		n = str(root.get("map_name"))
	if n.is_empty() and not root.scene_file_path.is_empty():
		n = root.scene_file_path.get_file().get_basename()
	if n.is_empty():
		n = String(root.name).to_snake_case()
	return n


static func default_output(root: Node3D) -> String:
	var prefab: bool = root is SceneScript and root.kind == 1
	return Paths.game_file(("prefabs/%s.prefab" if prefab else "maps/%s.scene") % output_name(root))


func _merge_file(out: Dictionary, file: Dictionary, defaults: Dictionary) -> void:
	for k in file:
		if not out.has(k) and k != "GameObjects" and k != "RootObject":
			out[k] = file[k]
	for k in defaults:
		if not out.has(k):
			out[k] = defaults[k]
	# Keep __version last, as s&box does.
	if out.has("__version"):
		var v = out["__version"]
		out.erase("__version")
		out["__version"] = v


## Whether a node is written as a GameObject.
func _is_object(node: Node, root: Node) -> bool:
	if node.owner != root and node != root:
		return false # previews and other unsaved nodes
	if node is EntityScript or node is PrefabScript or node.has_meta(M_GUID):
		return true
	if node.get_meta(&"bulwark_ignore", false):
		return false
	for child in node.get_children():
		if _is_object(child, root):
			return true
	return false


func _guid_for(node: Node) -> String:
	var g: String = node.get_meta(M_GUID, "")
	if not is_guid(g) or (_guids.has(g) and _guids[g] != node):
		# New, or a duplicate made by copying a node in the editor.
		g = random_guid()
		node.set_meta(M_GUID, g)
	_guids[g] = node
	return g


func _object_header(node: Node, name: String) -> Dictionary:
	var go := {}
	go["__guid"] = _guid_for(node)
	go["__version"] = 2
	var flags := int(node.get_meta(M_EXTRA, {}).get("Flags", 0))
	if node.get("editor_only"):
		flags |= FLAG_EDITOR_ONLY
	else:
		flags &= ~FLAG_EDITOR_ONLY
	go["Flags"] = flags & ~FLAG_ABSOLUTE
	go["Name"] = name
	return go


static func _frame(node: Node) -> Transform3D:
	return Convert.FACING if node is EntityScript or node is PrefabScript else Transform3D.IDENTITY


## A node's transform relative to its GameObject parent, in Bulwark space.
static func local_quake(node: Node3D, parent_frame: Transform3D) -> Transform3D:
	return parent_frame.affine_inverse() * Convert.to_quake(node.transform) * _frame(node)


func _put_transform(go: Dictionary, t: Transform3D) -> void:
	var s := Convert.transform_to_strings(t)
	go["Position"] = s.Position
	go["Rotation"] = s.Rotation
	go["Scale"] = s.Scale


func _export_object(node: Node3D, root: Node, parent_frame: Transform3D) -> Dictionary:
	if node is PrefabScript:
		return _export_instance(node, parent_frame)
	var go := _object_header(node, node.name)
	_put_transform(go, local_quake(node, parent_frame))
	var extra: Dictionary = node.get_meta(M_EXTRA, {})
	for k in extra:
		if not k in GO_KEYS:
			go[k] = extra[k]
	go["Enabled"] = node.enabled if node is EntityScript else node.get_meta(M_ENABLED, true)
	if node is EntityScript:
		go["Components"] = _components(node, node.classname, node.properties)
	else:
		go["Components"] = node.get_meta(M_COMPONENTS, []).duplicate(true)
	var children: Array = []
	var frame := _frame(node)
	for child in node.get_children():
		if child is Node3D and _is_object(child, root):
			children.append(_export_object(child, root, frame))
	go["Children"] = children
	return go


func _components(node: Node, classname: String, properties: Dictionary) -> Array:
	var list: Array = node.get_meta(M_COMPONENTS, []).duplicate(true)
	if classname.is_empty():
		return list
	var c := {}
	c["__type"] = ENTITY_TYPE
	var cg: String = node.get_meta(M_COMPONENT_GUID, "")
	if not is_guid(cg):
		cg = random_guid()
		node.set_meta(M_COMPONENT_GUID, cg)
	c["__guid"] = cg
	c["__enabled"] = true
	c["Flags"] = 0
	c["classname"] = classname
	var types: Dictionary = node.get_meta(M_TYPES, {})
	for key in properties:
		if key == "classname" or key.begins_with("__"):
			continue
		c[key] = _typed_value(str(properties[key]), str(types.get(key, "")))
	list.push_front(c)
	return list


## A spawn key value back in the JSON type it was imported with, as long as
## the text still reads as that type. "@<guid>" becomes a reference later.
static func _typed_value(text: String, type: String) -> Variant:
	match type:
		"number":
			if text.is_valid_float():
				return text.to_float() if text.contains(".") or text.contains("e") else text.to_int()
		"bool":
			if text == "1" or text == "0":
				return text == "1"
		"array":
			var parts := text.split(" ", false)
			var nums := []
			for p in parts:
				if not p.is_valid_float():
					return parts
				nums.append(p.to_float() if p.contains(".") else p.to_int())
			return nums
	return text


func _map_object(root: Node3D) -> Dictionary:
	var world := ""
	var spawn := true
	var collision := true
	if root is SceneScript:
		world = root.world
		spawn = root.spawn_map_entities
		collision = root.world_collision
	elif root.get("map_name") != null:
		# A Stiletto FTEWorld3D root: its world is the FTEW it exports, and its
		# own entities are in that world's entity lump.
		world = str(root.map_name) + ".ftew"
	if world.is_empty():
		return {}
	var saved: Dictionary = root.get_meta(M_MAP_OBJECT, {})
	var go := {}
	go["__guid"] = saved.get("__guid", random_guid())
	go["__version"] = 2
	go["Flags"] = saved.get("Flags", 0)
	go["Name"] = saved.get("Name", "Map")
	go["Enabled"] = true
	var c: Dictionary = saved.get("Component", {}).duplicate(true)
	if c.is_empty():
		c = {"__type": "Sandbox.MapInstance", "__guid": random_guid(), "__enabled": true, "Flags": 0}
	c["MapName"] = world
	c["EnableCollision"] = collision
	c["SpawnMapEntities"] = spawn
	go["Components"] = [c]
	go["Children"] = []
	if not saved.has("__guid"):
		root.set_meta(M_MAP_OBJECT, {"__guid": go.__guid, "Name": go.Name, "Component": c.duplicate()})
	return go


func _export_instance(node: Node3D, parent_frame: Transform3D) -> Dictionary:
	var go := {}
	go["__guid"] = _guid_for(node)
	go["__version"] = 2
	go["__Prefab"] = node.prefab
	var patch: Dictionary = node.get_meta(M_PATCH, {}).duplicate(true)
	for k in ["AddedObjects", "RemovedObjects", "PropertyOverrides", "MovedObjects"]:
		if not patch.has(k):
			patch[k] = []
	var root_guid := prefab_root_guid(node.prefab)
	if root_guid.is_empty():
		warnings.append("%s: prefab %s not found; its position is written as it was imported." % [node.name, node.prefab])
	else:
		var t := Convert.transform_to_strings(local_quake(node, parent_frame))
		var overrides: Array = []
		for o in patch.PropertyOverrides:
			var target: Dictionary = o.get("Target", {})
			if target.get("IdValue", "") == root_guid and o.get("Property", "") in ["Position", "Rotation", "Scale", "Name"]:
				continue
			overrides.append(o)
		var mine: Array = []
		for p in ["Position", "Rotation", "Scale"]:
			mine.append({"Target": {"Type": "GameObject", "IdValue": root_guid}, "Property": p, "Value": t[p]})
		mine.append({"Target": {"Type": "GameObject", "IdValue": root_guid}, "Property": "Name", "Value": String(node.name)})
		patch.PropertyOverrides = mine + overrides
	go["__PrefabInstancePatch"] = patch
	go["__PrefabIdToInstanceId"] = node.get_meta(M_ID_MAP, {}).duplicate()
	return go


static func prefab_root_guid(prefab: String) -> String:
	var data = read_json(Paths.game_file(prefab))
	if data is Dictionary and data.get("RootObject") is Dictionary:
		var root: Dictionary = data.RootObject
		return str(root.get("__guid", root.get("Id", "")))
	return ""


## Spawn key values written as "@<guid>" become s&box GameObject references.
func _resolve_references(v: Variant) -> void:
	if v is Dictionary:
		for k in v.keys():
			var x = v[k]
			if x is String and x.begins_with("@") and is_guid(x.substr(1)):
				var g: String = x.substr(1)
				if not _guids.has(g):
					warnings.append("Reference to %s, which is not in this scene." % g)
				v[k] = {"_type": "gameobject", "go": g}
			else:
				_resolve_references(x)
	elif v is Array:
		for x in v:
			_resolve_references(x)


# --- Import -------------------------------------------------------------------

## A node tree for a .scene or .prefab file. The caller owns the result.
func import_file(path: String) -> Node3D:
	errors.clear()
	warnings.clear()
	var data = read_json(path)
	if not data is Dictionary:
		errors.append("Not a scene or prefab: " + path)
		return null
	var root: Node3D = SceneScript.new()
	root.name = path.get_file().get_basename().to_pascal_case()
	root.scene_name = path.get_file().get_basename()
	var file: Dictionary = {}
	for k in data:
		if k != "GameObjects" and k != "RootObject":
			file[k] = data[k]
	root.set_meta(M_FILE, file)
	if data.has("RootObject"):
		root.kind = 1
		var go: Dictionary = data.RootObject
		_import_into(root, go, root)
		var entity := _entity_component(go)
		if not entity.is_empty():
			root.classname = str(entity.get("classname", ""))
			root.properties = _entity_keys(root, entity)
		for child in go.get("Children", []):
			_import_object(child, root, root, Transform3D.IDENTITY)
	else:
		var meta: Dictionary = data.get("SceneProperties", {}).get("Metadata", {})
		root.title = str(meta.get("Title", data.get("Title", ""))) if meta.get("Title", data.get("Title")) != null else ""
		for go in data.get("GameObjects", []):
			if go is Dictionary:
				if root.world.is_empty() and _take_map(root, go):
					continue
				_import_object(go, root, root, Transform3D.IDENTITY)
	_set_owner(root, root)
	return root


static func _set_owner(node: Node, owner: Node) -> void:
	for child in node.get_children():
		child.owner = owner
		_set_owner(child, owner)


## Takes a GameObject that only holds a MapInstance as the root's world.
func _take_map(root: Node3D, go: Dictionary) -> bool:
	var comps: Array = go.get("Components", [])
	if comps.size() != 1 or not go.get("Children", []).is_empty():
		return false
	var c: Dictionary = comps[0]
	if str(c.get("__type", "")).trim_prefix("Sandbox.") != "MapInstance":
		return false
	root.preview_world = false # set after world, so the preview loads once below
	root.world = str(c.get("MapName", ""))
	root.spawn_map_entities = bool(c.get("SpawnMapEntities", true))
	root.world_collision = bool(c.get("EnableCollision", true))
	root.set_meta(M_MAP_OBJECT, {"__guid": go.get("__guid", random_guid()), "Name": go.get("Name", "Map"),
		"Flags": go.get("Flags", 0), "Component": c})
	root.preview_world = true
	return true


static func _entity_component(go: Dictionary) -> Dictionary:
	for c in go.get("Components", []):
		if c is Dictionary and str(c.get("__type", "")) == ENTITY_TYPE and c.get("__enabled", true):
			return c
	return {}


## Spawn keys as strings, remembering each one's JSON type.
func _entity_keys(node: Node, c: Dictionary) -> Dictionary[String, String]:
	var keys: Dictionary[String, String] = {}
	var types := {}
	for k in c:
		if k.begins_with("__") or k == "Flags" or k == "classname" or k.begins_with("OnComponent"):
			continue
		var v = c[k]
		match typeof(v):
			TYPE_FLOAT:
				keys[k] = str(int(v)) if v == floorf(v) and absf(v) < 1e15 else Convert.num(v, 6)
				types[k] = "number"
			TYPE_BOOL:
				keys[k] = "1" if v else "0"
				types[k] = "bool"
			TYPE_ARRAY:
				var parts: PackedStringArray = []
				for x in v:
					parts.append(str(int(x)) if x is float and x == floorf(x) else str(x))
				keys[k] = " ".join(parts)
				types[k] = "array"
			TYPE_DICTIONARY:
				if v.has("go"):
					keys[k] = "@" + str(v.go)
			TYPE_NIL:
				pass
			_:
				keys[k] = str(v)
	if not types.is_empty():
		node.set_meta(M_TYPES, types)
	node.set_meta(M_COMPONENT_GUID, c.get("__guid", ""))
	return keys


## Copies the GameObject fields the editor doesn't show into node metadata.
func _import_into(node: Node, go: Dictionary, root: Node) -> void:
	node.set_meta(M_GUID, str(go.get("__guid", random_guid())))
	var extra := {}
	for k in go:
		if not k in GO_KEYS or k == "Flags":
			extra[k] = go[k]
	if not extra.is_empty():
		node.set_meta(M_EXTRA, extra)
	var others: Array = []
	var entity := _entity_component(go)
	for c in go.get("Components", []):
		if c != entity:
			others.append(c)
	if not others.is_empty():
		node.set_meta(M_COMPONENTS, others)


func _import_object(go: Dictionary, parent: Node, root: Node, parent_frame: Transform3D) -> void:
	if go.has("__Prefab"):
		_import_instance(go, parent, parent_frame)
		return
	var entity := _entity_component(go)
	var node: Node3D
	if entity.is_empty():
		node = Node3D.new()
		if not go.get("Enabled", true):
			node.set_meta(M_ENABLED, false)
	else:
		node = EntityScript.new()
		node.classname = str(entity.get("classname", ""))
		node.properties = _entity_keys(node, entity)
		node.enabled = bool(go.get("Enabled", true))
		node.editor_only = (int(go.get("Flags", 0)) & FLAG_EDITOR_ONLY) != 0
	_import_into(node, go, root)
	node.name = _safe_name(str(go.get("Name", "Object")))
	var t := Convert.transform_from_strings(str(go.get("Position", "")), str(go.get("Rotation", "")), str(go.get("Scale", "")))
	if int(go.get("Flags", 0)) & FLAG_ABSOLUTE:
		warnings.append("%s: absolute transforms are imported as local ones." % node.name)
	node.transform = Convert.from_quake(parent_frame * t * _frame(node).affine_inverse())
	parent.add_child(node, true)
	var frame := _frame(node)
	for child in go.get("Children", []):
		if child is Dictionary:
			_import_object(child, node, root, frame)


func _import_instance(go: Dictionary, parent: Node, parent_frame: Transform3D) -> void:
	var node: Node3D = PrefabScript.new()
	node.set_meta(M_GUID, str(go.get("__guid", random_guid())))
	var patch: Dictionary = go.get("__PrefabInstancePatch", {})
	node.set_meta(M_PATCH, patch)
	node.set_meta(M_ID_MAP, go.get("__PrefabIdToInstanceId", {}))
	var path := str(go.get("__Prefab", ""))
	var root_guid := prefab_root_guid(path)
	var file = read_json(Paths.game_file(path))
	var base := {}
	if file is Dictionary and file.get("RootObject") is Dictionary:
		base = file.RootObject
	var pos := str(base.get("Position", ""))
	var rot := str(base.get("Rotation", ""))
	var scl := str(base.get("Scale", ""))
	var name := str(base.get("Name", path.get_file().get_basename()))
	for o in patch.get("PropertyOverrides", []):
		if o is Dictionary and o.get("Target", {}).get("IdValue", "") == root_guid:
			match str(o.get("Property", "")):
				"Position": pos = str(o.Value)
				"Rotation": rot = str(o.Value)
				"Scale": scl = str(o.Value)
				"Name": name = str(o.Value)
	node.name = _safe_name(name)
	node.transform = Convert.from_quake(parent_frame * Convert.transform_from_strings(pos, rot, scl) * Convert.FACING.affine_inverse())
	node.prefab = path
	parent.add_child(node, true)


static func _safe_name(n: String) -> String:
	for c in [".", ":", "@", "/", "\"", "%"]:
		n = n.replace(c, "_")
	return n if not n.is_empty() else "Object"
