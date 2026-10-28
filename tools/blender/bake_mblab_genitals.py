# Blender background script: moves the genital assets of the MakeHuman bake (bake_genitals.py) onto the MB-Lab bodies
# baked by bake_mblab.py, and adds them to each body's human.json (female assets to the female body, male to male).
#
#   blender -b --python tools/blender/bake_mblab_genitals.py -- <makehuman_bake_dir> <mblab_out_dir>
#
# Per asset:
#   1. The MakeHuman pelvis is aligned to the MB-Lab pelvis: hip and thigh joints first, then a similarity ICP on the
#      skin around the crotch.
#   2. Vertices near the skin keep their signed height over it (the female's are sunk into it); the skin rim goes 1 mm
#      under the MB-Lab skin. Vertices further out move with the smoothed displacement between the two crotches.
#      Slider morphs are rotated and scaled along.
#   3. Every vertex is bound to the nearest triangle of the pelvis and thighs and takes the MB-Lab skin's UVs there,
#      so the asset samples the body's own skin texture (a "skin" asset); vertices are split along the skin's uv seams.
#      The body skin over the asset is hidden where the asset's rim closes the hole, and the rim is trimmed to a hem
#      around that hole.
import bpy, sys, os, json, struct
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

argv = sys.argv[sys.argv.index("--") + 1:]
MH, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
CARRIERS = {"Hips", "LeftUpLeg", "RightUpLeg", "Spine"}
PROXY = np.dtype([("r", "<u4", 3), ("w", "<f4", 3), ("o", "<f4", 3), ("uv", "<f4", 2)])


def log(*a):
    print("[genitals]", *a, flush=True)


def read_base(d):
    meta = json.load(open(os.path.join(d, "human.json"), encoding="utf-8"))
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
    rtris = take("<u4", t * 3).reshape(-1, 3).astype(np.int64)
    joints = take("u1", n * 4).reshape(-1, 4)
    names = [bone["name"] for bone in meta["bones"]]
    joint = {bone["name"]: co[bone["head"]].mean(0) for bone in meta["bones"]}
    return {"meta": meta, "co": co, "rv": rv, "rtris": rtris, "tris": rv["v"][rtris].astype(np.int64),
            "dominant": np.array([names[j] for j in joints[:, 0]]), "joint": joint}


def read_asset(path):
    b = open(path, "rb").read()
    n = struct.unpack_from("<I", b)[0]
    pv = np.frombuffer(b, PROXY, n, 4)
    o = 4 + n * PROXY.itemsize
    t = struct.unpack_from("<I", b, o)[0]
    tris = np.frombuffer(b, "<u4", t * 3, o + 4).reshape(-1, 3)
    o += 4 + t * 12
    scaled = b[o]
    o += 1
    scale = [struct.unpack_from("<IIf", b, o + 12 * k) for k in range(3)]
    o += 36
    nd = struct.unpack_from("<I", b, o)[0]
    deleted = np.frombuffer(b, "<u4", nd, o + 4)
    o += 4 + nd * 4
    morphs = {}
    if o < len(b):
        m = struct.unpack_from("<I", b, o)[0]
        o += 4
        for _ in range(m):
            ln = struct.unpack_from("<I", b, o)[0]
            name = b[o + 4:o + 4 + ln].decode()
            o += 4 + ln
            k = struct.unpack_from("<I", b, o)[0]
            idx = np.frombuffer(b, "<u4", k, o + 4)
            d = np.frombuffer(b, "<f4", k * 3, o + 4 + k * 4).reshape(-1, 3)
            o += 4 + k * 16
            morphs[name] = (idx, d.astype(np.float64))
    return pv, tris, scaled, scale, deleted, morphs


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


def barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-18:
        return np.array([1.0, 0.0, 0.0])
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    return np.clip(np.array([1 - v - w, v, w]), 0, 1) / max(np.clip(np.array([1 - v - w, v, w]), 0, 1).sum(), 1e-9)


def skin_uv(dst, rtri, bw):
    return np.array([[dst["rv"]["u"][k], dst["rv"]["t"][k]] for k in rtri]).T @ bw


