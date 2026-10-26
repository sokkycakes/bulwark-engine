@tool
extends RefCounted
## Entity definitions for the browser and inspector. Reads three formats:
##
## - Radiant QUAKED comments (*.def), which Nuclide generates from its source
##   (Tools/base_entities.def): `/*QUAKED name (r g b) (mins) (maxs) FLAGS`
##   or `(r g b) ? FLAGS` for brush entities, then Markdown sections.
## - Valve FGD (*.fgd): @BaseClass, @PointClass, @SolidClass with base(),
##   size(), color(), studio() and typed keyvalues, choices and flags.
## - Nuclide entityDef decls (base/decls/def/*.def): `entityDef name { "k" "v" }`,
##   which name a "spawnclass" and the defaults they set.
##
## Each class is a Dictionary:
##   name, kind ("point" | "solid"), color (Color), mins, maxs (Vector3),
##   description, model, group, source (file),
##   keys: Array of { name, type, label, default, description, choices: [[value, label]] },
##   flags: Array of { bit, name, description, default },
##   inputs, outputs: Array of { name, description }.

var classes: Dictionary = {}
var errors: PackedStringArray = []
var _fgd_bases: Dictionary = {}


func clear() -> void:
	classes.clear()
	errors.clear()
	_fgd_bases.clear()


func names() -> PackedStringArray:
	var n := PackedStringArray(classes.keys())
	n.sort()
	return n


func get_class_def(name: String) -> Dictionary:
	return classes.get(name, {})


func find_key(def: Dictionary, key: String) -> Dictionary:
	for k in def.get("keys", []):
		if k.name == key:
			return k
	return {}


## Loads every definition file under a directory (or a single file).
func load_path(path: String) -> int:
	var count := classes.size()
	if DirAccess.dir_exists_absolute(path):
		_load_dir(path)
	elif FileAccess.file_exists(path):
		load_file(path)
	else:
		errors.append("Entity definitions not found: " + path)
	return classes.size() - count


func _load_dir(dir: String) -> void:
	var files := DirAccess.get_files_at(dir)
	files.sort()
	for f in files:
		if f.get_extension().to_lower() in ["def", "fgd"]:
			load_file(dir.path_join(f))
	var dirs := DirAccess.get_directories_at(dir)
	dirs.sort()
	for d in dirs:
		_load_dir(dir.path_join(d))


func load_file(path: String) -> void:
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty() and FileAccess.get_open_error() != OK:
		errors.append("Cannot read " + path)
		return
	if path.get_extension().to_lower() == "fgd":
		parse_fgd(text, path)
	else:
		if text.contains("/*QUAKED"):
			parse_quaked(text, path)
		if text.contains("entityDef"):
			parse_entitydefs(text, path)


static func new_class(name: String, source: String) -> Dictionary:
	return {
		"name": name, "kind": "point", "color": Color(1, 0.5, 0), "mins": Vector3(-8, -8, -8),
		"maxs": Vector3(8, 8, 8), "description": "", "model": "", "group": group_of(name),
		"source": source, "keys": [], "flags": [], "inputs": [], "outputs": [],
	}


## The part of a classname before its first underscore: func_door is "func".
static func group_of(name: String) -> String:
	var i := name.find("_")
	return name.substr(0, i) if i > 0 else name


func _put(def: Dictionary) -> void:
	var old: Dictionary = classes.get(def.name, {})
	if old.is_empty():
		classes[def.name] = def
		return
	# A class described twice (QUAKED and FGD, say) keeps the richer details.
	for key in ["description", "model"]:
		if str(old[key]).is_empty():
			old[key] = def[key]
	for list in ["keys", "flags", "inputs", "outputs"]:
		for item in def[list]:
			var have := false
			for o in old[list]:
				if o.name == item.name:
					have = true
					break
			if not have:
				old[list].append(item)


# --- QUAKED -----------------------------------------------------------------

func parse_quaked(text: String, source: String) -> void:
	var at := 0
	while true:
		var start := text.find("/*QUAKED", at)
		if start < 0:
			break
		var end := text.find("*/", start)
		if end < 0:
			end = text.length()
		_quaked_block(text.substr(start + 8, end - start - 8), source)
		at = end + 2


