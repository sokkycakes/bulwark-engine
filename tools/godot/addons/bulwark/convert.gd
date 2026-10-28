@tool
extends RefCounted
## Converts between Godot space (Y up, metres) and Bulwark space (Z up, Quake
## units). The mapping is the one Stiletto's FTEW exporter uses, so entities
## placed here line up with world geometry exported from the same scene:
## (x, y, z) in Godot is (x, -z, y) * units_per_meter in Bulwark.

## Columns are where Godot's X, Y and Z axes land in Bulwark space.
const C := Basis(Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, -1, 0))

## Entities face Godot's -Z, which is Bulwark's +Y. Quake entities face their
## own +X, so an entity's frame is turned a quarter about Z.
const FACING := Transform3D(Basis(Vector3(0, 0, 1), PI / 2), Vector3.ZERO)


static func units() -> float:
	for setting in ["bulwark/units_per_meter", "stiletto/units_per_meter"]:
		if ProjectSettings.has_setting(setting):
			var u := float(ProjectSettings.get_setting(setting))
			if is_finite(u) and u > 0:
				return u
	return 32.0


static func point_to_quake(p: Vector3) -> Vector3:
	return C * p * units()


static func point_from_quake(p: Vector3) -> Vector3:
	return C.transposed() * p / units()


static func to_quake(t: Transform3D) -> Transform3D:
	return Transform3D(C * t.basis * C.transposed(), C * t.origin * units())


static func from_quake(t: Transform3D) -> Transform3D:
	return Transform3D(C.transposed() * t.basis * C, C.transposed() * t.origin / units())


## Quake angles (pitch yaw roll, pitch positive looking down) to a basis, in
## the order Bulwark's Scene_AnglesToQuat uses: yaw, then pitch, then roll.
static func basis_from_angles(a: Vector3) -> Basis:
	return Basis(Vector3(0, 0, 1), deg_to_rad(a.y)) * Basis(Vector3(0, 1, 0), deg_to_rad(a.x)) * Basis(Vector3(1, 0, 0), deg_to_rad(a.z))


static func angles_from_basis(b: Basis) -> Vector3:
	b = b.orthonormalized()
	var fwd := b.x
	var left := b.y
	var up := b.z
	var xy := Vector2(fwd.x, fwd.y).length()
	var a := Vector3(rad_to_deg(atan2(-fwd.z, xy)), 0, 0)
	if xy > 0.001:
		a.y = rad_to_deg(atan2(fwd.y, fwd.x))
		a.z = rad_to_deg(atan2(left.z, up.z))
	else:
		a.y = rad_to_deg(atan2(-left.x, left.y))
	for i in 3:
		if absf(a[i]) < 1e-4:
			a[i] = 0.0
	return a


## Shortest text for a float: up to `decimals` places, no trailing zeros.
static func num(v: float, decimals: int = 4) -> String:
	var s := String.num(snappedf(v, pow(10.0, -decimals)), decimals)
	if s.contains("."):
		s = s.rstrip("0").trim_suffix(".")
	if s == "-0":
		s = "0"
	return s


static func vec_text(v: Vector3, sep: String = ",", decimals: int = 4) -> String:
	return num(v.x, decimals) + sep + num(v.y, decimals) + sep + num(v.z, decimals)


static func parse_floats(text: String) -> PackedFloat64Array:
	var out := PackedFloat64Array()
	for part in text.replace(",", " ").split(" ", false):
		if not part.is_valid_float():
			return PackedFloat64Array()
		out.append(part.to_float())
	return out


static func parse_vec(text: String, fallback: Vector3) -> Vector3:
	var f := parse_floats(text)
	if f.size() == 3:
		return Vector3(f[0], f[1], f[2])
	if f.size() == 1:
		return Vector3(f[0], f[0], f[0])
	return fallback


## A GameObject transform (Position, Rotation, Scale strings) as a Transform3D.
## Rotation is a quaternion "x,y,z,w" or, in older files, angles "p,y,r".
static func transform_from_strings(position: String, rotation: String, scale: String) -> Transform3D:
	var basis := Basis.IDENTITY
	var r := parse_floats(rotation)
	if r.size() == 4:
		basis = Basis(Quaternion(r[0], r[1], r[2], r[3]).normalized())
	elif r.size() == 3:
		basis = basis_from_angles(Vector3(r[0], r[1], r[2]))
	return Transform3D(basis.scaled_local(parse_vec(scale, Vector3.ONE)), parse_vec(position, Vector3.ZERO))


static func transform_to_strings(t: Transform3D) -> Dictionary:
	var s := t.basis.get_scale()
	var q := t.basis.get_rotation_quaternion()
	if q.w < 0:
		q = -q
	return {
		"Position": vec_text(t.origin),
		"Rotation": num(q.x, 6) + "," + num(q.y, 6) + "," + num(q.z, 6) + "," + num(q.w, 6),
		"Scale": vec_text(s),
	}
