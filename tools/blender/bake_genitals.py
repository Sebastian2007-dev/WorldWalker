# Blender background script: fits genital meshes to WorldWalker's morphable human and bakes them as fitted assets
# (kind "genitals") with their own morph sliders.
#
#   blender -b [source.blend] --python tools/blender/bake_genitals.py -- <out_dir> [--placeholder]
#                                                                    [--proxy <file.proxy> <label>]...
#
# Sources (any combination):
#   * Mesh objects named "genitals_<name>" in the opened .blend, modelled on an MPFB human (scale 0.1, the default).
#     Shape keys other than the basis become morphs: "<slider>-decr" / "<slider>-incr" pairs form one slider
#     (-1..1), any other key a one-sided slider (0..1). Custom properties: "label" (shown in the creator),
#     "sex" ("male" / "female", picked by the templates). A vertex group "delete_<object name>" on the MPFB base mesh
#     of the file hides those body vertices while the asset is worn. The body the mesh is fitted against is that base
#     mesh with its shape keys as set in the file, so model on whatever body shape suits the mesh.
#   * --placeholder: MakeHuman's own genital helper geometry (CC0) on the young male body, with its penis targets.
#   * --proxy: a MakeHuman topology (whole-body .proxy + .obj for hm08) that models genitals. Only the part that
#     departs from the body near the crotch is kept, with two rings of matching skin around it; the body skin under
#     it is hidden. Sex comes from the proxy's tags, licence and author from its header. Proxy vertices bound to the
#     genital helper follow MakeHuman's penis targets, which become sliders as for the placeholder.
#
# Each asset vertex follows the nearest body triangle (three base vertices, barycentric weights) plus an offset scaled
# by the pelvis' extent, like MakeHuman proxies (see tools/human_assets.py). UVs come from the body at that point, so
# the asset uses the body's skin texture and blends into it. The asset follows the hip bone rigidly.
# Run after bake_human.py: it adds to (and replaces its own entries in) <out_dir>/human.json.
import bpy, bmesh, sys, os, json, gzip, struct, re
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree
from mathutils.geometry import barycentric_transform

argv = sys.argv[sys.argv.index("--") + 1:]
OUT = os.path.abspath(argv[0])
PLACEHOLDER = "--placeholder" in argv
PROXIES = [(argv[i + 1], argv[i + 2]) for i, a in enumerate(argv) if a == "--proxy"]

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from human_assets import parse_mhclo, parse_obj
from bl_ext.user_default.mpfb.services.objectservice import ObjectService
from bl_ext.user_default.mpfb.services.locationservice import LocationService

TARGETS = os.path.join(LocationService.get_mpfb_data(""), "targets")
FACE = np.array([-1.0, -1.0, 1.0])  # MPFB humans look along -y; model space looks along +y
HELPER = range(15128, 15328)        # base mesh vertex group "helper-genital"
SUBDIVISIONS = 2
MARGIN = 0.035                      # --proxy: skin kept around the genitals (m)
RIM = 0.008                        # --proxy: body vertices closer than this to the asset's edge stay visible (m)


def log(*a):
    print("[genitals]", *a, flush=True)


def read_target(path):
    """MakeHuman .target(.gz) -> (indices, deltas in Blender space at MPFB scale 0.1)."""
    opener = gzip.open if path.endswith(".gz") else open
    idx, d = [], []
    with opener(path, "rt") as f:
        for line in f:
            w = line.split()
            if len(w) == 4 and not line.startswith("#"):
                idx.append(int(w[0]))
                d.append((float(w[1]), -float(w[3]), float(w[2])))  # MakeHuman y up -> Blender z up
    return np.asarray(idx, np.int64), np.asarray(d, np.float64).reshape(-1, 3) * 0.1