def quads(dst):
    """Face of each body triangle: the bake fans a quad into two consecutive triangles (a,b,c),(a,c,d), and the engine
    joins them back (Human::subdivide). A hidden vertex hides all of its faces, both halves of a quad."""
    rt, face = dst["rtris"], np.zeros(len(dst["rtris"]), np.int64)
    i = f = 0
    while i < len(rt):
        face[i] = f
        if i + 1 < len(rt) and rt[i + 1][0] == rt[i][0] and rt[i + 1][1] == rt[i][2]:
            face[i + 1] = f
            i += 1
        i += 1
        f += 1
    return face


def border_of(tris):
    edges = {}
    for tri in tris:
        for k in range(3):
            e = tuple(sorted((int(tri[k]), int(tri[(k + 1) % 3]))))
            edges[e] = edges.get(e, 0) + 1
    return np.unique([v for e, c in edges.items() if c == 1 for v in e]).astype(np.int64)


def hidden_skin(dst, placed, atris, border, margin=0.006):
    """Body vertices under the asset. Parts of the asset lie below the skin (the female's are sunk up to 15 mm into
    it), so 'under' means: the line along the body normal meets the asset at least `margin` inside its border. The
    engine hides every face around a hidden vertex, so a vertex is hidden only when its neighbours are covered too;
    the hole then stays inside the asset's skin rim."""
    dco, tris = dst["co"], dst["tris"]
    fn = np.cross(dco[tris[:, 1]] - dco[tris[:, 0]], dco[tris[:, 2]] - dco[tris[:, 0]])
    vn = np.zeros_like(dco)
    for k in range(3):
        np.add.at(vn, tris[:, k], fn)
    vn /= np.maximum(np.linalg.norm(vn, axis=1), 1e-12)[:, None]
    asset = BVHTree.FromPolygons([Vector(x) for x in placed], [tuple(map(int, x)) for x in atris])
    border_tree = kdtree(placed[border])
    lo, hi = placed.min(0) - 0.03, placed.max(0) + 0.03
    candidates = np.nonzero(((dco > lo) & (dco < hi)).all(1))[0]
    covered = np.zeros(len(dco), bool)
    for i in candidates:
        o, n = Vector(dco[i] - vn[i] * 0.03), Vector(vn[i])
        loc = asset.ray_cast(o, n, 0.06)[0]
        covered[i] = loc is not None and border_tree.find(loc)[2] > margin
    face = quads(dst)
    corners = [set() for _ in range(face.max() + 1)]
    for f, t in zip(face, tris):
        corners[f].update(int(a) for a in t)
    neighbours = [set() for _ in dco]
    for c in corners:
        for a in c:
            neighbours[a].update(c)
    return [int(i) for i in candidates if covered[i] and all(covered[j] for j in neighbours[i])]


