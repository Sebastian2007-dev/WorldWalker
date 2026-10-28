# Blender background script: fits hairstyles onto the MB-Lab bodies baked by bake_mblab.py, and adds them to each
# body's human.json as "hair" assets. Two kinds of source: Character Creator OBJ exports that include the head they
# were made on, and rigged FBX hairstyles (VRChat style) without a head.
#
#   blender -b --python tools/blender/bake_hair.py -- <hair_root> <mblab_out_dir>
#
# <hair_root> holds the unpacked packs (see HAIRS). Per hairstyle and body:
#   1. The source head is aligned to the MB-Lab head: eye centres first, then a similarity ICP on the skull. Without a
#      head, the hair's inner shell over the skull stands in for it, starting from the rig's head joint.
#   2. What remains between the heads is a displacement field (closest points, smoothed); every hair vertex moves with
#      the field of the head around it, and vertices that end up under the skin are pushed out of it.
#   3. Each vertex is bound to the nearest triangle of the head, neck or upper body (barycentric weights + offset),
#      in the engine's proxy format, so it follows the sliders and the skeleton.
# The hair's textures (diffuse + opacity) and the scalp cap are packed into one atlas per hairstyle.
import bpy, sys, os, json, struct
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

argv = sys.argv[sys.argv.index("--") + 1:]
ROOT, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
# OBJ (centimetres, y up, looking along +z) -> model space (metres, z up, looking along +y): a rotation.
AXES = np.array([[-1.0, 0, 0], [0, 0, 1.0], [0, 1.0, 0]]) * 0.01
FBM = "x_textures/textures/maya file.fbm/"
# parts: OBJ group (and material) with its textures; the scalp cap keeps the skin from showing between the strands.
HAIRS = [
    {"name": "hair_cc_long", "label": "Lang, glatt", "obj": "long/x_obj/obj/obj file hair 7.obj",
     "head": ("Body", None), "eyes": "Eye",
     "parts": [("PSKMeshes", None, "long/x_texture/Default_Material_Transparency_Diffuse.tif",
                "long/x_texture/Default_Material_Transparency_Opacity.png"),
               ("Scalp_Male", None, "long/x_texture/Scalp1_Transparency_Diffuse.jpg",
                "long/x_texture/Scalp1_Transparency_Opacity.jpg")]},
    {"name": "hair_cc_ponytail", "label": "Pferdeschwanz", "obj": "ponytail/x_obj file/obj file/obj file.obj",
     "head": ("Body", "BodySG"), "eyes": "Eye",
     "parts": [("Hair", None, "ponytail/" + FBM + "CMI_BG_CAS_Default_Hair_01_Clip_CIN_Transparency_Diffuse.jpg",
                "ponytail/" + FBM + "CMI_BG_CAS_Default_Hair_01_Clip_CIN_Transparency_Opacity.jpg"),
               ("Scalp", None, "ponytail/" + FBM + "Scalp1_Transparency_Diffuse.jpg",
                "ponytail/" + FBM + "Scalp1_Transparency_Opacity.jpg")]},
    {"name": "hair_cc_bun", "label": "Dutt mit Pony", "obj": "bun/x_obj file/obj file/obj file.obj",
     "head": ("Body", "BodySG"), "eyes": "Eye",
     "parts": [("Hair_TUR", "Hair_TURSG" + s, f"bun/{FBM}Haircard_0{k}_Transparency_Diffuse.png",
                f"bun/{FBM}Haircard_0{k}_Transparency_Opacity.png") for k, s in ((1, ""), (2, "1"), (3, "2"), (4, "3"))]
              + [("Scalp", None, "bun/" + FBM + "Scalp1_Transparency_Diffuse.jpg",
                  "bun/" + FBM + "Scalp1_Transparency_Opacity.jpg")]},
]
# Rigged FBX hairstyles (unpacked to wetcat/ and dawn/): the WetCat pack ships previews instead of its textures, so its
# solid strands get the pack's grey strand texture (tex.png); Dawn's hair cards come with an alpha texture (from the
# unitypackage, first uv set).
WETCAT = [("hair_wc_alexxa", "Lang, offen (Alexxa)", "Hair_AleXXA_By_WetCAt.fbx"),
          ("hair_wc_faith", "Lang mit Zöpfchen (Faith)", "Faith_hair(July)_by_WetCat.fbx"),
          ("hair_wc_freya", "Seitenzopf (Freya)", "Hair_Freya(By_WetCat).fbx"),
          ("hair_wc_kitty", "Zwei Zöpfe (Kitty)", "Hair_BadKittyProject.fbx"),
          ("hair_wc_lilia", "Sehr lang (Lilia)", "Hair(Lilia)_By_WetCat.fbx"),
          ("hair_wc_lissa", "Lang mit Pony (Lissa)", "HairLissa.fbx"),
          ("hair_wc_misty", "Lang, gestuft (Misty)", "MistyHair(By_WetCat).fbx"),
          ("hair_wc_nina", "Lang, glatt (Nina)", "HairNina.fbx"),
          ("hair_wc_selena", "Halblang (Selena)", "Hair(Selena).fbx")]
