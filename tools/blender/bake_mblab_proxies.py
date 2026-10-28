# Blender background script: moves MakeHuman proxy assets (clothes, hair, eyebrows) of the MakeHuman bake
# (bake_human.py, human_assets.py) onto the MB-Lab bodies baked by bake_mblab.py, and adds them to each body's
# human.json.
#
#   blender -b --python tools/blender/bake_mblab_proxies.py -- <makehuman_bake_dir> <mblab_out_dir> [kind ...]
#
# The assets come from the MakeHuman bake and its extra/ directory (tools/install_asset_packs.py); the results go to
# <mblab_out_dir>/<sex>/extra (human.json there lists them, paths relative to it), outside version control.
#
# Per body (female, male):
#   1. The MakeHuman body takes that sex's shape (its macro targets) and is posed into the MB-Lab rest pose: every
#      bone turns onto the MB-Lab bone's direction and stretches to its length, the skin follows its weights. The
#      MakeHuman arms hang 20 degrees lower, so without this sleeves would be torn off at the shoulders.
#   2. A non-rigid registration then draws the posed MakeHuman skin onto the MB-Lab skin (closest points both ways,
#      smoothed over the mesh), so that breasts, crotch and hips correspond, not just the nearest points.
#   3. Every asset vertex is posed with the weights of the skin it hangs from, then keeps its height over the skin:
#      its nearest point on the posed MakeHuman skin moves with the field onto the MB-Lab skin, and the height is laid
#      off along the normal there. Far from the skin (skirt hems, long hair) vertices just move with the field.
#   4. Binding to the nearest MB-Lab triangle of the same body part; clothes hide the skin they cover closely.
import bpy, sys, os, json, struct
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mblab_fit import read_base, read_asset, kdtree, barycentric, border_of, hidden_skin, umeyama

argv = sys.argv[sys.argv.index("--") + 1:]
MH, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
KINDS = argv[2:] or ["clothes", "eyebrows", "hair"]
# Minimum height over the skin: cloth stays outside it, brows and hair roots may lie on it.
MIN_HEIGHT = {"clothes": 0.003}


def log(*a):
    print("[proxies]", *a, flush=True)


def part(bone):
    """Body part of a bone: nearest points are looked up on the part and its neighbours (see reach)."""
    side = "L" if bone.startswith("Left") else "R" if bone.startswith("Right") else ""
    if "Hand" in bone:
        return "hand" + side
    if bone.endswith("ForeArm") or bone.endswith("Arm"):
        return "arm" + side
    if bone.endswith("UpLeg") or bone.endswith("Leg"):
        return "leg" + side
    if bone.endswith("Foot") or bone.endswith("ToeBase"):
        return "foot" + side
    if bone in ("Head", "Neck"):
        return "head"
    return "trunk"


def read_targets(d):
    b = open(os.path.join(d, "targets.bin"), "rb").read()
    n, o, out = struct.unpack_from("<I", b)[0], 4, []
    for _ in range(n):
        k = struct.unpack_from("<I", b, o)[0]
        idx = np.frombuffer(b, "<u4", k, o + 4)
        delta = np.frombuffer(b, "<f4", k * 3, o + 4 + k * 4).reshape(-1, 3)
        out.append((idx, delta))
        o += 4 + k * 16
    return out


def joints_of(body, co):
    """Bone head and tail positions on the vertices co."""
    def at(bone, key):
        p = co[bone[key]].mean(0)
        return p + np.array(bone.get(key + "Offset", [0, 0, 0]))
    return {b["name"]: (at(b, "head"), at(b, "tail")) for b in body["meta"]["bones"]}