def mesh_positions(obj):
    """Vertex positions with the object's shape keys mixed as set (world space)."""
    me = obj.data
    n = len(me.vertices)
    co = np.empty(n * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3).astype(np.float64)
    if me.shape_keys:
        kbs = me.shape_keys.key_blocks
        basis = co.copy()
        kbs[0].data.foreach_get("co", co.reshape(-1))
        basis = co.copy()
        for kb in kbs[1:]:
            if kb.value == 0 or kb.mute:
                continue
            k = np.empty(n * 3, np.float32)
            kb.data.foreach_get("co", k)
            co += (k.reshape(-1, 3) - basis) * kb.value
    m = np.array(obj.matrix_world)
    return co @ m[:3, :3].T + m[:3, 3]


def load_basemesh():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    return ObjectService.load_base_mesh(context=bpy.context, scale_factor=0.1, load_vertex_groups=True)


def young_body(sex):
    """The MPFB base mesh and its positions as a young adult of that sex; for males also the body morphs that
    flatten the moulded underwear bulge while the genitals are shown."""
    body = load_basemesh()
    pos = mesh_positions(body)
    for name in ("universal-%s-young-averagemuscle-averageweight" % sex, "caucasian-%s-young" % sex):
        idx, d = read_target(os.path.join(TARGETS, "macrodetails", name + ".target.gz"))
        pos[idx] += d
    body_morphs = []
    if sex == "male":
        idx, d = read_target(os.path.join(TARGETS, "pelvis", "bulge-decr.target.gz"))
        pos[idx] += d
        body_morphs.append(("bulge-decr", idx, d, 1.0))
    return body, pos, body_morphs


def penis_bodies(pos):
    """Body positions for each of MakeHuman's penis targets (they move the genital helper)."""
    keys = {}
    for part in ("length", "circ", "testicles"):
        for way in ("decr", "incr"):
            idx, d = read_target(os.path.join(TARGETS, "genitals", "penis-%s-%s.target.gz" % (part, way)))
            k = pos.copy()
            k[idx] += d
            keys["%s-%s" % (part, way)] = k
    return keys


def placeholder_source():
    """MakeHuman's genital helper on the young male body: (body object, faces, basis, {key: positions}, props)."""
    body, pos, body_morphs = young_body("male")
    keys = penis_bodies(pos)
    helper = set(HELPER)
    faces = [list(p.vertices) for p in body.data.polygons if all(v in helper for v in p.vertices)]
    remap = {v: i for i, v in enumerate(sorted(helper))}
    sub = lambda a: a[sorted(helper)]
    src = {"faces": [[remap[v] for v in f] for f in faces], "basis": sub(pos), "keys": {k: sub(v) for k, v in keys.items()},
           "props": {"label": "Männlich (MakeHuman)", "sex": "male"}, "delete": [], "body_morphs": body_morphs}
    return body, pos, {"genitals_male_mpfb": src}


def proxy_positions(info, pos):
    """Positions of a MakeHuman proxy's vertices on a body (MPFB's fitting: three base vertices plus an offset
    (d0, d1, d2) -> Blender (d0, -d2, d1), each axis scaled by the body's extent between the reference pair)."""
    scale = []
    for key, axis in (("x_scale", 0), ("z_scale", 1), ("y_scale", 2)):
        a, b, ref = info["scale"][key]
        scale.append(abs(pos[a, axis] - pos[b, axis]) / 0.1 / ref)
    refs = np.array([v[0] for v in info["verts"]])
    ws = np.array([v[1] for v in info["verts"]])
    d = np.array([v[2] for v in info["verts"]])
    off = np.stack([d[:, 0], -d[:, 2], d[:, 1]], axis=1) * np.array(scale) * 0.1
    return (pos[refs] * ws[:, :, None]).sum(axis=1) + off, refs


