@tool
extends RefCounted
## Read-only previews of compiled worlds, so entities can be placed against
## the real map: FTEW worlds (docs/spec/ftew.md) and Q3-family BSPs (IBSP,
## RBSP, FBSP; docs/spec/bsp.md). Textures are found the way Bulwark finds
## them: <name>.mat's diffusemap, or an image with the material's name.

const Convert := preload("convert.gd")
const Paths := preload("paths.gd")

const SURF_SKY := 0x4
const SURF_NODRAW := 0x80

static var _textures: Dictionary = {}
static var _last_error := ""


static func last_error() -> String:
	return _last_error


## A Node3D holding the world's meshes, or null when it can't be read.
static func build(map_name: String) -> Node3D:
	_last_error = ""
	var path := Paths.map_file(map_name)
	var data := FileAccess.get_file_as_bytes(path)
	if data.is_empty():
		_last_error = "World not found: " + path
		return null
	var magic := data.slice(0, 4).get_string_from_ascii()
	var root: Node3D = null
	if magic == "FTEW":
		root = _ftew(data)
	elif magic in ["IBSP", "RBSP", "FBSP"]:
		root = _q3bsp(data, magic)
	else:
		_last_error = "No preview for this world format (%s)." % path.get_file()
	if root != null:
		root.name = "WorldPreview"
		root.set_meta(&"fte_ignore", true)
		root.set_meta(&"bulwark_ignore", true)
	return root


# --- FTEW ---------------------------------------------------------------------

static func _ftew(data: PackedByteArray) -> Node3D:
	if data.size() < 96 or data.decode_u32(4) != 1:
		_last_error = "Unsupported FTEW version."
		return null
	var dir := {}
	for i in 5:
		var at := 16 + i * 16
		dir[data.slice(at, at + 4).get_string_from_ascii()] = [data.decode_u32(at + 4), data.decode_u32(at + 8), data.decode_u32(at + 12)]
	for tag in ["MATL", "MESH", "INST"]:
		if not dir.has(tag) or dir[tag][0] + dir[tag][1] > data.size():
			_last_error = "Damaged FTEW: section " + tag
			return null
	var materials: PackedStringArray = []
	var at: int = dir.MATL[0]
	for i in dir.MATL[2]:
		var n := data.decode_u32(at)
		materials.append(data.slice(at + 4, at + 4 + n).get_string_from_ascii())
		at += 4 + n
	var meshes: Array = []
	at = dir.MESH[0]
	for i in dir.MESH[2]:
		var m := {"material": data.decode_u32(at), "vstart": at + 12, "vcount": data.decode_u32(at + 4), "icount": data.decode_u32(at + 8)}
		m.istart = m.vstart + m.vcount * 36
		at = m.istart + m.icount * 4
		meshes.append(m)
	# Entity models are stored relative to their entity's origin.
	var offsets := {}
	if dir.has("ENTS"):
		for ent in parse_entities(data.slice(dir.ENTS[0], dir.ENTS[0] + dir.ENTS[1]).get_string_from_utf8()):
			var model: String = ent.get("model", "")
			if model.begins_with("*"):
				offsets[model.substr(1).to_int()] = Convert.parse_vec(ent.get("origin", ""), Vector3.ZERO)
	var tools := {}
	at = dir.INST[0]
	for i in dir.INST[2]:
		var mesh_index := data.decode_u32(at)
		var model := data.decode_u32(at + 4)
		var flags := data.decode_u32(at + 8)
		var f := []
		for k in 12:
			f.append(data.decode_float(at + 12 + k * 4))
		at += 60
		if not flags & 1 or mesh_index >= meshes.size():
			continue
		var b := Basis(Vector3(f[0], f[1], f[2]), Vector3(f[3], f[4], f[5]), Vector3(f[6], f[7], f[8]))
		var t := Transform3D(b, Vector3(f[9], f[10], f[11]) + offsets.get(model, Vector3.ZERO))
		var m: Dictionary = meshes[mesh_index]
		var st: SurfaceTool = _tool(tools, materials[m.material] if m.material < materials.size() else "")
		var flip := b.determinant() < 0
		for k in range(0, m.icount - 2, 3):
			var tri := [data.decode_u32(m.istart + k * 4), data.decode_u32(m.istart + k * 4 + 4), data.decode_u32(m.istart + k * 4 + 8)]
			if flip:
				tri = [tri[0], tri[2], tri[1]]
			for v in tri:
				if v >= m.vcount:
					continue
				var o: int = m.vstart + v * 36
				var p := Vector3(data.decode_float(o), data.decode_float(o + 4), data.decode_float(o + 8))
				st.set_uv(Vector2(data.decode_float(o + 12), data.decode_float(o + 16)))
				st.set_color(Color(data.decode_float(o + 20), data.decode_float(o + 24), data.decode_float(o + 28), data.decode_float(o + 32)))
				st.add_vertex(Convert.point_from_quake(t * p))
	return _finish(tools)