def bone_matrices(src_meta, src_joints, dst_joints):
    """Per MakeHuman bone: turn onto the MB-Lab bone, stretched along it to its length, head onto head."""
    mats = []
    for b in src_meta["bones"]:
        if b["name"] == "Hips":
            # The rigs place the pelvis bone differently (MakeHuman's head 5 cm over the hip joints, MB-Lab's 8 cm under
            # them): the pelvis follows the hip joints and the start of the spine instead, which mean the same in both.
            keys = [(k, 0) for k in ("LeftUpLeg", "RightUpLeg", "Spine")]
            s, r, t = umeyama(np.array([src_joints[k][i] for k, i in keys]), np.array([dst_joints[k][i] for k, i in keys]))
            mats.append(np.block([[s * r, t[:, None]], [np.zeros((1, 3)), np.ones((1, 1))]]))
            continue
        if b["name"] not in dst_joints:
            mats.append(mats[b["parent"]] if b["parent"] >= 0 else np.eye(4))
            continue
        hs, ts = src_joints[b["name"]]
        hd, td = dst_joints[b["name"]]
        u, v = ts - hs, td - hd
        k = np.linalg.norm(v) / max(np.linalg.norm(u), 1e-9)
        u, v = u / max(np.linalg.norm(u), 1e-9), v / max(np.linalg.norm(v), 1e-9)
        c, axis = u @ v, np.cross(u, v)
        s = np.linalg.norm(axis)
        if s < 1e-9:
            r = np.eye(3)
        else:
            a = axis / s
            kx = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
            r = np.eye(3) + s * kx + (1 - c) * kx @ kx
        m = r @ (np.eye(3) + (k - 1) * np.outer(u, u))
        mats.append(np.block([[m, (hd - m @ hs)[:, None]], [np.zeros((1, 3)), np.ones((1, 1))]]))
    return np.array(mats)


def pose(points, weights, mats):
    """Linear blend skinning: weights (n, bones) over the bone matrices."""
    h = np.c_[points, np.ones(len(points))]
    return np.einsum("nb,bij,nj->ni", weights, mats, h)[:, :3]


def dense_weights(body, n_bones):
    w = np.zeros((len(body["co"]), n_bones))
    for k in range(4):
        np.add.at(w, (np.arange(len(w)), body["joints"][:, k]), body["weights"][:, k])
    return w / np.maximum(w.sum(1, keepdims=True), 1e-9)


def reach(p):
    """The body parts a point of part p may land on: its own and the adjacent ones. The parts' borders differ between
    the bodies (MB-Lab's thighs begin lower), so a shirt hem weighted to the thighs must still find the hips; but a
    point never crosses between the legs or from one arm to the other."""
    side = p[-1] if p[-1] in "LR" else ""
    return {"trunk": {"trunk", "head", "armL", "armR", "legL", "legR"}, "head": {"head", "trunk"},
            "arm": {p, "trunk", "hand" + side}, "hand": {p, "arm" + side}, "leg": {p, "trunk", "foot" + side},
            "foot": {p, "leg" + side}}[p.rstrip("LR")]


def part_trees(co, tris, parts):
    """BVH per body part over the triangles with a corner in it or in an adjacent part (see reach)."""
    trees = {}
    for p in set(parts):
        sel = tris[np.isin(parts[tris], list(reach(p))).all(1)]
        if len(sel):
            trees[p] = (sel, BVHTree.FromPolygons([Vector(x) for x in co], [tuple(map(int, t)) for t in sel]))
    return trees


def nearest(trees, p, x):
    sel, tree = trees[p]
    loc, n, fi, _ = tree.find_nearest(Vector(x))
    return np.array(loc), np.array(n), sel[fi]