func _quaked_block(block: String, source: String) -> void:
	var lines := block.split("\n")
	var header := lines[0].strip_edges()
	var name := header.get_slice(" ", 0)
	if name.is_empty():
		return
	var def := new_class(name, source)
	var rest := header.substr(name.length()).strip_edges()
	var groups := _paren_groups(rest)
	if groups.size() > 0:
		var c := _floats(groups[0].text)
		if c.size() == 3:
			def.color = Color(c[0], c[1], c[2])
		rest = rest.substr(groups[0].end).strip_edges()
	var flagnames: PackedStringArray = []
	if rest.begins_with("?"):
		def.kind = "solid"
		flagnames = rest.substr(1).split(" ", false)
	else:
		groups = _paren_groups(rest)
		if groups.size() >= 2:
			var mn := _floats(groups[0].text)
			var mx := _floats(groups[1].text)
			if mn.size() == 3 and mx.size() == 3:
				def.mins = Vector3(mn[0], mn[1], mn[2])
				def.maxs = Vector3(mx[0], mx[1], mx[2])
			rest = rest.substr(groups[1].end).strip_edges()
		if rest.begins_with("?"):
			rest = rest.substr(1)
		flagnames = rest.split(" ", false)
	for i in flagnames.size():
		if flagnames[i] != "x" and flagnames[i] != "-":
			def.flags.append({"bit": 1 << i, "name": flagnames[i], "description": "", "default": false})
	var section := ""
	var overview: PackedStringArray = []
	for i in range(1, lines.size()):
		var line := lines[i].strip_edges(false, true)
		if line.begins_with("# "):
			section = line.substr(2).get_slice(" ", 0).to_upper()
			continue
		if line.begins_with("@ingroup"):
			if line.contains("brushentity"):
				def.kind = "solid"
			continue
		match section:
			"OVERVIEW":
				overview.append(line)
			"KEYS":
				var item := _quaked_item(line)
				if not item.is_empty():
					def.keys.append({"name": item.name, "type": guess_type(item.name, item.description),
						"label": item.name, "default": "", "description": item.description, "choices": []})
			"SPAWNFLAGS":
				var item := _quaked_item(line)
				if not item.is_empty():
					_quaked_flag(def, item)
			"INPUTS", "OUTPUTS":
				var item := _quaked_item(line)
				if not item.is_empty():
					def[section.to_lower()].append({"name": item.name, "description": item.description})
	def.description = "\n".join(overview).strip_edges()
	_put(def)


## `- "key" : text` or `- NAME (8) : text`.
static func _quaked_item(line: String) -> Dictionary:
	line = line.strip_edges()
	if not line.begins_with("- "):
		return {}
	line = line.substr(2)
	var colon := line.find(":")
	var head := line.substr(0, colon).strip_edges() if colon >= 0 else line
	var desc := line.substr(colon + 1).strip_edges() if colon >= 0 else ""
	return {"name": head.replace("\"", "").strip_edges(), "description": desc}


static func _quaked_flag(def: Dictionary, item: Dictionary) -> void:
	var name: String = item.name
	var bit := 0
	var open := name.find("(")
	if open >= 0:
		bit = int(name.substr(open + 1, name.find(")", open) - open - 1))
		name = name.substr(0, open).strip_edges()
	for f in def.flags:
		if f.name == name or (bit != 0 and f.bit == bit):
			f.description = item.description
			if bit != 0:
				f.bit = bit
			return
	if bit != 0:
		def.flags.append({"bit": bit, "name": name, "description": item.description, "default": false})


## Key types guessed from a name and its description, for keys whose format
## doesn't say.
static func guess_type(key: String, desc: String) -> String:
	var d := desc.to_lower()
	if key in ["target", "killtarget", "netname", "parentname", "message"] and d.contains("target"):
		return "target_destination"
	if key in ["target", "killtarget", "parentname"]:
		return "target_destination"
	if key == "targetname":
		return "target_source"
	if key in ["model", "studio"]:
		return "studio"
	if key in ["rendercolor", "_color", "color", "_light"] or d.contains("color"):
		return "color255"
	if key == "angles" or key == "angle":
		return "angle"
	if d.contains("sound shader") or key.begins_with("snd_") or d.contains("sample"):
		return "sound"
	if d.contains("in seconds") or d.contains("speed") or d.contains("time") or d.contains("delay") or d.contains("radius"):
		return "float"
	return "string"


# --- FGD ----------------------------------------------------------------------