# --- Q3-family BSP ------------------------------------------------------------

static func _q3bsp(data: PackedByteArray, magic: String) -> Node3D:
	var ibsp := magic == "IBSP"
	var lump := func(i: int) -> Array: return [data.decode_u32(8 + i * 8), data.decode_u32(12 + i * 8)]
	var vsize := 44 if ibsp else 80
	var color_at := 40 if ibsp else 64
	var ssize := 104 if ibsp else 148
	var shaders: Array = []
	var l: Array = lump.call(1)
	for i in l[1] / 72:
		var o: int = l[0] + i * 72
		shaders.append([data.slice(o, o + 64).get_string_from_ascii(), data.decode_u32(o + 64)])
	var verts: Array = lump.call(10)
	var indices: Array = lump.call(11)
	var surfs: Array = lump.call(13)
	var tools := {}
	for i in surfs[1] / ssize:
		var o: int = surfs[0] + i * ssize
		var shader := data.decode_u32(o)
		var type := data.decode_u32(o + 8)
		var fv := data.decode_u32(o + 12)
		var nv := data.decode_u32(o + 16)
		var fi := data.decode_u32(o + 20)
		var ni := data.decode_u32(o + 24)
		if shader >= shaders.size() or shaders[shader][1] & (SURF_NODRAW | SURF_SKY):
			continue
		var name: String = shaders[shader][0]
		if name.begins_with("common/") or name.begins_with("textures/common/"):
			continue
		var vert := func(v: int) -> Array:
			var a: int = verts[0] + (fv + v) * vsize
			return [Vector3(data.decode_float(a), data.decode_float(a + 4), data.decode_float(a + 8)),
				Vector2(data.decode_float(a + 12), data.decode_float(a + 16)),
				Color8(data[a + color_at], data[a + color_at + 1], data[a + color_at + 2], 255)]
		var st: SurfaceTool = _tool(tools, name)
		if type == 1 or type == 3:
			for k in ni:
				var v := data.decode_s32(indices[0] + (fi + k) * 4)
				if v < 0 or v >= nv:
					continue
				_add(st, vert.call(v))
		elif type == 2:
			var w := data.decode_s32(o + ssize - 8)
			var h := data.decode_s32(o + ssize - 4)
			if w >= 3 and h >= 3 and w * h <= nv:
				var grid: Array = []
				for v in w * h:
					grid.append(vert.call(v))
				_patch(st, grid, w, h)
	return _finish(tools)


static func _add(st: SurfaceTool, v: Array) -> void:
	st.set_uv(v[1])
	st.set_color(v[2])
	st.add_vertex(Convert.point_from_quake(v[0]))


## Tessellates a biquadratic Bezier patch: each 3x3 block of control points,
## 4 steps per side.
static func _patch(st: SurfaceTool, grid: Array, w: int, h: int) -> void:
	const STEPS := 4
	for by in range(0, h - 1, 2):
		for bx in range(0, w - 1, 2):
			var rows: Array = []
			for sy in STEPS + 1:
				var row: Array = []
				for sx in STEPS + 1:
					row.append(_bezier(grid, w, bx, by, float(sx) / STEPS, float(sy) / STEPS))
				rows.append(row)
			for sy in STEPS:
				for sx in STEPS:
					var a: Array = rows[sy][sx]
					var b: Array = rows[sy][sx + 1]
					var c: Array = rows[sy + 1][sx]
					var d: Array = rows[sy + 1][sx + 1]
					for v in [a, c, b, b, c, d]:
						_add(st, v)


