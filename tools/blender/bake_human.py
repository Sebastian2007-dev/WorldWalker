# Blender background script: bakes the MakeHuman base mesh (via the MPFB extension) into WorldWalker's morphable
# human: base mesh, morph targets (detail sliders and macro combinations), Mixamo skeleton definition, skin weights
# and fitted assets (eyes, eyebrows, eyelashes, hair, clothes) from MakeHuman asset packs.
#
#   blender -b --python bake_human.py -- <asset_pack_dir> <out_dir>
#
# Everything is in the engine's model space: metres, z up, the character looks along +y, feet near z = 0.
# Outputs (in <out_dir>):
#   human.json       metadata (bones, sliders, macro config, assets, skins)
#   base.bin         base positions, body render vertices, body triangles, skin weights (see write_base)
#   targets.bin      sparse morph targets (see write_targets)
#   asset_<name>.bin fitted asset meshes (see write_asset)
#   tex/*.png        textures
import bpy, sys, os, json, gzip, struct, glob, re
import numpy as np
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
ASSET_DIR, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
os.makedirs(os.path.join(OUT, "tex"), exist_ok=True)

from bl_ext.user_default.mpfb.services.objectservice import ObjectService
from bl_ext.user_default.mpfb.services.locationservice import LocationService
from bl_ext.user_default.mpfb.services.humanservice import HumanService

DATA = LocationService.get_mpfb_data("")
TARGETS = os.path.join(DATA, "targets")
SCALE = 0.1                 # MakeHuman units are decimetres
FACE = np.array([-1.0, -1.0, 1.0])  # MPFB humans look along -y; rotate 180 degrees about z
MAX_TEX = 2048


def log(*a):
    print("[human]", *a, flush=True)


def mh_to_model(v):
    """MakeHuman coordinates (decimetres, y up, looking along +z) -> model space."""
    v = np.asarray(v, np.float64).reshape(-1, 3)
    return np.stack([v[..., 0], -v[..., 2], v[..., 1]], axis=-1) * SCALE * FACE


# ---------------------------------------------------------------- base mesh

bpy.ops.wm.read_factory_settings(use_empty=True)
basemesh = ObjectService.load_base_mesh(context=bpy.context, scale_factor=SCALE, load_vertex_groups=True)
me = basemesh.data
N = len(me.vertices)
co = np.empty(N * 3, np.float32)
me.vertices.foreach_get("co", co)
co = co.reshape(-1, 3) * FACE                        # Blender space after MPFB import -> model space
GROUND = float(co[np.array([v.index for v in me.vertices if any(me.vertices[v.index].groups)], int)][:, 2].min())
log("base mesh", N, "vertices")


def group_vertices(name):
    gi = basemesh.vertex_groups[name].index
    return [v.index for v in me.vertices if any(g.group == gi for g in v.groups)]


body = np.zeros(N, bool)
body[group_vertices("body")] = True


def check_target_space():
    """The engine parses .target files itself; make sure the MakeHuman -> model mapping matches MPFB's loader."""
    from bl_ext.user_default.mpfb.services.targetservice import TargetService
    path = os.path.join(TARGETS, "nose", "nose-trans-up.target.gz")
    TargetService.load_target(basemesh, path, weight=1.0, name="probe")
    kb = me.shape_keys.key_blocks["probe"]
    shaped = np.empty(N * 3, np.float32)
    kb.data.foreach_get("co", shaped)
    mpfb_delta = (shaped.reshape(-1, 3) * FACE) - co
    idx, ours = read_target(path)
    err = np.abs(mpfb_delta[idx] - ours).max()
    log("target space check: max error", err)
    assert err < 1e-4, "target coordinate mapping differs from MPFB"
    basemesh.shape_key_clear()


def read_target(path):
    """MakeHuman .target(.gz): lines 'index dx dy dz' in MakeHuman space."""
    opener = gzip.open if path.endswith(".gz") else open
    idx, d = [], []
    with opener(path, "rt") as f:
        for line in f:
            w = line.split()
            if len(w) == 4 and not line.startswith("#"):
                idx.append(int(w[0]))
                d.append((float(w[1]), float(w[2]), float(w[3])))
    return np.asarray(idx, np.int64), mh_to_model(np.asarray(d, np.float64)).astype(np.float32)