HAIRS += [{"name": n, "label": l, "fbx": "wetcat/" + f, "texture": "wetcat/tex.png", "alphaTest": False,
           "license": "WetCat free hair pack (assets/ungrouped)"} for n, l, f in WETCAT]
HAIRS += [{"name": "hair_dawn", "label": "Lang mit Pony (Dawn)", "fbx": "dawn/DawnHairFree_velvet/DawnHairFBX.fbx",
           "texture": "dawn/unity/14.png", "alphaTest": True, "license": "Dawn Hair by velvet, free (assets/ungrouped)"}]
# Bones whose skin hair may hang from: long hair reaches down the back.
CARRIERS = {"Head", "Neck", "Spine2", "Spine1"}


def log(*a):
    print("[hair]", *a, flush=True)


def read_obj(path):
    """Positions (model space), uvs, and faces as (group, material, [(v, vt), ...])."""
    v, vt, faces = [], [], []
    group = material = None
    with open(path, errors="ignore") as f:
        for line in f:
            if line.startswith("v "):
                v.append([float(x) for x in line.split()[1:4]])
            elif line.startswith("vt "):
                vt.append([float(x) for x in line.split()[1:3]])
            elif line.startswith(("o ", "g ")):
                group = line.split(maxsplit=1)[1].strip() if len(line.split()) > 1 else None
            elif line.startswith("usemtl"):
                material = line.split(maxsplit=1)[1].strip()
            elif line.startswith("f "):
                corners = []
                for t in line.split()[1:]:
                    p = t.split("/")
                    corners.append((int(p[0]) - 1, int(p[1]) - 1 if len(p) > 1 and p[1] else -1))
                faces.append((group, material, corners))
    return np.asarray(v) @ AXES.T, np.asarray(vt), faces