def prepare(src, dst, sex, targets):
    """The MakeHuman body of this sex posed into the MB-Lab rest pose, and the field from it to the MB-Lab skin."""
    meta = src["meta"]
    co = src["co"].copy()
    for name in ("macrodetails/caucasian-%s-young" % sex,
                 "macrodetails/universal-%s-young-averagemuscle-averageweight" % sex):
        t = meta["macroTargets"].get(name)
        if t is not None:
            idx, delta = targets[t]
            co[idx] += delta
    names = [b["name"] for b in meta["bones"]]
    weights = dense_weights(src, len(names))
    mats = bone_matrices(meta, joints_of(src, co), joints_of(dst, dst["co"]))
    posed = pose(co, weights, mats)
    src_parts = np.array([part(names[j]) for j in weights.argmax(1)])
    dst_names = [b["name"] for b in dst["meta"]["bones"]]
    dst_parts = np.array([part(dst_names[j]) for j in dst["joints"][np.arange(len(dst["co"])), dst["weights"].argmax(1)]])
    src_trees = part_trees(posed, src["tris"], src_parts)
    dst_trees = part_trees(dst["co"], dst["tris"], dst_parts)
    # Near the skin the registration gives a point on the MB-Lab skin already: the whole body may take it (the
    # triangles in the middle of the crotch belong to neither leg's part).
    dst_trees["body"] = (dst["tris"], BVHTree.FromPolygons([Vector(x) for x in dst["co"]],
                                                           [tuple(map(int, t)) for t in dst["tris"]]))
    # Non-rigid registration: the posed MakeHuman skin is drawn onto the MB-Lab skin. In every step each MakeHuman
    # vertex pulls towards its nearest MB-Lab point and each MB-Lab vertex pulls the nearest MakeHuman point towards
    # itself, so a smaller breast is stretched over a larger one instead of being pressed onto its underside (a bra
    # would slide under the breast). The pulls are smoothed over the MakeHuman mesh, strongly at first, then less.
    tris = src["tris"]
    edges = np.unique(np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), 1), axis=0)
    degree = np.bincount(edges.ravel(), minlength=len(posed)).astype(np.float64)

    def smooth(d, times):
        for _ in range(times):
            total = np.zeros_like(d)
            np.add.at(total, edges[:, 0], d[edges[:, 1]])
            np.add.at(total, edges[:, 1], d[edges[:, 0]])
            d = 0.5 * d + 0.5 * np.where(degree[:, None] > 0, total / np.maximum(degree, 1)[:, None], d)
        return d

    reg = posed.copy()
    steps = 16
    for step in range(steps):
        reg_trees = part_trees(reg, tris, src_parts)
        pull, weight = np.zeros_like(reg), np.zeros(len(reg))
        for i, (x, p) in enumerate(zip(reg, src_parts)):
            if p in dst_trees:
                pull[i] += nearest(dst_trees, p, x)[0] - x
                weight[i] += 1
        for y, p in zip(dst["co"], dst_parts):
            if p in reg_trees:
                loc, _, tri = nearest(reg_trees, p, y)
                bw = barycentric(loc, *reg[tri])
                np.add.at(pull, tri, bw[:, None] * (y - loc))
                np.add.at(weight, tri, bw)
        d = np.where(weight[:, None] > 0, pull / np.maximum(weight, 1e-9)[:, None], 0.0)
        reg += smooth(d, int(round(24 * (1 - step / steps))) + 2)
    disp = reg - posed
    tree = kdtree(posed)
    err = np.array([np.linalg.norm(nearest(dst_trees, p, x)[0] - x) if p in dst_trees else 0.0
                    for x, p in zip(reg, src_parts)])
    log(sex, "MakeHuman registered onto MB-Lab: skin distance %.1f mm median, %.1f mm 90th percentile, moved %.1f mm median"
        % (1000 * np.median(err), 1000 * np.percentile(err, 90), 1000 * np.median(np.linalg.norm(disp, axis=1))))

    affines = {}

    def affine(p):
        """Least squares affine map of the posed MakeHuman skin of part p onto the MB-Lab skin."""
        if p not in affines:
            sel = src_parts == p
            a = np.c_[posed[sel], np.ones(sel.sum())]
            affines[p] = np.linalg.lstsq(a, posed[sel] + disp[sel], rcond=None)[0].T
        return affines[p]

    def field(x):
        nb = tree.find_n(Vector(x), 8)
        w = np.array([1.0 / max(d, 1e-4) for _, _, d in nb])
        return x + (disp[[j for _, j, _ in nb]] * w[:, None]).sum(0) / w.sum()

    return {"co": co, "weights": weights, "mats": mats, "posed": posed, "reg": reg, "src_trees": src_trees,
            "dst_parts": dst_parts,
            "dst_trees": dst_trees, "field": field, "affine": affine, "names": names}