check_target_space()

# ---------------------------------------------------------------- morph targets

targets = []      # (name, indices, deltas)
target_index = {}


def add_target(name, paths):
    """One engine target; several files (left + right side) are summed."""
    acc = {}
    for p in paths:
        idx, d = read_target(p)
        for i, v in zip(idx, d):
            acc[i] = acc.get(i, 0) + v
    idx = np.array(sorted(acc), np.uint32)
    d = np.array([acc[i] for i in idx], np.float32).reshape(-1, 3)
    keep = np.abs(d).max(axis=1) > 1e-6 if len(d) else np.zeros(0, bool)
    target_index[name] = len(targets)
    targets.append((name, idx[keep], d[keep]))
    return target_index[name]


def target_file(group, name):
    for cand in (os.path.join(TARGETS, group, name + ".target.gz"), os.path.join(TARGETS, group, name + ".target")):
        if os.path.isfile(cand):
            return cand
    hits = glob.glob(os.path.join(TARGETS, "**", name + ".target*"), recursive=True)
    return hits[0] if hits else None


# Detail sliders: every symmetric modifier of the body and face groups; left/right sides move together.
SKIP_GROUPS = {"asym", "genitals", "expression", "measure"}
SKIP_NAMES = ("nipple", "bulge")
sliders = []
catalog = json.load(open(os.path.join(TARGETS, "target.json")))
for group, info in catalog.items():
    if group in SKIP_GROUPS:
        continue
    for cat in info["categories"]:
        if any(s in cat["name"] for s in SKIP_NAMES):
            continue
        op = cat.get("opposites")
        if not op:  # one-sided slider (head shapes like "head-oval"): 0..1
            files = [f for f in (target_file(group, n) for n in cat["targets"]) if f]
            if files:
                sliders.append({"group": group, "name": cat["name"], "neg": -1,
                                "pos": add_target(group + "/" + cat["name"] + "+", files)})
            continue
        side = []
        for sign in ("negative", "positive"):
            names = [op.get(sign + "-unsided")] if op.get(sign + "-unsided") else \
                    [op.get(sign + "-left"), op.get(sign + "-right")]
            files = [target_file(group, n) for n in names if n]
            files = [f for f in files if f]
            side.append(add_target(group + "/" + cat["name"] + ("-" if sign == "negative" else "+"), files) if files else -1)
        if side == [-1, -1]:
            continue
        sliders.append({"group": group, "name": cat["name"], "neg": side[0], "pos": side[1]})
log(len(sliders), "detail sliders")

# Macro targets: every file the engine's macro formula can ask for (no babies; ages child..old).
macro_names = []
for path in sorted(glob.glob(os.path.join(TARGETS, "macrodetails", "**", "*.target*"), recursive=True)) + \
        sorted(glob.glob(os.path.join(TARGETS, "breast", "*-*-*-*-*-*.target*"))):
    rel = os.path.relpath(path, TARGETS).replace("\\", "/")
    rel = re.sub(r"\.target(\.gz)?$", "", rel)
    if "baby" in rel:
        continue
    add_target(rel, [path])
    macro_names.append(rel)
log(len(macro_names), "macro targets,", sum(len(t[1]) for t in targets), "deltas in total")

macro_config = json.load(open(os.path.join(TARGETS, "macrodetails", "macro.json")))["macrotargets"]


def write_targets():
    # targets.bin: u32 count; per target: u32 n, n x u32 vertex index, n x float3 delta
    with open(os.path.join(OUT, "targets.bin"), "wb") as f:
        f.write(struct.pack("<I", len(targets)))
        for _, idx, d in targets:
            f.write(struct.pack("<I", len(idx)))
            f.write(np.asarray(idx, np.uint32).tobytes())
            f.write(np.asarray(d, np.float32).tobytes())


# ---------------------------------------------------------------- skeleton

rig_json = json.load(open(os.path.join(DATA, "rigs", "standard", "rig.mixamo.json")))
armature = HumanService.add_builtin_rig(basemesh, "mixamo", import_weights=True)
arm_to_model = Matrix.Diagonal((*FACE, 1.0)) @ armature.matrix_world


def bare(name):
    return name.split(":")[-1]