def proxy_source(path, label):
    """The genital part of a MakeHuman topology, on the young body of the proxy's sex."""
    info = parse_mhclo(path)
    header = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        w = line.split(None, 2)
        if len(w) == 3 and w[0] == "#" and w[1] in ("author", "license"):
            header[w[1]] = w[2].split("(")[0].strip()
        elif len(w) >= 2 and w[0] == "tag" and w[1].lower() in ("male", "female"):
            header["sex"] = w[1].lower()
    sex = header.get("sex", "female")
    body, pos, body_morphs = young_body(sex)
    co, refs = proxy_positions(info, pos)
    _, faces = parse_obj(info["obj"])
    faces = [[v for v, _ in f] for f in faces]
    # Skin the topology shares with the body lies on it; the genitals stand out from it (or sink in).
    tris, _, in_body, _ = body_triangles(body, pos)
    tree = BVHTree.FromPolygons([Vector(p) for p in pos], tris.tolist())
    off_body = np.array([(Vector(p) - tree.find_nearest(Vector(p))[0]).length for p in co])
    helper = np.isin(refs, list(HELPER)).any(axis=1)
    centre = pos[list(HELPER)].mean(axis=0)
    near = (np.abs(co[:, 0] - centre[0]) < 0.07) & (np.abs(co[:, 1] - centre[1]) < 0.09) & \
           (np.abs(co[:, 2] - centre[2]) < 0.08)
    core = near & ((off_body > 0.002) | helper)
    if not core.any():
        raise SystemExit(path + ": no genital geometry found near the crotch")
    # A margin of skin around it, so the asset's edge lies on the body and meets it without a seam, far enough out
    # that the hidden body skin (below) clears the genitals.
    core_tree = KDTree(int(core.sum()))
    for i, v in enumerate(np.where(core)[0]):
        core_tree.insert(Vector(co[v]), i)
    core_tree.balance()
    margin = np.array([core_tree.find(Vector(p))[2] < MARGIN for p in co])
    faces = [f for f in faces if any(margin[v] for v in f)]
    used = sorted({v for f in faces for v in f})
    remap = {v: i for i, v in enumerate(used)}
    faces = [[remap[v] for v in f] for f in faces]
    basis = co[used]
    keys = {}
    if helper.any():
        keys = {k: proxy_positions(info, p)[0][used] for k, p in penis_bodies(pos).items()}
    # Hide the body skin the asset covers, so that sunken parts are not covered by the body. Hiding a base vertex
    # hides every face around it, so a vertex is hidden only when all of those faces lie inside the asset's outline.
    edges = {}
    for f in faces:
        for a, b in zip(f, f[1:] + f[:1]):
            e = (min(a, b), max(a, b))
            edges[e] = edges.get(e, 0) + 1
    rim = sorted({v for e, n in edges.items() if n == 1 for v in e})
    rim_tree = KDTree(len(rim))
    for i, v in enumerate(rim):
        rim_tree.insert(Vector(basis[v]), i)
    rim_tree.balance()
    patch = BVHTree.FromPolygons([Vector(p) for p in basis],
                                 [(f[0], f[k], f[k + 1]) for f in faces for k in range(1, len(f) - 1)])
    inside = np.zeros(len(pos), bool)
    for v in np.where(in_body)[0]:
        p = Vector(pos[v])
        hit = patch.find_nearest(p, 0.04)
        if hit[0] is not None:
            to_rim = rim_tree.find(p)[2]
            inside[v] = to_rim > RIM and hit[3] < 0.5 * to_rim
    ring_inside = inside.copy()
    for p in body.data.polygons:
        vs = list(p.vertices)
        if not all(inside[v] for v in vs):
            ring_inside[vs] = False
    delete = [int(v) for v in np.where(ring_inside)[0]]
    name = "genitals_" + re.sub(r"[^a-z0-9]+", "_", (info["name"] or os.path.basename(path)).lower()).strip("_")
    log(name, sex, int(core.sum()), "genital vertices", len(used), "kept", len(delete), "body vertices hidden")
    src = {"faces": faces, "basis": basis, "keys": keys, "delete": delete, "body_morphs": body_morphs,
           "subdivisions": 1, "props": {"label": label, "sex": sex, "license": header.get("license", ""),
                                         "author": header.get("author", "")}}
    return body, pos, {name: src}