def transfer(entry, origin, src, dst, fit, out):
    """entry from the catalog in directory origin; writes the asset into the directory out."""
    pv, atris, scaled, scale, _, _ = read_asset(os.path.join(origin, entry["file"]))
    co = fit["co"]
    s3 = np.full(3, 0.1)
    if scaled:
        s3 = np.array([abs(co[a][k] - co[b][k]) / ref for k, (a, b, ref) in enumerate(scale)])
    p = (co[pv["r"]] * pv["w"][..., None]).sum(1) + pv["o"] * s3
    weights = np.einsum("nk,nkb->nb", pv["w"].astype(np.float64), fit["weights"][pv["r"]])
    posed = pose(p, weights, fit["mats"])
    parts = [part(fit["names"][j]) for j in weights.argmax(1)]
    lowest = MIN_HEIGHT.get(entry["kind"], 0.0)
    qs = [q if q in fit["src_trees"] and q in fit["dst_trees"] else "trunk" for q in parts]
    share = lambda prefix: np.mean([q.startswith(prefix) for q in qs])
    # Shoes, hats, glasses and masks keep their shape: one affine map per foot (or for the head), fitted to how the
    # skin of that part moves, instead of following the skin vertex by vertex (which bends a shoe's toe cap inside out).
    footwear = entry["kind"] == "clothes" and share("foot") > 0.3 and np.percentile(posed[:, 2], 90) < 0.6
    headwear = entry["kind"] == "clothes" and share("head") > 0.8
    placed, heights, far = [], [], []
    for x, q in zip(posed, qs):
        if footwear or headwear:
            placed.append(fit["affine"](q) @ np.append(x, 1.0))
            heights.append(0.0)
            far.append(1.0)
            continue
        loc, n, tri = nearest(fit["src_trees"], q, x)
        h = (x - loc) @ n
        # The same point of the registered skin, which lies on MB-Lab's within millimetres.
        mapped = barycentric(loc, *fit["posed"][tri]) @ fit["reg"][tri]
        loc2, n2, _ = nearest(fit["dst_trees"], "body", mapped)
        near_skin = loc2 + n2 * max(h, lowest)
        b = np.clip((abs(h) - 0.02) / 0.04, 0, 1)
        placed.append((1 - b) * near_skin + b * fit["field"](x))
        heights.append(max(h, lowest))
        far.append(b)
    placed = np.array(placed)
    # Where the nearest skin point jumps (a collar in the hollow between neck and shoulders) neighbours move
    # differently: the moves are smoothed over the asset's surface. Near the skin the smoothing only moves along it:
    # vertices go back to their height over the skin (else underwear lifts off), and nothing stays under the skin.
    _, weld = np.unique(np.round(placed / 1e-5).astype(np.int64), axis=0, return_inverse=True)
    weld = weld.ravel()
    move = np.zeros((weld.max() + 1, 3))
    np.add.at(move, weld, placed - posed)
    move /= np.bincount(weld)[:, None]
    pairs = np.unique(np.sort(np.concatenate([weld[atris][:, [0, 1]], weld[atris][:, [1, 2]], weld[atris][:, [2, 0]]]), 1),
                      axis=0)
    for _ in range(6):
        total, count = move.copy(), np.ones(len(move))
        np.add.at(total, pairs[:, 0], move[pairs[:, 1]])
        np.add.at(total, pairs[:, 1], move[pairs[:, 0]])
        np.add.at(count, pairs.ravel(), 1)
        move = 0.5 * move + 0.5 * total / count[:, None]
    placed = posed + move[weld]
    for i, (y, q) in enumerate(zip(placed, qs)):
        if far[i] < 1:
            loc, n, _ = nearest(fit["dst_trees"], "body", y)
            placed[i] = (1 - far[i]) * (loc + n * heights[i]) + far[i] * y
    binding = []
    for i, (y, q) in enumerate(zip(placed, qs)):
        loc, n, tri = nearest(fit["dst_trees"], "body" if far[i] < 1 else q, y)
        if (y - loc) @ n < lowest:
            y = placed[i] = loc + n * lowest
        bw = barycentric(loc, *dst["co"][tri])
        binding.append((tri, bw, (y - dst["co"][tri].T @ bw) / 0.1))
    hide = []
    if entry["kind"] == "clothes" and not entry.get("seeThrough"):
        # Skin the cloth covers closely (none under see-through fabric: fishnet, lace): within 1.5 cm over it along the normal, or up to 2 cm under it, where a coarse
        # garment spans a sharp shape (MB-Lab's nipples) as a chord. Under a loose skirt or a wide trouser hem the
        # skin stays, else one would look into the garment there.
        hide = hidden_skin(dst, placed, atris, border_of(atris, placed), margin=0.004, reach=(0.02, 0.035))
        # Footwear hides the foot below its opening: MB-Lab's toes reach further than the MakeHuman shoes, and above
        # the opening the ankle keeps the shoe's lining out of sight.
        if footwear:
            edge = placed[border_of(atris, placed), 2]
            opening = np.percentile(edge[edge > np.median(placed[:, 2])], 10) if (edge > np.median(placed[:, 2])).any()                 else placed[:, 2].max()
            foot = np.char.startswith(fit["dst_parts"], "foot") & (dst["co"][:, 2] < opening - 0.01)
            hide = sorted(set(hide) | set(np.nonzero(foot)[0].tolist()))
        # Socks, stockings and tights hide the whole foot they cover: made for MakeHuman's shorter foot, they would
        # leave MB-Lab's toes out.
        elif entry.get("slot") == "legwear" and share("foot") > 0.05:
            hide = sorted(set(hide) | set(np.nonzero(np.char.startswith(fit["dst_parts"], "foot"))[0].tolist()))
    with open(os.path.join(out, os.path.basename(entry["file"])), "wb") as f:
        f.write(struct.pack("<I", len(binding)))
        for (tri, bw, off), uv in zip(binding, pv["uv"]):
            f.write(struct.pack("<3I3f3f2f", *map(int, tri), *bw, *off, *uv))
        f.write(struct.pack("<I", len(atris)))
        f.write(np.asarray(atris, np.uint32).tobytes())
        f.write(struct.pack("<B", 0))
        for _ in range(3):
            f.write(struct.pack("<IIf", 0, 0, 1.0))
        f.write(struct.pack("<I", len(hide)))
        f.write(np.asarray(hide, np.uint32).tobytes())
    log(os.path.basename(os.path.dirname(out)), entry["name"], len(binding), "vertices,", len(hide), "skin vertices hidden")
    # The textures stay in the MakeHuman bake; paths are relative to the catalog's directory.
    textures = {k: os.path.relpath(os.path.join(origin, entry[k]), out).replace(os.sep, "/")
                for k in ("albedo", "normal") if entry.get(k)}
    # The engine binds it to the smoothed body it renders (as it does skin assets), so it keeps its height there.
    return dict({k: v for k, v in entry.items() if k != "sex"}, smooth=True, file=os.path.basename(entry["file"]),
                **textures)