def joint_def(info):
    """Vertex indices whose mean is the joint (MakeHuman 'CUBE' joints are vertex groups of the base mesh)."""
    if info["strategy"] == "MEAN":
        verts = info["vertex_indices"]
    elif info["strategy"] == "CUBE":
        verts = group_vertices(info["cube_name"])
    else:
        raise ValueError("unsupported joint strategy " + info["strategy"])
    if "offset" in info:
        log("  ignoring joint offset", info.get("offset"))
    return [int(v) for v in verts]


order, seen = [], set()


def visit(b):
    order.append(b)
    for c in b.children:
        visit(c)


for b in armature.data.bones:
    if b.parent is None:
        visit(b)
bone_index = {b.name: i for i, b in enumerate(order)}
bones = []
for b in order:
    m = arm_to_model @ b.matrix_local
    q = m.to_quaternion().normalized()
    info = rig_json["bones"][b.name]
    bones.append({"name": bare(b.name), "parent": bone_index[b.parent.name] if b.parent else -1,
                  "head": joint_def(info["head"]), "tail": joint_def(info["tail"]),
                  "rot": [q.x, q.y, q.z, q.w],
                  "neutralHead": list(m.translation), "neutralTail": list(arm_to_model @ b.tail_local)})
log(len(bones), "bones")

# Skin weights per base vertex: four strongest bones.
weights = np.zeros((N, 4), np.float32)
joints = np.zeros((N, 4), np.uint8)
group_bone = {g.index: bone_index.get(g.name, -1) for g in basemesh.vertex_groups}
for v in me.vertices:
    ws = sorted(((g.weight, group_bone.get(g.group, -1)) for g in v.groups if group_bone.get(g.group, -1) >= 0),
                reverse=True)[:4]
    total = sum(w for w, _ in ws)
    for k, (w, bi) in enumerate(ws):
        weights[v.index, k] = w / total if total > 0 else 0
        joints[v.index, k] = bi


# ---------------------------------------------------------------- body render mesh

def render_mesh(obj_mesh, face_ok):
    """Triangles of the faces passing face_ok, with vertices split where UVs differ. Returns (base index, uv) per
    render vertex and the triangle list."""
    uvl = obj_mesh.uv_layers.active.data
    key_to_rv, rverts, tris = {}, [], []
    for poly in obj_mesh.polygons:
        if not face_ok(poly):
            continue
        corners = []
        for li in poly.loop_indices:
            vi = obj_mesh.loops[li].vertex_index
            uv = uvl[li].uv
            key = (vi, round(uv[0], 6), round(uv[1], 6))
            rv = key_to_rv.get(key)
            if rv is None:
                rv = key_to_rv[key] = len(rverts)
                rverts.append((vi, uv[0], 1.0 - uv[1]))
            corners.append(rv)
        for k in range(1, len(corners) - 1):
            # Model space mirrors x and y (a rotation): winding stays the same.
            tris.append((corners[0], corners[k], corners[k + 1]))
    return rverts, tris


body_rverts, body_tris = render_mesh(me, lambda p: all(body[v] for v in p.vertices))
log("body:", len(body_rverts), "render vertices,", len(body_tris), "triangles")


def write_base():
    # base.bin: u32 N, N x float3 position; u32 R, R x (u32 base vertex, float u, float v);
    #           u32 T, T x u32x3; N x u8x4 joints, N x u8x4 weights (unorm)
    with open(os.path.join(OUT, "base.bin"), "wb") as f:
        f.write(struct.pack("<I", N))
        f.write(co.astype(np.float32).tobytes())
        f.write(struct.pack("<I", len(body_rverts)))
        for vi, u, v in body_rverts:
            f.write(struct.pack("<Iff", vi, u, v))
        f.write(struct.pack("<I", len(body_tris)))
        f.write(np.asarray(body_tris, np.uint32).tobytes())
        f.write(joints.tobytes())
        q = np.round(weights * 255).astype(np.int32)
        q[:, 0] += 255 - q.sum(axis=1)
        q[weights.sum(axis=1) == 0] = 0
        f.write(np.clip(q, 0, 255).astype(np.uint8).tobytes())


# ---------------------------------------------------------------- assets (MakeHuman asset packs)