def file_sources():
    """genitals_* objects of the opened .blend and the MPFB base mesh they were modelled on."""
    body = next((o for o in bpy.data.objects if o.type == "MESH" and "body" in o.vertex_groups
                 and len(o.data.vertices) > 15000), None)
    sources = {}
    for obj in bpy.data.objects:
        if obj.type != "MESH" or not obj.name.startswith("genitals_"):
            continue
        me = obj.data
        basis_vals = {}
        if me.shape_keys:
            for kb in me.shape_keys.key_blocks:
                basis_vals[kb.name] = kb.value
                kb.value = 0.0
        basis = mesh_positions(obj)
        keys = {}
        if me.shape_keys:
            for kb in me.shape_keys.key_blocks[1:]:
                kb.value = 1.0
                keys[kb.name] = mesh_positions(obj)
                kb.value = 0.0
            for kb in me.shape_keys.key_blocks:
                kb.value = basis_vals[kb.name]
        delete = []
        if body and ("delete_" + obj.name) in body.vertex_groups:
            gi = body.vertex_groups["delete_" + obj.name].index
            delete = [v.index for v in body.data.vertices if any(g.group == gi and g.weight > 0.5 for g in v.groups)]
        name = re.sub(r"[^a-z0-9_]+", "_", obj.name.lower())
        # Body shape keys named "body:<x>" on the base mesh, set to a non-zero value: applied to the body while
        # the asset is shown (e.g. flattening a moulded bulge). The mesh is fitted against the body with them.
        body_morphs = []
        if body and body.data.shape_keys:
            kbs = body.data.shape_keys.key_blocks
            b0 = np.empty(len(body.data.vertices) * 3, np.float32)
            kbs[0].data.foreach_get("co", b0)
            for kb in kbs[1:]:
                if kb.name.startswith("body:") and kb.value != 0:
                    k = np.empty(len(b0), np.float32)
                    kb.data.foreach_get("co", k)
                    d = (k - b0).reshape(-1, 3).astype(np.float64)
                    idx = np.where(np.abs(d).max(axis=1) > 1e-7)[0]
                    body_morphs.append((kb.name[5:], idx, d[idx], float(kb.value)))
        sources[name] = {"faces": [list(p.vertices) for p in me.polygons], "basis": basis, "keys": keys,
                         "props": {k: obj[k] for k in ("label", "sex") if k in obj}, "delete": delete,
                         "body_morphs": body_morphs}
    if sources and not body:
        raise SystemExit("genitals_* objects need the MPFB base mesh they were modelled on in the same file")
    return body, (mesh_positions(body) if body else None), sources


def subdivide(src):
    """Catmull-Clark the source (basis and every key alike) so the low-poly cages render smoothly."""
    me = bpy.data.meshes.new("src")
    me.from_pydata(src["basis"].tolist(), [], src["faces"])
    obj = bpy.data.objects.new("src", me)
    bpy.context.scene.collection.objects.link(obj)
    mod = obj.modifiers.new("subd", "SUBSURF")
    mod.levels = mod.render_levels = src.get("subdivisions", SUBDIVISIONS)

    def evaluate(co):
        me.vertices.foreach_set("co", co.astype(np.float32).reshape(-1))
        me.update()
        ev = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        m = ev.to_mesh()
        out = np.empty(len(m.vertices) * 3, np.float32)
        m.vertices.foreach_get("co", out)
        faces = [list(p.vertices) for p in m.polygons]
        ev.to_mesh_clear()
        return out.reshape(-1, 3).astype(np.float64), faces

    keys = {k: evaluate(v)[0] for k, v in src["keys"].items()}
    basis, faces = evaluate(src["basis"])
    bpy.data.objects.remove(obj)
    return dict(src, basis=basis, faces=faces, keys=keys)


def body_triangles(body, pos):
    """Triangles of the body faces with their base vertex indices and UVs (engine convention: v flipped)."""
    me = body.data
    gi = body.vertex_groups["body"].index
    in_body = np.zeros(len(me.vertices), bool)
    for v in me.vertices:
        if any(g.group == gi for g in v.groups):
            in_body[v.index] = True
    uv = me.uv_layers.active.data
    # UV islands: faces sharing a vertex with the same UV belong together.
    parent = {}

    def root(k):
        while parent.setdefault(k, k) != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k
    corner = lambda l: (me.loops[l].vertex_index, round(uv[l].uv[0], 5), round(uv[l].uv[1], 5))
    tris, uvs, faces = [], [], []
    for p in me.polygons:
        if not all(in_body[v] for v in p.vertices):
            continue
        loops = list(p.loop_indices)
        for l in loops[1:]:
            parent[root(corner(l))] = root(corner(loops[0]))
        for k in range(1, len(loops) - 1):
            corners = (loops[0], loops[k], loops[k + 1])
            tris.append([me.loops[l].vertex_index for l in corners])
            uvs.append([(uv[l].uv[0], 1.0 - uv[l].uv[1]) for l in corners])
            faces.append(loops[0])
    roots = [root(corner(l)) for l in faces]
    ids = {r: i for i, r in enumerate(dict.fromkeys(roots))}
    return np.asarray(tris), np.asarray(uvs), in_body, np.array([ids[r] for r in roots])