def read_fbx(path):
    """Like read_obj (one group "hair"), plus the rig's head joint. Blender looks along -y, the model along +y."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=path)
    v, vt, faces = [], [], []
    for o in bpy.data.objects:
        if o.type != "MESH":
            continue
        m, base = o.data, len(v)
        v += [list(o.matrix_world @ x.co) for x in m.vertices]
        uv = m.uv_layers[0].data
        for poly in m.polygons:
            faces.append(("hair", None, [(base + m.loops[li].vertex_index, len(vt) + k)
                                         for k, li in enumerate(poly.loop_indices)]))
            vt += [list(uv[li].uv) for li in poly.loop_indices]
    arm = [o for o in bpy.data.objects if o.type == "ARMATURE"][0]
    flip = np.diag([-1.0, -1.0, 1.0])
    joint = flip @ np.array(arm.matrix_world @ arm.data.bones["Head"].head_local)
    return np.asarray(v) @ flip, np.asarray(vt), faces, joint


def inner_shell(v, joint):
    """The hair's innermost vertices over the skull: per direction from a point inside it (above the head joint), the
    nearest one. Only directions above the forehead, so the bangs in front of the face stay out."""
    cap = v[v[:, 2] > joint[2]]
    centre = joint + [0, 0.02, 0.45 * (cap[:, 2].max() - joint[2])]
    d = cap - centre
    r = np.linalg.norm(d, axis=1)
    elevation, azimuth = np.arcsin(d[:, 2] / r), np.arctan2(d[:, 1], d[:, 0])
    bins = {}
    for i in np.nonzero(elevation > np.radians(15))[0]:
        key = (int(elevation[i] / np.radians(7.5)), int((azimuth[i] + np.pi) / np.radians(10)))
        if key not in bins or r[i] < r[bins[key]]:
            bins[key] = i
    return cap[list(bins.values())]


def lift_bangs(x, eyes):
    """Anime heads have their eyes low, so the bangs made for them hang over a realistic face down to the nose. In
    front of the face the hair between the hairline and the lowest bang tip is squashed upwards until the tips end at
    the brows; strands at the sides of the face and below the mouth keep their place."""
    centre = (eyes[0] + eyes[1]) / 2
    brow, hairline, mouth = centre[2] + 0.018, centre[2] + 0.06, centre[2] - 0.06

    def smooth(a, b, t):
        t = np.clip((t - a) / (b - a), 0, 1)
        return t * t * (3 - 2 * t)

    face = (1 - smooth(0.055, 0.08, np.abs(x[:, 0]))) * smooth(centre[1] - 0.05, centre[1] - 0.03, x[:, 1])
    bangs = (face > 0.5) & (x[:, 2] > mouth) & (x[:, 2] < hairline)
    if not bangs.any() or x[bangs, 2].min() >= brow:
        return x
    tip = x[bangs, 2].min()
    z = x[:, 2]
    lifted = np.where(z >= tip, hairline - (hairline - z) * (hairline - brow) / (hairline - tip), z + brow - tip)
    weight = face * smooth(mouth - 0.02, mouth, z) * (z < hairline)
    x = x.copy()
    x[:, 2] = z + weight * (lifted - z)
    return x


def select(faces, group, material=None):
    return [c for g, m, c in faces if g == group and (material is None or m == material)]


def read_body(sex):
    d = os.path.join(OUT, sex)
    meta = json.load(open(os.path.join(d, "human.json")))
    b = open(os.path.join(d, "base.bin"), "rb").read()
    o = 0

    def take(fmt, n):
        nonlocal o
        a = np.frombuffer(b, fmt, n, o)
        o += a.nbytes
        return a

    n = int(take("<u4", 1)[0])
    co = take("<f4", n * 3).reshape(-1, 3).astype(np.float64)
    r = int(take("<u4", 1)[0])
    rv = np.frombuffer(b[o:o + r * 12], np.dtype([("v", "<u4"), ("u", "<f4"), ("t", "<f4")]))
    o += r * 12
    t = int(take("<u4", 1)[0])
    tris = rv["v"][take("<u4", t * 3).reshape(-1, 3)].astype(np.int64)
    joints = take("u1", n * 4).reshape(-1, 4)
    names = [bone["name"] for bone in meta["bones"]]
    dominant = np.array([names[j] for j in joints[:, 0]])
    eyes = [a for a in meta["assets"] if a["kind"] == "eyes"][0]
    eb = open(os.path.join(d, eyes["file"]), "rb").read()
    ne = struct.unpack_from("<I", eb)[0]
    ev = np.unique(np.frombuffer(eb, np.dtype([("r", "<u4", 3), ("w", "<f4", 3), ("o", "<f4", 3), ("uv", "<f4", 2)]),
                                 ne, 4)["r"][:, 0])
    return meta, co, tris, dominant, co[ev]


def eye_centres(points):
    left, right = points[points[:, 0] < 0], points[points[:, 0] >= 0]
    return left.mean(0), right.mean(0)


def umeyama(p, q):
    mp, mq = p.mean(0), q.mean(0)
    a, b = p - mp, q - mq
    u, s, vt = np.linalg.svd(b.T @ a / len(p))
    d = np.diag([1, 1, np.sign(np.linalg.det(u @ vt))])
    r = u @ d @ vt
    scale = np.trace(np.diag(s) @ d) / (a ** 2).sum(1).mean()
    return scale, r, mq - scale * r @ mp


def kdtree(points):
    t = KDTree(len(points))
    for i, p in enumerate(points):
        t.insert(p, i)
    t.balance()
    return t


def bvh(co, tris):
    return BVHTree.FromPolygons([Vector(p) for p in co], [tuple(int(i) for i in t) for t in tris])


def barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-18:
        return np.array([1.0, 0.0, 0.0])
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    return np.array([1 - v - w, v, w])


def pixels(path, size):
    img = bpy.data.images.load(path, check_existing=False)
    img.colorspace_settings.name = "Non-Color"
    img.scale(*size)
    px = np.empty(size[0] * size[1] * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(size[1], size[0], 4)


def save(px, path):
    img = bpy.data.images.new("atlas", px.shape[1], px.shape[0], alpha=True)
    img.colorspace_settings.name = "Non-Color"
    img.pixels.foreach_set(np.clip(px, 0, 1).astype(np.float32).ravel())
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def fit(spec, sex, body, source):
    meta, co, tris, dominant, eye_points = body
    v, vt, faces, head, gap = source
    head_tris = tris[np.isin(dominant[tris], ["Head"]).all(1)]
    head_tree = bvh(co, head_tris)
    if "obj" in spec:
        # 1. Eyes, then the skull (points above the eyes' lower edge).
        src_eyes = eye_centres(v[np.unique([i for c in select(faces, spec["eyes"]) for i, _ in c])])
        dst_eyes = eye_centres(eye_points)
        s = np.linalg.norm(dst_eyes[1] - dst_eyes[0]) / np.linalg.norm(src_eyes[1] - src_eyes[0])
        r, t = np.eye(3), (dst_eyes[0] + dst_eyes[1]) / 2 - s * (src_eyes[0] + src_eyes[1]) / 2
        skull = head[head[:, 2] > (src_eyes[0][2] + src_eyes[1][2]) / 2 - 0.02]
    else:
        # 1. Head joints, scaled by the height of the skull over them; then the inner shell a few mm over the skull.
        bone = [b for b in meta["bones"] if b["name"] == "Head"][0]
        dst_joint = co[bone["head"]].mean(0) + np.array(bone.get("headOffset", [0, 0, 0]))
        top = co[np.unique(head_tris)][:, 2].max()
        s, r = (top + gap - dst_joint[2]) / (head[:, 2].max() - spec["joint"][2]), np.eye(3)
        t = dst_joint - s * spec["joint"]
        skull = head
    for _ in range(20):
        p = skull @ (s * r).T + t
        q = []
        for x in p:
            loc, n, _, _ = head_tree.find_nearest(Vector(x))
            q.append(np.array(loc) + np.array(n) * gap)
        q = np.array(q)
        # Without a head the shell includes strands standing off the skull: the worst fifth is left out.
        d = np.linalg.norm(p - q, axis=1)
        keep = d <= np.percentile(d, 80 if gap else 100)
        ds, dr, dt = umeyama(p[keep], q[keep])
        s, r, t = ds * s, dr @ r, ds * dr @ t + dt
    err = np.linalg.norm(skull @ (s * r).T + t - q, axis=1)
    log(sex, spec["name"], "aligned: scale %.3f, skull distance %.1f mm (median)" % (s, 1000 * np.median(err)))

    def place(x):
        return x @ (s * r).T + t

    # 2. Displacement field from the aligned source head to the MB-Lab head, neck and upper body.
    carrier_tris = tris[np.isin(dominant[tris], list(CARRIERS)).all(1)]
    carrier = bvh(co, carrier_tris)
    hp = place(head)
    disp = np.zeros_like(hp)
    for i, x in enumerate(hp):
        loc, n, _, dist = carrier.find_nearest(Vector(x), 0.05)
        if loc is not None:
            disp[i] = np.array(loc) + np.array(n) * gap - x
    tree = kdtree(hp)
    near = [[j for _, j, _ in tree.find_n(x, 12)] for x in hp]
    for _ in range(6):
        disp = np.array([disp[n].mean(0) for n in near])

    rverts, out_tris, cells = [], [], []
    parts = spec["parts"]
    grid = int(np.ceil(np.sqrt(len(parts))))
    rows = int(np.ceil(len(parts) / grid))
    pushed = 0
    for k, (group, material, diffuse, opacity) in enumerate(parts):
        pf = select(faces, group, material)
        if not pf:
            log("  missing part", group, material)
            continue
        cx, cy = k % grid, k // grid
        cells.append((cx, cy, diffuse, opacity))
        key = {}
        for corners in pf:
            # Faces whose uvs sit in another tile (UDIM) are moved back into the first one.
            tile = np.floor(min(vt[j][0] for _, j in corners) + 1e-6) if corners[0][1] >= 0 else 0.0
            ids = []
            for i, j in corners:
                uv = vt[j] - [tile, 0.0] if j >= 0 else np.zeros(2)
                kk = (i, round(uv[0], 6), round(uv[1], 6))
                if kk not in key:
                    key[kk] = len(rverts)
                    u = (cx + np.clip(uv[0], 0, 1)) / grid
                    w = (rows - 1 - cy + np.clip(uv[1], 0, 1)) / rows
                    rverts.append([i, u, 1.0 - w])
                ids.append(key[kk])
            for m in range(1, len(ids) - 1):
                out_tris.append((ids[0], ids[m], ids[m + 1]))
    used = np.unique([rv[0] for rv in rverts])
    moved = []
    for i in used:
        x = place(v[i])
        nb = tree.find_n(x, 8)
        w = np.array([1.0 / max(d, 1e-4) for _, _, d in nb])
        moved.append(x + (disp[[j for _, j, _ in nb]] * w[:, None]).sum(0) / w.sum())
    moved = np.array(moved)
    if "fbx" in spec:
        moved = lift_bangs(moved, eye_centres(eye_points))
    pos = {}
    for i, x in zip(used, moved):
        # Under the skin: out along the surface normal, a millimetre above it.
        loc, n, _, _ = carrier.find_nearest(Vector(x))
        if loc is not None and (x - np.array(loc)) @ np.array(n) < 0.001:
            x = np.array(loc) + np.array(n) * 0.001
            pushed += 1
        pos[i] = x

    # 3. Binding to the body's nearest carrier triangle.
    with open(os.path.join(OUT, sex, "asset_" + spec["name"] + ".bin"), "wb") as f:
        f.write(struct.pack("<I", len(rverts)))
        for i, u, w in rverts:
            x = pos[i]
            loc, _, fi, _ = carrier.find_nearest(Vector(x))
            tri = carrier_tris[fi]
            bw = barycentric(np.array(loc), *co[tri])
            off = (x - co[tri].T @ bw) / 0.1  # the engine scales unscaled offsets by 0.1 (MakeHuman decimetres)
            f.write(struct.pack("<3I3f3f2f", *map(int, tri), *bw, *off, u, w))
        f.write(struct.pack("<I", len(out_tris)))
        f.write(np.asarray(out_tris, np.uint32).tobytes())
        f.write(struct.pack("<B", 0))
        for _ in range(3):
            f.write(struct.pack("<IIf", 0, 0, 1.0))
        f.write(struct.pack("<I", 0))
    log(sex, spec["name"], len(rverts), "vertices,", len(out_tris), "triangles,", pushed, "pushed out of the skin")
    return cells, grid, rows


def atlas(spec, cells, grid, rows, sex):
    cell = 2048 if grid * rows == 1 else 1024 if grid <= 2 else 1024
    px = np.zeros((rows * cell, grid * cell, 4), np.float32)
    for cx, cy, diffuse, opacity in cells:
        rgb = pixels(os.path.join(ROOT, diffuse), (cell, cell))
        a = pixels(os.path.join(ROOT, opacity), (cell, cell))[..., 0] if opacity else rgb[..., 3]
        # Some packs ship near black hair, meant to be dyed by Character Creator's shader: it becomes a neutral grey
        # that keeps the strands' variation, so the creator's hair colours (which keep the luminance) work on it.
        lum = rgb[..., :3].mean(-1)
        top = np.percentile(lum[a > 0.5], 99) if (a > 0.5).any() else 1.0
        if top < 0.3:
            rgb[..., :3] = (0.22 + 0.45 * np.clip(lum / max(top, 1e-3), 0, 1))[..., None]
        y0 = (rows - 1 - cy) * cell
        px[y0:y0 + cell, cx * cell:(cx + 1) * cell, :3] = rgb[..., :3]
        px[y0:y0 + cell, cx * cell:(cx + 1) * cell, 3] = a
    rel = "tex/" + spec["name"] + "_albedo.png"
    save(px, os.path.join(OUT, sex, rel))
    return rel


def source(spec):
    """Positions, uvs, faces, the head (points to align and to measure the displacement on) and the gap between that
    head and the skin."""
    if "obj" in spec:
        v, vt, faces = read_obj(os.path.join(ROOT, spec["obj"]))
        return v, vt, faces, v[np.unique([i for c in select(faces, *spec["head"]) for i, _ in c])], 0.0
    v, vt, faces, spec["joint"] = read_fbx(os.path.join(ROOT, spec["fbx"]))
    spec["parts"] = [("hair", None, spec["texture"], None)]
    return v, vt, faces, inner_shell(v, spec["joint"]), 0.003


sources = {spec["name"]: source(spec) for spec in HAIRS}
for sex in ("female", "male"):
    body = read_body(sex)
    meta = body[0]
    names = {h["name"] for h in HAIRS}
    meta["assets"] = [a for a in meta["assets"] if a["name"] not in names]
    for spec in HAIRS:
        cells, grid, rows = fit(spec, sex, body, sources[spec["name"]])
        meta["assets"].append({"kind": "hair", "name": spec["name"], "file": "asset_" + spec["name"] + ".bin",
                               "albedo": atlas(spec, cells, grid, rows, sex), "normal": None,
                               "alphaTest": spec.get("alphaTest", True), "label": spec["label"],
                               "license": spec.get("license", "see assets/ungrouped (Character Creator export)")})
    with open(os.path.join(OUT, sex, "human.json"), "w") as f:
        json.dump(meta, f)
    log(sex, "done")