def copy_texture(path, name):
    """Copies a texture into tex/ as PNG, at most MAX_TEX pixels wide. Returns the file name relative to OUT."""
    if not path or not os.path.isfile(path):
        return None
    img = bpy.data.images.load(path, check_existing=False)
    w, h = img.size
    if max(w, h) > MAX_TEX:
        s = MAX_TEX / max(w, h)
        img.scale(max(1, int(w * s)), max(1, int(h * s)))
    out = os.path.join("tex", name + ".png")
    img.filepath_raw = os.path.join(OUT, out)
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)
    return out.replace("\\", "/")


def parse_mhmat(path):
    mat = {}
    folder = os.path.dirname(path)
    for line in open(path, encoding="utf-8", errors="replace"):
        w = line.split()
        if len(w) >= 2 and w[0] in ("diffuseTexture", "normalmapTexture", "transparencyMapTexture"):
            p = os.path.join(folder, w[1])
            if not os.path.isfile(p):
                p = os.path.join(ASSET_DIR, w[1])
            mat[w[0]] = p
        elif len(w) == 4 and w[0] == "diffuseColor":
            mat["diffuseColor"] = [float(x) for x in w[1:]]
    return mat


def parse_obj(path):
    vt, faces = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("vt "):
            vt.append(tuple(float(x) for x in line.split()[1:3]))
        elif line.startswith("f "):
            corners = []
            for c in line.split()[1:]:
                p = c.split("/")
                corners.append((int(p[0]) - 1, int(p[1]) - 1 if len(p) > 1 and p[1] else -1))
            faces.append(corners)
    return vt, faces


def parse_mhclo(path):
    folder = os.path.dirname(path)
    info = {"verts": [], "delete": [], "scale": {}, "material": None, "obj": None, "name": None, "z_depth": 50}
    status = ""
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        w = line.split()
        if status == "v":
            if w[0].lstrip("-").isdigit():
                if len(w) == 1:
                    info["verts"].append(((int(w[0]),) * 3, (1.0, 0.0, 0.0), (0.0, 0.0, 0.0)))
                else:
                    info["verts"].append(((int(w[0]), int(w[1]), int(w[2])), tuple(float(x) for x in w[3:6]),
                                          tuple(float(x) for x in w[6:9])))
                continue
            status = ""
        if status == "d":
            if w[0].isdigit():
                k = 0
                while k < len(w):
                    if k + 2 < len(w) and w[k + 1] == "-":
                        info["delete"].extend(range(int(w[k]), int(w[k + 2]) + 1))
                        k += 3
                    else:
                        info["delete"].append(int(w[k]))
                        k += 1
                continue
            status = ""
        key = w[0]
        if key == "verts":
            status = "v"
        elif key == "delete_verts":
            status = "d"
        elif key in ("x_scale", "y_scale", "z_scale"):
            info["scale"][key] = (int(w[1]), int(w[2]), float(w[3]))
        elif key == "material":
            info["material"] = os.path.join(folder, w[1])
        elif key == "obj_file":
            info["obj"] = os.path.join(folder, w[1])
        elif key == "name":
            info["name"] = w[1]
        elif key == "z_depth":
            info["z_depth"] = int(w[1])
    return info