def scale_pairs(pos, in_body, centre):
    """Pairs of body vertices spanning the pelvis along each model axis, with their distance in the fitting body."""
    model = pos * FACE
    c = centre * FACE
    near = np.where(in_body & (np.abs(model[:, 2] - c[2]) < 0.03))[0]
    pairs = []
    for axis in (0, 1):
        a, b = near[model[near, axis].argmin()], near[model[near, axis].argmax()]
        pairs.append((int(a), int(b), float(abs(model[a, axis] - model[b, axis]))))
    column = np.where(in_body & (np.abs(model[:, 0] - c[0]) < 0.02) & (model[:, 1] > c[1] - 0.05))[0]
    top = column[np.abs(model[column, 2] - (c[2] + 0.15)).argmin()]
    bottom = column[np.abs(model[column, 2] - (c[2] - 0.08)).argmin()]
    pairs.append((int(top), int(bottom), float(abs(model[top, 2] - model[bottom, 2]))))
    return pairs


def fit(name, src, body, pos, out):
    src = subdivide(src)
    tris, tri_uv, in_body, island = body_triangles(body, pos)
    verts = [Vector(p) for p in pos]
    tree = BVHTree.FromPolygons(verts, tris.tolist())
    island_trees = {}

    def nearest(p, isl=None):
        """Nearest body point (optionally on one UV island): (triangle, barycentric weights, point)."""
        if isl is None:
            loc, _, ti, _ = tree.find_nearest(Vector(p))
        else:
            if isl not in island_trees:
                sel = np.where(island == isl)[0]
                island_trees[isl] = (sel, BVHTree.FromPolygons(verts, tris[sel].tolist()))
            sel, t = island_trees[isl]
            loc, _, k, _ = t.find_nearest(Vector(p))
            ti = sel[k]
        a, b, c = (Vector(pos[i]) for i in tris[ti])
        w = barycentric_transform(loc, a, b, c, Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1)))
        w = np.clip(np.array(w), 0, None)
        return ti, w / w.sum(), loc
    centre = src["basis"].mean(axis=0)
    pairs = scale_pairs(pos, in_body, centre)
    hits = [nearest(p) for p in src["basis"]]
    # Asset faces whose corners map to different UV islands of the body would stretch across the texture; their
    # stray corners take their UV from the face's island instead (a vertex is split where that differs).
    rverts, keyed, split = [], {}, {}
    faces = []
    for f in src["faces"]:
        votes = [island[hits[v][0]] for v in f]
        isl = max(set(votes), key=votes.count)
        corners = []
        for v in f:
            ti, w, loc = hits[v]
            key = (v, None if island[ti] == isl else isl)
            if key not in keyed:
                uv_ti, uv_w = (ti, w) if key[1] is None else nearest(loc, isl)[:2]
                uv = (tri_uv[uv_ti] * uv_w[:, None]).sum(axis=0)
                off = (np.asarray(src["basis"][v]) - np.asarray(loc)) * FACE
                keyed[key] = len(rverts)
                split.setdefault(v, []).append(len(rverts))
                rverts.append((tris[ti], w, off, uv))
            corners.append(keyed[key])
        faces.append(corners)
    tris_out = [(f[0], f[k], f[k + 1]) for f in faces for k in range(1, len(f) - 1)]
    # Model space mirrors x and y (a rotation): winding stays the same.
    morphs = []
    for key, co in sorted(src["keys"].items()):
        d = (co - src["basis"]) * FACE
        idx = np.where(np.abs(d).max(axis=1) > 1e-6)[0]
        moved = [(r, d[v]) for v in idx for r in split.get(v, [])]
        morphs.append((key, np.array([r for r, _ in moved], np.int64), np.array([x for _, x in moved]).reshape(-1, 3)))
    # Body morphs travel in the same section, named "body:<x>", indexed by base vertex.
    body_morphs = src.get("body_morphs", [])
    morphs += [("body:" + key, idx, d * FACE) for key, idx, d, _ in body_morphs]
    # asset_<name>.bin as written by human_assets.write_asset, followed by the morphs:
    #   u32 M; M x (u32 name length, name bytes, u32 n, n x u32 vertex, n x float3 delta in metres)
    # Asset morphs index asset vertices; "body:<x>" morphs index base vertices.
    with open(os.path.join(out, "asset_" + name + ".bin"), "wb") as f:
        f.write(struct.pack("<I", len(rverts)))
        for r, w, off, uv in rverts:
            f.write(struct.pack("<3I3f3f2f", *map(int, r), *w, *off, *uv))
        f.write(struct.pack("<I", len(tris_out)))
        f.write(np.asarray(tris_out, np.uint32).tobytes())
        f.write(struct.pack("<B", 1))
        for a, b, ref in pairs:
            f.write(struct.pack("<IIf", a, b, ref))
        f.write(struct.pack("<I", len(src["delete"])))
        f.write(np.asarray(src["delete"], np.uint32).tobytes())
        f.write(struct.pack("<I", len(morphs)))
        for key, idx, d in morphs:
            raw = key.encode("utf-8")
            f.write(struct.pack("<I", len(raw)) + raw + struct.pack("<I", len(idx)))
            f.write(np.asarray(idx, np.uint32).tobytes())
            f.write(np.asarray(d, np.float32).tobytes())
    # Sliders: "<x>-decr"/"<x>-incr" pairs become one two-sided slider.
    sliders, keys = [], [m[0] for m in morphs if not m[0].startswith("body:")]
    for key in keys:
        m = re.match(r"(.*)-(decr|incr)$", key)
        if m and m.group(2) == "incr" and m.group(1) + "-decr" in keys:
            sliders.append({"name": m.group(1), "neg": m.group(1) + "-decr", "pos": key})
        elif not m or (m.group(2) == "decr" and m.group(1) + "-incr" not in keys) or \
                (m.group(2) == "incr" and m.group(1) + "-decr" not in keys):
            sliders.append({"name": key, "pos": key})
    log(name, len(rverts), "vertices", len(tris_out), "triangles", len(morphs), "morphs", len(src["delete"]), "hidden")
    entry = {"kind": "genitals", "name": name, "file": "asset_" + name + ".bin", "skin": True, "bone": "Hips",
             "sliders": sliders, "license": "CC0", "author": src["props"].get("author", ""),
             "bodyMorphs": {"body:" + key: w for key, _, _, w in body_morphs}}
    entry.update(src["props"])
    return entry


def main():
    # Each source loads a fresh base mesh (which clears the scene), so it is fitted before the next one loads.
    # Proxies come before the placeholder: the templates pick the first asset of each sex.
    loaders = [file_sources] + [lambda p=p, l=l: proxy_source(p, l) for p, l in PROXIES]
    if PLACEHOLDER:
        loaders.append(placeholder_source)
    entries = []
    for load in loaders:
        body, pos, found = load()
        entries += [fit(name, src, body, pos, OUT) for name, src in found.items()]
    if not entries:
        raise SystemExit("nothing to bake: open a .blend with genitals_* objects or pass --placeholder / --proxy")
    meta_path = os.path.join(OUT, "human.json")
    meta = json.load(open(meta_path, encoding="utf-8"))
    names = {e["name"] for e in entries}
    meta["assets"] = [a for a in meta["assets"] if a["name"] not in names] + entries
    with open(meta_path, "w") as f:
        json.dump(meta, f)
    log(len(entries), "assets written")


main()
