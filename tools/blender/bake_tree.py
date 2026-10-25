# Blender background script: bakes one Poly Haven tree .blend into WorldWalker runtime assets.
#
#   blender -b <file.blend> --python bake_tree.py -- <species_json> <out_dir>
#
# Outputs (in <out_dir>/<species>/):
#   <material>_albedo.png   RGBA8 sRGB, alpha = cutout mask
#   <material>_nrm.png      RG = tangent-space normal (GL), B = roughness
#   <variant>_lod0.wwm      binary mesh (see write_wwm)
#   <variant>_lod1.wwm
#   <variant>_imp_albedo.png / _imp_normal.png   hemi-octahedral impostor atlases
#   species.json            metadata for the engine
import bpy, bmesh, sys, os, json, math, random, struct
import numpy as np
from mathutils import Vector, Matrix

argv = sys.argv[sys.argv.index("--") + 1:]
CFG = json.loads(argv[0])
OUT = os.path.join(argv[1], CFG["species"])
os.makedirs(OUT, exist_ok=True)
TEX_DIR = os.path.join(os.path.dirname(bpy.data.filepath), "textures")

IMP_GRID = 8
IMP_FRAME = 256


def log(*a):
    print("[bake]", *a, flush=True)


# ---------------------------------------------------------------- textures

def find_texture_file(image_name):
    stem = os.path.splitext(image_name)[0]
    for ext in (".png", ".jpg", ".exr"):
        for cand in (stem + "_4k" + ext, stem + ext):
            p = os.path.join(TEX_DIR, cand)
            if os.path.exists(p):
                return p
    return None


def material_images(mat):
    """Classify the image textures referenced by a material by filename suffix."""
    found = {}
    if not mat.node_tree:
        return found
    names = [n.image.name for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image]
    # Prefer images named after the material itself (trunk_a over shared bark).
    names.sort(key=lambda n: 0 if n.startswith(mat.name + "_") else 1)
    for n in names:
        s = os.path.splitext(n)[0]
        for kind, suffix in (("diff", "_diff"), ("alpha", "_alpha"), ("nor", "_nor_gl"), ("rough", "_rough")):
            if s.endswith(suffix) and kind not in found:
                found[kind] = n
    return found