def write_asset(kind, mhclo_path):
    """Fitted asset: every render vertex follows three base vertices plus an offset scaled by the body's size
    (MakeHuman's proxy fitting, evaluated by the engine for every body shape)."""
    info = parse_mhclo(mhclo_path)
    if not info["obj"] or not os.path.isfile(info["obj"]) or not info["verts"]:
        return None
    vt, faces = parse_obj(info["obj"])
    name = kind + "_" + re.sub(r"[^a-z0-9]+", "_", (info["name"] or os.path.basename(mhclo_path)).lower()).strip("_")
    # Offsets: MPFB's mhclo reader maps (d0, d1, d2) -> Blender (d0, -d2, d1); each Blender axis is scaled by the
    # body's extent between two reference vertices: x by x_scale (along x), y by z_scale (along y), z by y_scale
    # (along z). Model space flips x and y.
    scale_pairs = [info["scale"].get("x_scale"), info["scale"].get("z_scale"), info["scale"].get("y_scale")]
    if any(p is None for p in scale_pairs):
        scale_pairs = None
    key_to_rv, rverts, tris = {}, [], []
    for face in faces:
        corners = []
        for vi, ti in face:
            uv = vt[ti] if 0 <= ti < len(vt) else (0.0, 0.0)
            key = (vi, ti)
            rv = key_to_rv.get(key)
            if rv is None:
                refs, ws, d = info["verts"][vi]
                off = np.array([d[0], -d[2], d[1]]) * FACE
                rv = key_to_rv[key] = len(rverts)
                rverts.append((refs, ws, off, uv[0], 1.0 - uv[1]))
            corners.append(rv)
        for k in range(1, len(corners) - 1):
            tris.append((corners[0], corners[k], corners[k + 1]))
    mat = parse_mhmat(info["material"]) if info["material"] and os.path.isfile(info["material"]) else {}
    albedo = copy_texture(mat.get("diffuseTexture"), name + "_albedo")
    normal = copy_texture(mat.get("normalmapTexture"), name + "_nrm")
    alpha_tex = mat.get("transparencyMapTexture")
    # asset_<name>.bin: u32 R; R x (u32x3 refs, float3 weights, float3 offset, float u, float v); u32 T, T x u32x3;
    #                   u8 has_scale + 3 x (u32 a, u32 b, float reference); u32 D, D x u32 deleted base vertices
    with open(os.path.join(OUT, "asset_" + name + ".bin"), "wb") as f:
        f.write(struct.pack("<I", len(rverts)))
        for refs, ws, off, u, v in rverts:
            f.write(struct.pack("<3I3f3f2f", *refs, *ws, *off, u, v))
        f.write(struct.pack("<I", len(tris)))
        f.write(np.asarray(tris, np.uint32).tobytes())
        f.write(struct.pack("<B", 1 if scale_pairs else 0))
        for p in (scale_pairs or [(0, 0, 1.0)] * 3):
            f.write(struct.pack("<IIf", p[0], p[1], p[2]))
        f.write(struct.pack("<I", len(info["delete"])))
        f.write(np.asarray(info["delete"], np.uint32).tobytes())
    log("asset", kind, name, len(rverts), "vertices", len(tris), "triangles", "delete", len(info["delete"]))
    return {"kind": kind, "name": name, "file": "asset_" + name + ".bin", "albedo": albedo, "normal": normal,
            "alphaTest": kind in ("hair", "eyebrows", "eyelashes") or bool(alpha_tex), "zDepth": info["z_depth"],
            "color": mat.get("diffuseColor")}


assets, skins, eye_materials = [], [], []
for kind in ("eyes", "eyebrows", "eyelashes", "hair", "clothes"):
    for mhclo in sorted(glob.glob(os.path.join(ASSET_DIR, kind, "*", "*.mhclo"))):
        a = write_asset(kind, mhclo)
        if a:
            assets.append(a)
# Eye colours are separate materials for the eye mesh.
for mhmat in sorted(glob.glob(os.path.join(ASSET_DIR, "eyes", "materials", "*.mhmat"))):
    nm = os.path.splitext(os.path.basename(mhmat))[0]
    tex = copy_texture(parse_mhmat(mhmat).get("diffuseTexture"), "eye_" + nm)
    if tex:
        eye_materials.append({"name": nm, "albedo": tex})
for mhmat in sorted(glob.glob(os.path.join(ASSET_DIR, "skins", "*", "*.mhmat"))):
    nm = os.path.splitext(os.path.basename(mhmat))[0]
    m = parse_mhmat(mhmat)
    tex = copy_texture(m.get("diffuseTexture"), "skin_" + nm)
    if tex:
        skins.append({"name": nm, "albedo": tex, "normal": copy_texture(m.get("normalmapTexture"), "skin_" + nm + "_nrm")})
log(len(assets), "assets,", len(skins), "skins,", len(eye_materials), "eye colours")

write_base()
write_targets()
with open(os.path.join(OUT, "human.json"), "w") as f:
    json.dump({"vertices": N, "ground": GROUND, "bones": bones, "sliders": sliders,
               "macroTargets": {n: target_index[n] for n in macro_names}, "macro": macro_config,
               "assets": assets, "skins": skins, "eyeMaterials": eye_materials}, f)
log("done")
