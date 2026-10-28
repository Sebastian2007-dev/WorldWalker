# Shared by the MB-Lab transfer scripts (bake_mblab_genitals.py, bake_mblab_proxies.py): reading the bakes of
# bake_human.py / bake_mblab.py, alignment and binding helpers, and the body skin an asset hides.
import json, os, struct
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

PROXY = np.dtype([("r", "<u4", 3), ("w", "<f4", 3), ("o", "<f4", 3), ("uv", "<f4", 2)])


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
    weights = take("u1", n * 4).reshape(-1, 4)
    names = [bone["name"] for bone in meta["bones"]]
    joint = {bone["name"]: co[bone["head"]].mean(0) for bone in meta["bones"]}
    return {"meta": meta, "co": co, "rv": rv, "rtris": rtris, "tris": rv["v"][rtris].astype(np.int64),
            "dominant": np.array([names[j] for j in joints[:, 0]]), "joint": joint,
            "joints": joints.astype(np.int64), "weights": weights.astype(np.float64) / 255.0}


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


def border_of(tris, points=None):
    """Vertices on the open edges of the triangles. With their points, vertices at the same place count as one (the
    assets are split along their uv seams, which are no edges of the surface)."""
    if points is not None:
        _, weld = np.unique(np.round(np.asarray(points) / 1e-5).astype(np.int64), axis=0, return_inverse=True)
        weld = weld.ravel()
        border = set(border_of(weld[np.asarray(tris)]).tolist())
        return np.array([i for i in range(len(weld)) if weld[i] in border], np.int64)
    edges = {}
    for tri in tris:
        for k in range(3):
            e = tuple(sorted((int(tri[k]), int(tri[(k + 1) % 3]))))
            edges[e] = edges.get(e, 0) + 1
    return np.unique([v for e, c in edges.items() if c == 1 for v in e]).astype(np.int64)


def hidden_skin(dst, placed, atris, border, margin=0.006, reach=(0.03, 0.06)):
    """Body vertices under the asset. Parts of the asset lie below the skin (the female's are sunk up to 15 mm into
    it), so 'under' means: the line along the body normal meets the asset at least `margin` inside its border. The
    engine hides every face around a hidden vertex, so a vertex is hidden only when its neighbours are covered too;
    the hole then stays inside the asset's skin rim. reach: the line starts this far under the skin and is this long."""
    dco, tris = dst["co"], dst["tris"]
    fn = np.cross(dco[tris[:, 1]] - dco[tris[:, 0]], dco[tris[:, 2]] - dco[tris[:, 0]])
    vn = np.zeros_like(dco)
    for k in range(3):
        np.add.at(vn, tris[:, k], fn)
    vn /= np.maximum(np.linalg.norm(vn, axis=1), 1e-12)[:, None]
    asset = BVHTree.FromPolygons([Vector(x) for x in placed], [tuple(map(int, x)) for x in atris])
    border_tree = kdtree(placed[border]) if len(border) else None  # closed meshes (rings) have no border
    lo, hi = placed.min(0) - 0.03, placed.max(0) + 0.03
    candidates = np.nonzero(((dco > lo) & (dco < hi)).all(1))[0]
    covered = np.zeros(len(dco), bool)
    for i in candidates:
        o, n = Vector(dco[i] - vn[i] * reach[0]), Vector(vn[i])
        loc = asset.ray_cast(o, n, reach[1])[0]
        covered[i] = loc is not None and (border_tree is None or border_tree.find(loc)[2] > margin)
    face = quads(dst)
    corners = [set() for _ in range(face.max() + 1)]
    for f, t in zip(face, tris):
        corners[f].update(int(a) for a in t)
    neighbours = [set() for _ in dco]
    for c in corners:
        for a in c:
            neighbours[a].update(c)
    return [int(i) for i in candidates if covered[i] and all(covered[j] for j in neighbours[i])]