def load_pixels(image_name, size):
    path = find_texture_file(image_name)
    if not path:
        log("missing texture", image_name)
        return None
    img = bpy.data.images.load(path, check_existing=False)
    img.colorspace_settings.name = 'Non-Color'  # read stored values untouched
    img.scale(size, size)
    px = np.empty(size * size * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(size, size, 4)


def save_png(path, px, srgb):
    h, w = px.shape[:2]
    img = bpy.data.images.new("tmp_out", w, h, alpha=True)
    img.colorspace_settings.name = 'sRGB' if srgb else 'Non-Color'
    img.pixels.foreach_set(np.ascontiguousarray(px, dtype=np.float32).ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)


def dilate_rgb(px, iterations):
    """Bleed colour of opaque texels into transparent ones so mipmaps don't fringe."""
    rgb = px[..., :3].copy()
    mask = px[..., 3] > 0.5
    for _ in range(iterations):
        if mask.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(mask.shape, np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            m = np.roll(mask, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * m[..., None]
            cnt += m
        grow = (~mask) & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        mask = mask | grow
    out = px.copy()
    out[..., :3] = rgb
    return out


def bake_material(mat):
    imgs = material_images(mat)
    leaf = "alpha" in imgs
    size = CFG.get("leaf_tex", 1024) if leaf else CFG.get("bark_tex", 512)
    albedo = None
    if "diff" in imgs:
        albedo = load_pixels(imgs["diff"], size)
    if albedo is None:
        albedo = np.ones((size, size, 4), np.float32) * np.array([0.35, 0.28, 0.2, 1.0], np.float32)
    albedo[..., 3] = 1.0
    if leaf:
        a = load_pixels(imgs["alpha"], size)
        if a is not None:
            albedo[..., 3] = a[..., 0]
        albedo = dilate_rgb(albedo, 16)
    nrm = np.zeros((size, size, 4), np.float32)
    nrm[..., 0] = 0.5
    nrm[..., 1] = 0.5
    nrm[..., 2] = 0.8
    nrm[..., 3] = 1.0
    if "nor" in imgs:
        n = load_pixels(imgs["nor"], size)
        if n is not None:
            nrm[..., 0:2] = n[..., 0:2]
    if "rough" in imgs:
        r = load_pixels(imgs["rough"], size)
        if r is not None:
            nrm[..., 2] = r[..., 0]
    save_png(os.path.join(OUT, mat.name + "_albedo.png"), albedo, True)
    save_png(os.path.join(OUT, mat.name + "_nrm.png"), np.clip(nrm, 0, 1), False)
    log("material", mat.name, "leaf" if leaf else "bark", size)
    return {"name": mat.name, "leaf": leaf,
            "albedo": mat.name + "_albedo.png", "normal": mat.name + "_nrm.png"}


# ---------------------------------------------------------------- mesh reduction

def tri_count(me):
    me.calc_loop_triangles()
    return len(me.loop_triangles)


def quadify_leaves(bm, islands, is_leaf, max_faces=48, max_err=0.12):
    """Replaces each small, leaf-only island (one curved leaf / needle card) by a flat quad.

    An affine map uv -> position is fitted by least squares; the quad spans the island's UV
    bounding box, so the texture (and its alpha cutout) is preserved. Islands that are not
    flat enough for the fit (relative error > max_err) are kept as they are."""
    uv_layer = bm.loops.layers.uv.active
    if uv_layer is None:
        return islands
    out, kill = [], []
    for isl in islands:
        tris = sum(len(fa.verts) - 2 for fa in isl)
        if tris <= 2 or len(isl) > max_faces or not all(is_leaf[fa.index] for fa in isl):
            out.append(isl)
            continue
        uvs, pos, nrm = [], [], Vector()
        for fa in isl:
            nrm += fa.normal * fa.calc_area()
            for lp in fa.loops:
                uvs.append(lp[uv_layer].uv[:])
                pos.append(lp.vert.co[:])
        uv = np.array(uvs)
        P = np.array(pos)
        X = np.c_[uv, np.ones(len(uv))]
        A, *_ = np.linalg.lstsq(X, P, rcond=None)
        lo, hi = uv.min(axis=0), uv.max(axis=0)
        corners_uv = np.array([[lo[0], lo[1]], [hi[0], lo[1]], [hi[0], hi[1]], [lo[0], hi[1]]])
        corners = np.c_[corners_uv, np.ones(4)] @ A
        size = np.linalg.norm(corners[2] - corners[0])
        err = np.sqrt(((X @ A - P) ** 2).sum(axis=1).mean())
        # Near-collinear UVs (a twig card mapped onto its stem) make the fit blow up into huge quads showing a
        # smeared strip of texture: only accept quads about as large as the geometry they replace.
        island_area = sum(fa.calc_area() for fa in isl)
        quad_area = np.linalg.norm(np.cross(corners[1] - corners[0], corners[3] - corners[0]))
        if size <= 1e-6 or err > max_err * size or quad_area > 2.0 * island_area:
            out.append(isl)
            continue
        verts = [bm.verts.new(Vector(c)) for c in corners]
        e1, e2 = Vector(corners[1] - corners[0]), Vector(corners[3] - corners[0])
        if e1.cross(e2).dot(nrm) < 0:
            verts.reverse()
            corners_uv = corners_uv[::-1]
        f = bm.faces.new(verts)
        f.material_index = isl[0].material_index
        f.smooth = True
        for lp, c in zip(f.loops, corners_uv):
            lp[uv_layer].uv = (float(c[0]), float(c[1]))
        out.append([f])
        kill.extend(isl)
    if kill:
        bmesh.ops.delete(bm, geom=kill, context='FACES_ONLY')
        bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.faces.ensure_lookup_table()
    bm.faces.index_update()  # note: is_leaf (indexed by old face index) is stale after this
    log("  quadified", len(kill), "leaf faces into", sum(1 for isl in out if len(isl) == 1 and len(isl[0].verts) == 4))
    return out


def ensure_uv_layer(me):
    """Some source files keep their texture coordinates only as a generic corner attribute named "UVMap" (the
    materials read it through an Attribute node). Without a real UV layer the exporter would write (0, 0) for
    every vertex and leaf cards would show a single opaque texel."""
    if me.uv_layers:
        return
    a = me.attributes.get("UVMap")
    if a is None or a.domain != 'CORNER' or a.data_type not in ('FLOAT2', 'FLOAT_VECTOR'):
        return
    n = len(me.loops)
    if a.data_type == 'FLOAT2':
        uv = np.empty(n * 2, np.float32)
        a.data.foreach_get("vector", uv)
    else:
        v = np.empty(n * 3, np.float32)
        a.data.foreach_get("vector", v)
        uv = np.ascontiguousarray(v.reshape(-1, 3)[:, :2]).ravel()
    me.attributes.remove(a)
    me.uv_layers.new(name="UVMap").data.foreach_set("uv", uv)
    log("  converted corner attribute UVMap to a UV layer")


def reduce_mesh(src_obj, target_tris, leaf_mats, seed, min_keep=1.0):
    """Returns a new mesh datablock (object space, origin at tree base) with ~target_tris.

    Small parts (leaf cards, twigs) are thinned out, but never below min_keep of them: past
    that point a tree looks bare, and the engine switches to the impostor earlier instead."""
    me = src_obj.data.copy()
    ensure_uv_layer(me)
    me.transform(Matrix.LocRotScale(None, src_obj.matrix_world.to_quaternion(), src_obj.matrix_world.to_scale()))
    total = tri_count(me)
    if total <= target_tris and not CFG.get("drop_materials"):
        return me

    mats = [m.name if m else "" for m in me.materials]
    bm = bmesh.new()
    bm.from_mesh(me)
    drop = set(CFG.get("drop_materials", []))
    if drop:
        # Parts that are not the tree itself (e.g. the rock the island trees stand on).
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if mats[f.material_index] in drop], context='FACES_ONLY')
        bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.faces.ensure_lookup_table()
    is_leaf = [mats[f.material_index] in leaf_mats if f.material_index < len(mats) else False for f in bm.faces]

    # Connected parts. A part containing any leaf face is a foliage cluster (leaf cards plus
    # their twigs); the largest part is the trunk and is never removed.
    seen = bytearray(len(bm.faces))
    islands = []
    for f in bm.faces:
        if seen[f.index]:
            continue
        island, stack = [], [f]
        seen[f.index] = 1
        while stack:
            cur = stack.pop()
            island.append(cur)
            for e in cur.edges:
                for nf in e.link_faces:
                    if not seen[nf.index]:
                        seen[nf.index] = 1
                        stack.append(nf)
        islands.append(island)
    islands = quadify_leaves(bm, islands, is_leaf)
    total = sum(len(fa.verts) - 2 for isl in islands for fa in isl)

    # Large parts (trunk, main limbs) are structural; every small part (leaf, leaf cluster,
    # twig) can be thinned out.
    structural = max(len(bm.faces) * 0.005, 400)
    foliage = [isl for isl in islands if len(isl) < structural]
    foliage_tris = sum(len(fa.verts) - 2 for isl in foliage for fa in isl)
    other_tris = total - foliage_tris

    # Foliage gets most of the budget: it defines the silhouette.
    leaf_budget = target_tris * 0.75 if foliage_tris else 0

    if foliage_tris > leaf_budget > 0:
        keep = max(leaf_budget / foliage_tris, min_keep)
        rng = random.Random(seed)
        kill = []
        scale = min(1.0 / math.sqrt(keep), 1.3)  # larger leaves look like cardboard up close
        for island in foliage:
            if rng.random() < keep:
                verts = {v for fa in island for v in fa.verts}
                c = sum((v.co for v in verts), Vector()) / len(verts)
                for v in verts:
                    v.co = c + (v.co - c) * scale
            else:
                kill.extend(island)
        bmesh.ops.delete(bm, geom=kill, context='FACES_ONLY')
        loose = [v for v in bm.verts if not v.link_faces]
        bmesh.ops.delete(bm, geom=loose, context='VERTS')

    bm.to_mesh(me)
    bm.free()

    # Decimate everything that is not leaf cards (trunk, branches, twigs). Leaf faces are
    # protected through the vertex group so the cards keep their shape.
    for _ in range(CFG.get("decimate_passes", 0)):
        cur = tri_count(me)
        if cur <= target_tris * 1.1 or other_tris <= 0:
            break
        leaf_now = sum(len(p.vertices) - 2 for p in me.polygons if mats[p.material_index] in leaf_mats)
        bark_now = cur - leaf_now
        bark_target = max(target_tris - leaf_now, target_tris * 0.2)
        # Aggressive collapse turns trunks and limbs into shards: only trim big structures gently.
        if bark_now <= bark_target or bark_now < 20000:
            break
        obj = bpy.data.objects.new("tmp_reduce", me)
        bpy.context.scene.collection.objects.link(obj)
        vg = obj.vertex_groups.new(name="bark")
        bark_verts = set()
        for p in me.polygons:
            if mats[p.material_index] not in leaf_mats:
                bark_verts.update(p.vertices)
        vg.add(list(bark_verts), 1.0, 'REPLACE')
        mod = obj.modifiers.new("dec", 'DECIMATE')
        mod.ratio = max(bark_target / bark_now, 0.4)
        mod.vertex_group = "bark"
        mod.use_collapse_triangulate = True
        dg = bpy.context.evaluated_depsgraph_get()
        out = bpy.data.meshes.new_from_object(obj.evaluated_get(dg))
        bpy.data.objects.remove(obj)
        me = out
    log("  reduced", src_obj.name, total, "->", tri_count(me))
    return me


# ---------------------------------------------------------------- export

def write_wwm(path, me, mat_index_of):
    """WWM1: header, submeshes, vertices (pos3 nrm3 uv2 float), u32 indices sorted by material."""
    me.calc_loop_triangles()
    nl = len(me.loops)
    vidx = np.empty(nl, np.int32); me.loops.foreach_get("vertex_index", vidx)
    co = np.empty(len(me.vertices) * 3, np.float32); me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    nrm = np.empty(nl * 3, np.float32); me.corner_normals.foreach_get("vector", nrm)
    nrm = nrm.reshape(-1, 3)
    uv = np.zeros((nl, 2), np.float32)
    if me.uv_layers:
        layer = me.uv_layers.active or me.uv_layers[0]
        t = np.empty(nl * 2, np.float32); layer.data.foreach_get("uv", t)
        uv = t.reshape(-1, 2)
        uv[:, 1] = 1.0 - uv[:, 1]  # Vulkan texture origin is top-left
    nt = len(me.loop_triangles)
    tl = np.empty(nt * 3, np.int32); me.loop_triangles.foreach_get("loops", tl)
    tl = tl.reshape(-1, 3)
    tm = np.empty(nt, np.int32); me.loop_triangles.foreach_get("material_index", tm)
    gm = np.array([mat_index_of.get(m.name if m else "", 0) for m in me.materials] or [0], np.int32)
    tm = gm[np.clip(tm, 0, len(gm) - 1)]

    verts = np.concatenate([co[vidx], nrm, uv], axis=1)
    verts = np.round(verts, 5)
    uniq, inv = np.unique(verts, axis=0, return_inverse=True)
    inv = inv.reshape(-1).astype(np.uint32)

    order = np.argsort(tm, kind="stable")
    tris = inv[tl[order]]
    tm = tm[order]
    subs = []
    for m in np.unique(tm):
        idx = np.nonzero(tm == m)[0]
        subs.append((int(idx[0]) * 3, len(idx) * 3, int(m)))

    lo = uniq[:, 0:3].min(axis=0); hi = uniq[:, 0:3].max(axis=0)
    with open(path, "wb") as f:
        f.write(b"WWM1")
        f.write(struct.pack("<III", len(uniq), tris.size, len(subs)))
        f.write(struct.pack("<6f", *lo, *hi))
        for s in subs:
            f.write(struct.pack("<III", *s))
        f.write(uniq.astype(np.float32).tobytes())
        f.write(tris.astype(np.uint32).tobytes())
    return int(tris.size // 3)


# ---------------------------------------------------------------- impostors

def hemi_oct_decode(u, v):
    """[-1,1]^2 grid coordinate -> unit direction on upper hemisphere (must match impostor.glsl)."""
    px, py = (u + v) * 0.5, (u - v) * 0.5
    z = 1.0 - abs(px) - abs(py)
    return Vector((px, py, z)).normalized()


def frame_basis(d):
    f = -d
    if abs(d.z) < 0.999:
        right = f.cross(Vector((0, 0, 1))).normalized()
    else:
        right = Vector((1, 0, 0))
    up = right.cross(f).normalized()
    return right, up


def make_imp_material(src_name, info, mode):
    m = bpy.data.materials.new("imp_%s_%s" % (mode, src_name))
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emit = nt.nodes.new("ShaderNodeEmission")
    transp = nt.nodes.new("ShaderNodeBsdfTransparent")
    mix = nt.nodes.new("ShaderNodeMixShader")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(os.path.join(OUT, info["albedo"]), check_existing=True)
    tex.image.alpha_mode = 'STRAIGHT'
    nt.links.new(tex.outputs["Alpha"], mix.inputs["Fac"])
    nt.links.new(transp.outputs[0], mix.inputs[1])
    nt.links.new(emit.outputs[0], mix.inputs[2])
    nt.links.new(mix.outputs[0], out.inputs["Surface"])
    if mode == "albedo":
        nt.links.new(tex.outputs["Color"], emit.inputs["Color"])
    else:
        geo = nt.nodes.new("ShaderNodeNewGeometry")
        flip = nt.nodes.new("ShaderNodeMix"); flip.data_type = 'VECTOR'
        neg = nt.nodes.new("ShaderNodeVectorMath"); neg.operation = 'SCALE'; neg.inputs["Scale"].default_value = -1
        enc = nt.nodes.new("ShaderNodeVectorMath"); enc.operation = 'MULTIPLY_ADD'
        enc.inputs[1].default_value = (0.5, 0.5, 0.5); enc.inputs[2].default_value = (0.5, 0.5, 0.5)
        nt.links.new(geo.outputs["Normal"], neg.inputs[0])
        nt.links.new(geo.outputs["Backfacing"], flip.inputs["Factor"])
        nt.links.new(geo.outputs["Normal"], flip.inputs[4])
        nt.links.new(neg.outputs[0], flip.inputs[5])
        nt.links.new(flip.outputs[1], enc.inputs[0])
        nt.links.new(enc.outputs[0], emit.inputs["Color"])
    if hasattr(m, "surface_render_method"):
        m.surface_render_method = 'DITHERED'
    else:
        m.blend_method = 'HASHED'
    return m


def setup_render_scene():
    sc = bpy.data.scenes.new("impostor")
    for eng in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            sc.render.engine = eng
            break
        except TypeError:
            pass
    sc.render.resolution_x = sc.render.resolution_y = IMP_FRAME
    sc.render.resolution_percentage = 100
    sc.render.film_transparent = True
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGBA'
    sc.render.filter_size = 0.75
    if hasattr(sc, "eevee"):
        sc.eevee.taa_render_samples = 32
    sc.world = bpy.data.worlds.new("imp_world")
    sc.world.use_nodes = True
    bg = sc.world.node_tree.nodes.get("Background")
    if bg:
        bg.inputs["Strength"].default_value = 0.0
    cam_data = bpy.data.cameras.new("imp_cam")
    cam_data.type = 'ORTHO'
    cam = bpy.data.objects.new("imp_cam", cam_data)
    sc.collection.objects.link(cam)
    sc.camera = cam
    return sc, cam


def bake_impostor(variant, me, materials_info, sc, cam):
    co = np.empty(len(me.vertices) * 3, np.float32); me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    zmin, zmax = float(co[:, 2].min()), float(co[:, 2].max())
    center = Vector((0.0, 0.0, (zmin + zmax) * 0.5))
    radius = float(np.sqrt(((co - np.array(center)) ** 2).sum(axis=1)).max()) * 1.02
    cam.data.ortho_scale = radius * 2
    cam.data.clip_start = 0.01
    cam.data.clip_end = radius * 6

    obj = bpy.data.objects.new("imp_" + variant, me)
    sc.collection.objects.link(obj)
    atlases = {}
    for mode in ("albedo", "normal"):
        by_name = {mi["name"]: mi for mi in materials_info}
        orig = [s.material for s in obj.material_slots]
        for i, m in enumerate(me.materials):
            if m and m.name in by_name:
                me.materials[i] = make_imp_material(m.name, by_name[m.name], mode)
        sc.view_settings.view_transform = 'Standard' if mode == "albedo" else 'Raw'
        atlas = np.zeros((IMP_GRID * IMP_FRAME, IMP_GRID * IMP_FRAME, 4), np.float32)
        tmp = os.path.join(OUT, "_frame.png")
        for j in range(IMP_GRID):
            for i in range(IMP_GRID):
                u = (i + 0.5) / IMP_GRID * 2 - 1
                v = (j + 0.5) / IMP_GRID * 2 - 1
                d = hemi_oct_decode(u, v)
                right, up = frame_basis(d)
                m = Matrix.Identity(4)
                m.col[0][:3] = right
                m.col[1][:3] = up
                m.col[2][:3] = d
                m.col[3][:3] = center + d * radius * 3
                cam.matrix_world = m
                sc.render.filepath = tmp
                bpy.ops.render.render(write_still=True, scene=sc.name)
                img = bpy.data.images.load(tmp, check_existing=False)
                img.colorspace_settings.name = 'Non-Color'
                px = np.empty(IMP_FRAME * IMP_FRAME * 4, np.float32)
                img.pixels.foreach_get(px)
                bpy.data.images.remove(img)
                # Blender pixel rows are bottom-up; atlas row j is stored top-down in the PNG
                # after save_png flips again, so keep Blender's convention here.
                y0 = (IMP_GRID - 1 - j) * IMP_FRAME
                atlas[y0:y0 + IMP_FRAME, i * IMP_FRAME:(i + 1) * IMP_FRAME] = px.reshape(IMP_FRAME, IMP_FRAME, 4)
        os.remove(tmp)
        for i, m in enumerate(orig):
            me.materials[i] = m
        if mode == "albedo":
            atlas = dilate_rgb(atlas, 8)
        else:
            atlas[..., 3] = atlases["albedo_alpha"]
        atlases["albedo_alpha"] = atlas[..., 3].copy()
        save_png(os.path.join(OUT, "%s_imp_%s.png" % (variant, mode)), np.clip(atlas, 0, 1), mode == "albedo")
    bpy.data.objects.remove(obj)
    return {"albedo": variant + "_imp_albedo.png", "normal": variant + "_imp_normal.png",
            "grid": IMP_GRID, "radius": radius, "center_z": center.z}


# ---------------------------------------------------------------- main

def main():
    mats_used = []
    for v in CFG["variants"]:
        for src in (v["lod0"], v["lod1"]):
            for m in bpy.data.objects[src].data.materials:
                if m and m.name not in mats_used and m.name not in CFG.get("drop_materials", []):
                    mats_used.append(m.name)
    materials = [bake_material(bpy.data.materials[n]) for n in mats_used]
    mat_index_of = {m["name"]: i for i, m in enumerate(materials)}
    leaf_mats = {m["name"] for m in materials if m["leaf"]}

    sc, cam = setup_render_scene()
    variants = []
    for vi, v in enumerate(CFG["variants"]):
        name = v["name"]
        log("variant", name)
        me0 = reduce_mesh(bpy.data.objects[v["lod0"]], CFG["lod0_tris"], leaf_mats, vi * 7 + 1, CFG.get("min_keep", [0.5, 0.25])[0])
        me1 = reduce_mesh(bpy.data.objects[v["lod1"]], CFG["lod1_tris"], leaf_mats, vi * 7 + 2, CFG.get("min_keep", [0.5, 0.25])[1])
        # Impostors are rendered from the full source mesh: they cost nothing at runtime.
        full = reduce_mesh(bpy.data.objects[v.get("impostor", v["lod0"])], 10 ** 9, leaf_mats, 0)
        t0 = write_wwm(os.path.join(OUT, name + "_lod0.wwm"), me0, mat_index_of)
        t1 = write_wwm(os.path.join(OUT, name + "_lod1.wwm"), me1, mat_index_of)
        imp = bake_impostor(name, full, materials, sc, cam)
        co = np.empty(len(me0.vertices) * 3, np.float32); me0.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        variants.append({
            "name": name,
            "height": float(co[:, 2].max()),
            "crown_radius": float(np.sqrt((co[:, :2] ** 2).sum(axis=1)).max()),
            "lods": [{"mesh": name + "_lod0.wwm", "tris": t0}, {"mesh": name + "_lod1.wwm", "tris": t1}],
            "impostor": imp,
        })
    meta = {"species": CFG["species"], "biomes": CFG["biomes"], "materials": materials, "variants": variants}
    with open(os.path.join(OUT, "species.json"), "w") as f:
        json.dump(meta, f, indent=2)
    log("done", CFG["species"])


main()