static func _bezier(grid: Array, w: int, bx: int, by: int, u: float, v: float) -> Array:
	var bu := [(1 - u) * (1 - u), 2 * u * (1 - u), u * u]
	var bv := [(1 - v) * (1 - v), 2 * v * (1 - v), v * v]
	var p := Vector3.ZERO
	var t := Vector2.ZERO
	var c := Color(0, 0, 0, 0)
	for j in 3:
		for i in 3:
			var k: float = bu[i] * bv[j]
			var g: Array = grid[(by + j) * w + bx + i]
			p += g[0] * k
			t += g[1] * k
			c += g[2] * k
	return [p, t, c]


# --- Shared -------------------------------------------------------------------

static func _tool(tools: Dictionary, material: String) -> SurfaceTool:
	if not tools.has(material):
		var st := SurfaceTool.new()
		st.begin(Mesh.PRIMITIVE_TRIANGLES)
		tools[material] = st
	return tools[material]


static func _finish(tools: Dictionary) -> Node3D:
	var root := Node3D.new()
	var mesh := ArrayMesh.new()
	for material in tools:
		var st: SurfaceTool = tools[material]
		st.generate_normals()
		st.commit(mesh)
		mesh.surface_set_material(mesh.get_surface_count() - 1, material_for(material))
		mesh.surface_set_name(mesh.get_surface_count() - 1, material)
	var mi := MeshInstance3D.new()
	mi.name = "World"
	mi.mesh = mesh
	root.add_child(mi)
	return root


static func material_for(name: String) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.vertex_color_use_as_albedo = true
	# A preview: show both sides rather than trust every format's winding.
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	m.resource_name = name
	var tex := texture_for(name)
	if tex != null:
		m.albedo_texture = tex
		m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC
	else:
		m.albedo_color = Color.from_hsv(float(name.hash() % 1000) / 1000.0, 0.15, 0.85)
	return m


static func texture_for(name: String) -> Texture2D:
	if _textures.has(name):
		return _textures[name]
	var tex: Texture2D = null
	var candidates: PackedStringArray = []
	var mat := FileAccess.get_file_as_string(Paths.game_file(name + ".mat"))
	if not mat.is_empty():
		var toks := _tokens(mat)
		for i in toks.size() - 1:
			if toks[i].to_lower() in ["diffusemap", "map", "clampmap"] and not toks[i + 1].begins_with("$"):
				candidates.append(toks[i + 1])
				break
	candidates.append(name)
	for c in candidates:
		var base := c.get_basename() if c.get_extension().to_lower() in ["tga", "png", "jpg", "jpeg", "bmp", "webp"] else c
		for ext in ["png", "tga", "jpg", "jpeg", "bmp", "webp"]:
			var file := Paths.game_file(base + "." + ext)
			if FileAccess.file_exists(file):
				var img := Image.load_from_file(file)
				if img != null and not img.is_empty():
					img.generate_mipmaps()
					tex = ImageTexture.create_from_image(img)
					break
		if tex != null:
			break
	_textures[name] = tex
	return tex


static func _tokens(text: String) -> PackedStringArray:
	var out: PackedStringArray = []
	for line in text.split("\n"):
		var cut := line.find("//")
		if cut >= 0:
			line = line.substr(0, cut)
		for t in line.replace("\t", " ").split(" ", false):
			if t != "{" and t != "}":
				out.append(t)
	return out


## Entity lump text as an Array of key/value Dictionaries.
static func parse_entities(text: String) -> Array:
	var out: Array = []
	var cur: Dictionary = {}
	var key := ""
	var have_key := false
	var i := 0
	var n := text.length()
	while i < n:
		var c := text[i]
		if c == "{":
			cur = {}
			have_key = false
		elif c == "}":
			out.append(cur)
		elif c == "\"":
			var e := text.find("\"", i + 1)
			if e < 0:
				break
			var s := text.substr(i + 1, e - i - 1)
			if have_key:
				cur[key] = s
			else:
				key = s
			have_key = not have_key
			i = e
		i += 1
	return out