src = read_base(MH)
targets = read_targets(MH)
# The MakeHuman bake's own assets and those of its extra/ directory, each with the directory its paths start from.
catalogs = [(MH, src["meta"])]
if os.path.exists(os.path.join(MH, "extra", "human.json")):
    catalogs.append((os.path.join(MH, "extra"), json.load(open(os.path.join(MH, "extra", "human.json"), encoding="utf-8"))))
for sex in ("female", "male"):
    dst = read_base(os.path.join(OUT, sex))
    fit = prepare(src, dst, sex, targets)
    out = os.path.join(OUT, sex, "extra")
    os.makedirs(out, exist_ok=True)
    # PROXIES_ONLY=name,name redoes just these assets, keeping the others of an earlier run (checks).
    only = set(filter(None, os.environ.get("PROXIES_ONLY", "").split(",")))
    keep = []
    if only and os.path.exists(os.path.join(out, "human.json")):
        keep = [a for a in json.load(open(os.path.join(out, "human.json"), encoding="utf-8"))["assets"]
                if a["name"] not in only]
    assets = keep + [transfer(entry, origin, src, dst, fit, out) for origin, catalog in catalogs
                     for entry in catalog["assets"] if entry["kind"] in KINDS and (not only or entry["name"] in only)]
    with open(os.path.join(out, "human.json"), "w", encoding="utf-8") as f:
        json.dump({"assets": assets}, f)
    log(sex, "done:", len(assets), "assets")