func parse_fgd(text: String, source: String) -> void:
	var t := _FgdTokens.new(text)
	while not t.done():
		var tok := t.next()
		if not tok.begins_with("@"):
			continue
		var cls := tok.substr(1).to_lower()
		if cls == "include":
			var inc := t.next()
			var path := source.get_base_dir().path_join(inc.trim_prefix("\"").trim_suffix("\""))
			if FileAccess.file_exists(path):
				parse_fgd(FileAccess.get_file_as_string(path), path)
			continue
		if not cls.ends_with("class"):
			_skip_block(t)
			continue
		var def := new_class("", source)
		def.kind = "solid" if cls == "solidclass" else "point"
		var bases: PackedStringArray = []
		# Helpers up to "=": base(...), size(...), color(...), studio(...)
		while not t.done() and t.peek() != "=":
			var helper := t.next().to_lower()
			if t.peek() != "(":
				continue
			var args := t.group()
			match helper:
				"base":
					for b in args.split(",", false):
						bases.append(b.strip_edges())
				"size":
					var parts := args.split(",")
					var a := _floats(parts[0])
					if parts.size() == 2:
						var b := _floats(parts[1])
						if a.size() == 3 and b.size() == 3:
							def.mins = Vector3(a[0], a[1], a[2])
							def.maxs = Vector3(b[0], b[1], b[2])
					elif a.size() == 3:
						def.maxs = Vector3(a[0], a[1], a[2]) / 2
						def.mins = -def.maxs
				"color":
					var c := _floats(args)
					if c.size() == 3:
						def.color = Color8(int(c[0]), int(c[1]), int(c[2]))
				"studio", "studioprop", "model":
					def.model = args.strip_edges().trim_prefix("\"").trim_suffix("\"")
		t.next() # "="
		def.name = t.next()
		if t.peek() == ":":
			t.next()
			def.description = _fgd_string(t)
		var keys: Array = []
		if t.peek() == "[":
			t.next()
			_fgd_members(t, def, keys)
		def.keys = keys
		# Inherited members come first, in base order; the class's own
		# members replace inherited ones of the same name.
		for list in ["keys", "flags", "inputs", "outputs"]:
			var merged: Array = []
			for b in bases:
				var base: Dictionary = _fgd_bases.get(b, classes.get(b, {}))
				for item in base.get(list, []):
					merged = merged.filter(func(m): return m.name != item.name)
					merged.append(item.duplicate(true))
			for item in def[list]:
				merged = merged.filter(func(m): return m.name != item.name)
				merged.append(item)
			def[list] = merged
		for b in bases:
			var base: Dictionary = _fgd_bases.get(b, classes.get(b, {}))
			if def.model.is_empty() and not base.is_empty():
				def.model = base.model
		if cls == "baseclass":
			_fgd_bases[def.name] = def
		else:
			_put(def)


func _fgd_members(t: _FgdTokens, def: Dictionary, keys: Array) -> void:
	while not t.done():
		var tok := t.next()
		if tok == "]":
			return
		if tok == "input" or tok == "output":
			var name := t.next()
			var args := t.group() if t.peek() == "(" else ""
			var desc := ""
			if t.peek() == ":":
				t.next()
				desc = _fgd_string(t)
			def[tok + "s"].append({"name": name, "description": desc, "type": args})
			continue
		if t.peek() != "(":
			continue
		var type := t.group().strip_edges().to_lower()
		var key := {"name": tok, "type": type, "label": tok, "default": "", "description": "", "choices": []}
		# name(type) [readonly] : "label" : default : "description" [= [...]]
		if t.peek() == "readonly" or t.peek() == "report":
			t.next()
		var fields: Array = []
		while t.peek() == ":":
			t.next()
			if t.peek() == ":" or t.peek() == "=":
				fields.append("")
			else:
				fields.append(_fgd_string(t))
		if fields.size() > 0:
			key.label = fields[0]
		if fields.size() > 1:
			key.default = fields[1]
		if fields.size() > 2:
			key.description = fields[2]
		if t.peek() == "=":
			t.next()
			if t.next() == "[":
				while not t.done() and t.peek() != "]":
					var value := t.next().trim_prefix("\"").trim_suffix("\"")
					var label := ""
					var on := ""
					if t.peek() == ":":
						t.next()
						label = _fgd_string(t)
					if t.peek() == ":":
						t.next()
						on = t.next()
					if type == "flags":
						def.flags.append({"bit": int(value), "name": label, "description": "", "default": on == "1"})
					else:
						key.choices.append([value, label])
				t.next()
		if type != "flags":
			keys.append(key)


## A quoted string, possibly joined across lines with "+".
static func _fgd_string(t: _FgdTokens) -> String:
	var s := t.next().trim_prefix("\"").trim_suffix("\"")
	while t.peek() == "+":
		t.next()
		s += t.next().trim_prefix("\"").trim_suffix("\"")
	return s


