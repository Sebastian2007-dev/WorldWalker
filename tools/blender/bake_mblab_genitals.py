# Blender background script: moves the genital assets of the MakeHuman bake (bake_genitals.py) onto the MB-Lab bodies
# baked by bake_mblab.py, and adds them to each body's human.json (female assets to the female body, male to male).
#
#   blender -b --python tools/blender/bake_mblab_genitals.py -- <makehuman_bake_dir> <mblab_out_dir>
#
# Per asset:
#   1. The MakeHuman pelvis is aligned to the MB-Lab pelvis: hip and thigh joints first, then a similarity ICP on the
#      skin around the crotch.
#   2. Vertices lying on the skin are put onto the MB-Lab skin at the same height; the rest moves with the smoothed
#      displacement between the two crotches. Slider morphs are rotated and scaled along.
#   3. Every vertex is bound to the nearest triangle of the pelvis and thighs and takes the MB-Lab skin's UVs there,
#      so the asset samples the body's own skin texture (a "skin" asset). The body skin under it is hidden, except
#      for a margin along the asset's edge.
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


def transfer(entry, src, dst, sex):
    pv, atris, scaled, scale, deleted, morphs = read_asset(os.path.join(MH, entry["file"]))
    co = src["co"]
    # Rest positions on the MakeHuman base mesh (the engine's proxy formula).
    s3 = np.full(3, 0.1)
    if scaled:
        s3 = np.array([abs(co[a][k] - co[b][k]) / ref for k, (a, b, ref) in enumerate(scale)])
    p = (co[pv["r"]] * pv["w"][..., None]).sum(1) + pv["o"] * s3
    height = np.linalg.norm(pv["o"] * s3, axis=1)  # distance from the skin it hangs from

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
    placed = []
    for x, h in zip(p @ (s * r).T + t, height):
        nb = tree.find_n(x, 8)
        w = np.array([1.0 / max(d, 1e-4) for _, _, d in nb])
        x = x + (disp[[j for _, j, _ in nb]] * w[:, None]).sum(0) / w.sum()
        if h < 0.002:  # on the skin: onto the MB-Lab skin, at its height above the MakeHuman skin
            loc, n, _, _ = tree_dst.find_nearest(Vector(x))
            x = np.array(loc) + np.array(n) * h * s
        placed.append(x)
    placed = np.array(placed)

    # 3. Binding, the skin's uvs, morphs, hidden skin.
    out = []
    for x in placed:
        loc, _, fi, _ = tree_dst.find_nearest(Vector(x))
        tri, rtri = carrier_tris[fi], carrier_rtris[fi]
        bw = barycentric(np.array(loc), *dco[tri])
        uv = np.array([[dst["rv"]["u"][k], dst["rv"]["t"][k]] for k in rtri]).T @ bw
        out.append((tri, bw, (x - dco[tri].T @ bw) / 0.1, uv))
    # Trim the skin border the asset brings along: it was cut for MakeHuman's groin and folds over MB-Lab's. What stays
    # is the part standing out of the skin plus a margin (the whole asset if nothing stands out).
    core = placed[height > 0.0015] if (height > 0.0015).sum() > 10 else placed
    core_tree = kdtree(core)
    keep_v = np.array([core_tree.find(x)[2] < 0.025 for x in placed])
    atris = atris[keep_v[atris].all(1)]
    used = np.unique(atris)
    remap = np.full(len(placed), -1)
    remap[used] = np.arange(len(used))
    atris = remap[atris]
    placed, out = placed[used], [out[i] for i in used]
    rot = s * r
    new_morphs = {}
    for name, (idx, d) in morphs.items():
        if name.startswith("body:"):
            continue  # moves MakeHuman body vertices
        sel = remap[idx] >= 0
        new_morphs[name] = (remap[idx][sel], ((d * s3) @ rot.T / 0.1)[sel])
    edges = {}
    for tri in atris:
        for k in range(3):
            e = tuple(sorted((int(tri[k]), int(tri[(k + 1) % 3]))))
            edges[e] = edges.get(e, 0) + 1
    border = np.unique([v for e, c in edges.items() if c == 1 for v in e])
    asset_tree, border_tree = kdtree(placed), kdtree(placed[border])
    near_body = np.unique(carrier_tris)
    hide = [int(i) for i in near_body
            if asset_tree.find(dco[i])[2] < 0.004 and border_tree.find(dco[i])[2] > 0.015]
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
