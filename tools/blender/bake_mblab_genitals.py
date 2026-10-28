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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mblab_fit import PROXY, read_base, read_asset, umeyama, kdtree, barycentric, skin_uv, quads, border_of, hidden_skin

argv = sys.argv[sys.argv.index("--") + 1:]
MH, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
CARRIERS = {"Hips", "LeftUpLeg", "RightUpLeg", "Spine"}


def log(*a):
    print("[genitals]", *a, flush=True)


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
    hide = hidden_skin(dst, placed, atris, border_of(atris, placed))
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