def transfer(entry, src, dst, sex):
    pv, atris, scaled, scale, deleted, morphs = read_asset(os.path.join(MH, entry["file"]))
    co = src["co"]
    # Rest positions on the MakeHuman base mesh (the engine's proxy formula).
    s3 = np.full(3, 0.1)
    if scaled:
        s3 = np.array([abs(co[a][k] - co[b][k]) / ref for k, (a, b, ref) in enumerate(scale)])
    p = (co[pv["r"]] * pv["w"][..., None]).sum(1) + pv["o"] * s3

    # 1. Hip and knee joints, then ICP on the MakeHuman skin around the asset (the hidden skin left out).
    keys = ("LeftUpLeg", "RightUpLeg", "LeftLeg", "RightLeg")  # hip and knee joints mean the same on both bodies
    s, r, t = umeyama(np.array([src["joint"][k] for k in keys]), np.array([dst["joint"][k] for k in keys]))
    centre = p.mean(0)
    hidden = np.zeros(len(co), bool)
    hidden[deleted] = True
    region = np.nonzero((np.linalg.norm(co - centre, axis=1) < 0.14) & ~hidden &
                        np.isin(np.arange(len(co)), np.unique(src["tris"])))[0]
    carrier_tris = dst["tris"][np.isin(dst["dominant"][dst["tris"]], list(CARRIERS)).all(1)]
    carrier_rtris = dst["rtris"][np.isin(dst["dominant"][dst["tris"]], list(CARRIERS)).all(1)]
    dco = dst["co"]
    tree_dst = BVHTree.FromPolygons([Vector(x) for x in dco], [tuple(map(int, x)) for x in carrier_tris])
    pts = co[region]
    for _ in range(25):
        x = pts @ (s * r).T + t
        q = np.array([tree_dst.find_nearest(Vector(y))[0] for y in x])
        # Rigid: the scale stays the joints' (a scaled ICP shrinks the patch onto the nearest bit of skin).
        _, dr, _ = umeyama(x, q)
        dt = q.mean(0) - dr @ x.mean(0)
        r, t = dr @ r, dr @ t + dt
    err = np.linalg.norm(pts @ (s * r).T + t - q, axis=1)
    log(sex, entry["name"], "aligned: scale %.3f, skin distance %.1f mm (median)" % (s, 1000 * np.median(err)))

    # 2. Displacement field over the region, smoothed.
    rp = pts @ (s * r).T + t
    disp = np.array([np.array(tree_dst.find_nearest(Vector(y))[0]) - y for y in rp])
    tree = kdtree(rp)
    near = [[j for _, j, _ in tree.find_n(y, 10)] for y in rp]
    for _ in range(5):
        disp = np.array([disp[n].mean(0) for n in near])
    def field(y):
        nb = tree.find_n(y, 8)
        w = np.array([1.0 / max(d, 1e-4) for _, _, d in nb])
        return y + (disp[[j for _, j, _ in nb]] * w[:, None]).sum(0) / w.sum()

    # Close to the skin a vertex keeps its signed height over it: its nearest point on the MakeHuman skin (the hidden
    # part included) moves with the field onto the MB-Lab skin, and the height is laid off along the normal there.
    # Further out the nearest skin point jumps between pubis and thighs, so those vertices just move with the field.
    tree_src = BVHTree.FromPolygons([Vector(x) for x in co], [tuple(map(int, x)) for x in src["tris"]])
    placed, heights = [], []
    for x in p:
        loc, n, _, _ = tree_src.find_nearest(Vector(x))
        h = (x - np.array(loc)) @ np.array(n)
        heights.append(h)
        loc, n, _, _ = tree_dst.find_nearest(Vector(field(np.array(loc) @ (s * r).T + t)))
        # The skin rim (within 2 mm of the MakeHuman skin) lies 1 mm under the MB-Lab skin: where the body shows it
        # covers the rim, and in the hole cut for the asset the rim takes the body's normal and texture (render/Human),
        # so no edge shows. A rim on the skin flickers with it (and TAA blends the two into a pale veil).
        lay = np.clip((abs(h) - 0.002) / 0.002, 0, 1)
        near_skin = np.array(loc) + np.array(n) * ((1 - lay) * -0.001 + lay * h * s)
        b = np.clip((abs(h) - 0.005) / 0.01, 0, 1)
        placed.append((1 - b) * near_skin + b * field(x @ (s * r).T + t))
    placed, heights = np.array(placed), np.abs(heights)

    # 3. Binding, the skin's uvs, morphs, hidden skin.
    out = []
    for x in placed:
        loc, _, fi, _ = tree_dst.find_nearest(Vector(x))
        tri, rtri = carrier_tris[fi], carrier_rtris[fi]
        bw = barycentric(np.array(loc), *dco[tri])
        out.append((tri, bw, (x - dco[tri].T @ bw) / 0.1, skin_uv(dst, rtri, bw)))
    # Hide the body skin under the asset, then trim the skin rim to a hem around that hole and the parts standing out
    # of the skin or sunk into it: the rest lies hidden under the body anyway and could poke through it in creases.
    hide = hidden_skin(dst, placed, atris, border_of(atris))
    face = quads(dst)
    hole = np.isin(face, face[np.isin(dst["tris"], hide).any(1)])
    g = np.linspace(0, 1, 6)
    bary = np.array([(a, b, 1 - a - b) for a in g for b in g if a + b <= 1])
    needed = np.concatenate([np.einsum("kj,tjc->tkc", bary, dco[dst["tris"][hole]]).reshape(-1, 3),
                             placed[heights > 0.002]])
    if len(needed):
        needed_tree = kdtree(needed)
        keep_v = np.array([needed_tree.find(x)[2] < 0.01 for x in placed])
        atris = atris[keep_v[atris].any(1)]
    used = np.unique(atris)
    remap = np.full(len(placed), -1)
    remap[used] = np.arange(len(used))
    atris = remap[atris]
    placed, out = placed[used], [out[i] for i in used]

    # The groin lies on uv seams of MB-Lab's skin. A triangle across one would sample the empty atlas between the two
    # sides: its corners far (in uv) from the middle one get a copy with uvs from the body's triangles around that one
    # in the atlas (bound to the body like the original).
    carrier_uv = np.array([skin_uv(dst, rt, np.full(3, 1 / 3)) for rt in carrier_rtris])
    copies, source, charts = {}, [], {}
    for k, tri in enumerate(atris):
        uvs = np.array([out[v][3] for v in tri])
        ref = int(np.argmin(np.abs(uvs - np.median(uvs, 0)).sum(1)))
        for j, v in enumerate(tri):
            if np.abs(uvs[j] - uvs[ref]).max() < 0.03:
                continue
            chart = tuple(np.round(uvs[ref] / 0.02).astype(int))  # cells of the atlas
            if chart not in charts:
                sel = np.nonzero(np.abs(carrier_uv - np.array(chart) * 0.02).max(1) < 0.06)[0]
                charts[chart] = (sel, BVHTree.FromPolygons([Vector(x) for x in dco],
                                                           [tuple(map(int, x)) for x in carrier_tris[sel]]))
            if (v, chart) not in copies:
                sel, tree_chart = charts[chart]
                loc, _, fi, _ = tree_chart.find_nearest(Vector(placed[v]))
                bw = barycentric(np.array(loc), *dco[carrier_tris[sel[fi]]])
                copies[(v, chart)] = len(out)
                out.append(out[v][:3] + (skin_uv(dst, carrier_rtris[sel[fi]], bw),))
                source.append(v)
            atris[k, j] = copies[(v, chart)]
    log(sex, entry["name"], len(source), "vertices copied along uv seams")
    rot = s * r
    new_morphs = {}
    for name, (idx, d) in morphs.items():
        if name.startswith("body:"):
            continue  # moves MakeHuman body vertices
        sel = remap[idx] >= 0
        idx, d = remap[idx][sel], ((d * s3) @ rot.T / 0.1)[sel]
        at = {int(i): k for k, i in enumerate(idx)}
        extra = [(len(placed) + c, at[v]) for c, v in enumerate(source) if v in at]
        new_morphs[name] = (np.concatenate([idx, [c for c, _ in extra]]).astype(np.int64),
                            np.concatenate([d, d[[k for _, k in extra]].reshape(-1, 3)]))
    path = os.path.join(OUT, sex, entry["file"])
    with open(path, "wb") as f:
        f.write(struct.pack("<I", len(out)))
        for tri, bw, off, uv in out:
            f.write(struct.pack("<3I3f3f2f", *map(int, tri), *bw, *off, *uv))
        f.write(struct.pack("<I", len(atris)))
        f.write(np.asarray(atris, np.uint32).tobytes())
        f.write(struct.pack("<B", 0))
        for _ in range(3):
            f.write(struct.pack("<IIf", 0, 0, 1.0))
        f.write(struct.pack("<I", len(hide)))
        f.write(np.asarray(hide, np.uint32).tobytes())
        f.write(struct.pack("<I", len(new_morphs)))
        for name, (idx, d) in new_morphs.items():
            raw = name.encode()
            f.write(struct.pack("<I", len(raw)) + raw)
            f.write(struct.pack("<I", len(idx)))
            f.write(np.asarray(idx, np.uint32).tobytes())
            f.write(np.asarray(d, np.float32).tobytes())
    log(sex, entry["name"], len(out), "vertices,", len(hide), "skin vertices hidden")
    e = {k: v for k, v in entry.items() if k != "bodyMorphs"}
    return e


src = read_base(MH)
for sex in ("female", "male"):
    dst = read_base(os.path.join(OUT, sex))
    meta = dst["meta"]
    meta["assets"] = [a for a in meta["assets"] if a["kind"] != "genitals"]
    for entry in src["meta"]["assets"]:
        if entry["kind"] == "genitals" and entry.get("sex") == sex:
            meta["assets"].append(transfer(entry, src, dst, sex))
    with open(os.path.join(OUT, sex, "human.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)
    log(sex, "done")