static func _skip_block(t: _FgdTokens) -> void:
	while not t.done() and t.peek() != "[" and not t.peek().begins_with("@"):
		t.next()
	if t.peek() != "[":
		return
	var depth := 0
	while not t.done():
		var tok := t.next()
		if tok == "[":
			depth += 1
		elif tok == "]":
			depth -= 1
			if depth == 0:
				return


class _FgdTokens:
	var toks: PackedStringArray = []
	var i := 0

	func _init(text: String) -> void:
		var n := text.length()
		var p := 0
		while p < n:
			var c := text[p]
			if c == "/" and p + 1 < n and text[p + 1] == "/":
				while p < n and text[p] != "\n":
					p += 1
			elif c in " \t\r\n":
				p += 1
			elif c == "\"":
				var e := text.find("\"", p + 1)
				if e < 0:
					e = n - 1
				toks.append(text.substr(p, e - p + 1))
				p = e + 1
			elif c in "[]():=,+":
				toks.append(c)
				p += 1
			else:
				var s := p
				while p < n and not (text[p] in " \t\r\n\"[]():=,+"):
					p += 1
				toks.append(text.substr(s, p - s))

	func done() -> bool:
		return i >= toks.size()

	func peek() -> String:
		return toks[i] if i < toks.size() else ""

	func next() -> String:
		i += 1
		return toks[i - 1] if i <= toks.size() else ""

	## The raw text of a parenthesised group, which must be next.
	func group() -> String:
		next()
		var parts: PackedStringArray = []
		var depth := 1
		while not done():
			var tok := next()
			if tok == "(":
				depth += 1
			elif tok == ")":
				depth -= 1
				if depth == 0:
					break
			parts.append(tok)
		return " ".join(parts).replace(" , ", ",")


# --- entityDef --------------------------------------------------------------

func parse_entitydefs(text: String, source: String) -> void:
	var at := 0
	while true:
		var start := text.find("entityDef", at)
		if start < 0:
			break
		var open := text.find("{", start)
		var close := text.find("}", open)
		if open < 0 or close < 0:
			break
		var name := text.substr(start + 9, open - start - 9).strip_edges()
		at = close + 1
		if name.is_empty() or name.contains(" "):
			continue
		var pairs := _quoted(text.substr(open + 1, close - open - 1))
		var def := new_class(name, source)
		def.group = "decl"
		var spawnclass := ""
		for p in range(0, pairs.size() - 1, 2):
			var k: String = pairs[p]
			var v: String = pairs[p + 1]
			match k:
				"spawnclass":
					spawnclass = v
				"model":
					def.model = v
				"mins", "maxs":
					var f := _floats(v)
					if f.size() == 3:
						def[k] = Vector3(f[0], f[1], f[2])
				"editor_color":
					var f := _floats(v)
					if f.size() == 3:
						def.color = Color(f[0], f[1], f[2])
				"editor_usage":
					def.description = v
			if not k.begins_with("editor_") and k != "spawnclass" and k != "inherit":
				def.keys.append({"name": k, "type": guess_type(k, ""), "label": k, "default": v, "description": "", "choices": []})
		if def.description.is_empty():
			def.description = "Declared in %s%s." % [source.get_file(), (", spawns " + spawnclass) if not spawnclass.is_empty() else ""]
		_put(def)


static func _quoted(text: String) -> PackedStringArray:
	var out: PackedStringArray = []
	var at := 0
	while true:
		var a := text.find("\"", at)
		if a < 0:
			break
		var b := text.find("\"", a + 1)
		if b < 0:
			break
		out.append(text.substr(a + 1, b - a - 1))
		at = b + 1
	return out


static func _floats(text: String) -> PackedFloat64Array:
	var out := PackedFloat64Array()
	for part in text.replace(",", " ").split(" ", false):
		if part.is_valid_float():
			out.append(part.to_float())
	return out


## (text) groups in order, with the index just past each closing paren.
static func _paren_groups(text: String) -> Array:
	var out: Array = []
	var at := 0
	while true:
		var a := text.find("(", at)
		if a < 0:
			break
		if not text.substr(at, a - at).strip_edges().is_empty():
			break
		var b := text.find(")", a)
		if b < 0:
			break
		out.append({"text": text.substr(a + 1, b - a - 1), "end": b + 1})
		at = b + 1
	return out
