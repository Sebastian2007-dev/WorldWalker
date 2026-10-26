# Blender background script: transfers the sculpted anatomy of Blender Studio's realistic base meshes (CC0, "Human
# Base Meshes" asset bundle) onto WorldWalker's MakeHuman body:
#
#   blender -b --python tools/blender/bake_realism.py -- <human_base_meshes_bundle.blend> <out_dir>
#
# Bundle: https://www.blender.org/download/demo/ (Asset Bundles -> Human Base Meshes, CC0).
# Per sex (male / female), with the MakeHuman body in the creator template's shape:
#   1. The sculpt is scaled to the body's height and placed on it.
#   2. Trunk and legs are wrapped onto the sculpt (ray along the vertex normal, displacement smoothed over the
#      surface). Head, arms, hands and feet keep their MakeHuman shape: the sculpt's arms hang at another angle and
#      its face is a different person. The wrap's displacement becomes the "definition" morph.
#   3. The sculpt's full multiresolution detail is baked as a tangent space normal map onto the MakeHuman UVs, faded
#      out with the same region mask.
# Outputs: realism.bin (the morphs, targets.bin format), tex/realism_<sex>_nrm.png, and "realism" in human.json.
# Run after bake_human.py.
import bpy, sys, os, json, gzip, struct
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index("--") + 1:]
BUNDLE, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])

from bl_ext.user_default.mpfb.services.objectservice import ObjectService
from bl_ext.user_default.mpfb.services.locationservice import LocationService
from bl_ext.user_default.mpfb.services.humanservice import HumanService

TARGETS = os.path.join(LocationService.get_mpfb_data(""), "targets")
FACE = np.array([-1.0, -1.0, 1.0])  # MPFB humans look along -y; model space looks along +y
TEX = 2048
SEXES = {"male": ("GEO-body_male_realistic", ["universal-male-young-averagemuscle-averageweight", "caucasian-male-young"]),
         "female": ("GEO-body_female_realistic", ["universal-female-young-averagemuscle-averageweight", "caucasian-female-young"])}
# Mixamo bones whose skin takes the sculpt's shape (summed weight = mask before smoothing).
# Knees and lower legs stay out: the sculpts stand with their legs at another angle.
WRAPPED = ("Hips", "Spine", "Spine1", "Spine2", "LeftUpLeg", "RightUpLeg", "LeftShoulder", "RightShoulder")
RAY = 0.035  # metres; longer rays reach the sculpt's arms from the chest


def log(*a):
    print("[realism]", *a, flush=True)


def read_target(path):
    idx, d = [], []
    with gzip.open(path, "rt") as f:
        for line in f:
            w = line.split()
            if len(w) == 4 and not line.startswith("#"):
                idx.append(int(w[0]))
                d.append((float(w[1]), -float(w[3]), float(w[2])))
    return np.asarray(idx, np.int64), np.asarray(d, np.float64).reshape(-1, 3) * 0.1


def positions(me):
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    return co.reshape(-1, 3).astype(np.float64)


def set_positions(me, co):
    me.vertices.foreach_set("co", co.astype(np.float32).reshape(-1))
    me.update()


def neighbours(me):
    adj = [[] for _ in me.vertices]
    for e in me.edges:
        a, b = e.vertices
        adj[a].append(b)
        adj[b].append(a)
    return adj


def smooth_field(field, adj, active, passes):
    """Laplacian smoothing of a per-vertex field over the mesh, restricted to active vertices."""
    for _ in range(passes):
        nxt = field.copy()
        for v in np.where(active)[0]:
            if adj[v]:
                nxt[v] = 0.5 * field[v] + 0.5 * field[adj[v]].mean(axis=0)
        field = nxt
    return field


def load_sculpt(name):
    with bpy.data.libraries.load(BUNDLE) as (src, dst):
        dst.objects = [name]
    obj = dst.objects[0]
    bpy.context.scene.collection.objects.link(obj)
    mw = obj.matrix_world.copy()
    obj.parent = None
    obj.matrix_world = mw
    return obj


def evaluated_copy(obj, name):
    ev = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    me = bpy.data.meshes.new_from_object(ev)
    me.transform(obj.matrix_world)
    copy = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(copy)
    return copy


def process(sex):
    sculpt_name, macros = SEXES[sex]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    body = ObjectService.load_base_mesh(context=bpy.context, scale_factor=0.1, load_vertex_groups=True)
    me = body.data
    base = positions(me)
    co = base.copy()
    for m in macros:
        idx, d = read_target(os.path.join(TARGETS, "macrodetails", m + ".target.gz"))
        co[idx] += d
    if me.shape_keys:
        body.shape_key_clear()
    set_positions(me, co)
    rig = HumanService.add_builtin_rig(body, "mixamo", import_weights=True)
    n = len(me.vertices)
    gi = body.vertex_groups["body"].index
    in_body = np.array([any(g.group == gi for g in v.groups) for v in me.vertices])
    wrapped = {body.vertex_groups[g].index for g in body.vertex_groups.keys()
               if g.split(":")[-1] in WRAPPED}
    mask = np.array([[sum(g.weight for g in v.groups if g.group in wrapped)] for v in me.vertices])
    mask = np.clip(mask, 0, 1) * in_body[:, None]
    adj = neighbours(me)
    # Hard bone boundaries would leave a step at the shoulders and neck: fade over a few rings.
    mask = smooth_field(np.clip((mask - 0.3) / 0.6, 0, 1), adj, in_body, 12)[:, 0]

    # 1. Place the sculpt: same height, feet on the same ground, trunks centred on each other.
    sculpt = load_sculpt(sculpt_name)
    multires = next(m for m in sculpt.modifiers if m.type == "MULTIRES")
    multires.levels = 1
    low = evaluated_copy(sculpt, "sculpt_low")
    sv = positions(low.data)
    mh = co[in_body]
    scale = (mh[:, 2].max() - mh[:, 2].min()) / (sv[:, 2].max() - sv[:, 2].min())

    def trunk_centre(p):
        lo, hi = p[:, 2].min(), p[:, 2].max()
        band = p[(p[:, 2] > lo + 0.5 * (hi - lo)) & (p[:, 2] < lo + 0.6 * (hi - lo)) & (np.abs(p[:, 0]) < 0.2 * (hi - lo))]
        return np.array([0.0, band[:, 1].mean(), lo])

    offset = trunk_centre(mh) - trunk_centre(sv * scale)
    place = lambda p: p * scale + offset
    set_positions(low.data, place(sv))
    log(sex, "sculpt scale %.3f" % scale)

    # 2. Wrap: rays along the MakeHuman normal both ways, nearest hit within RAY, else no change.
    tree = BVHTree.FromObject(low, bpy.context.evaluated_depsgraph_get())
    normals = np.empty(n * 3, np.float32)
    me.vertices.foreach_get("normal", normals)
    normals = normals.reshape(-1, 3)
    disp = np.zeros((n, 3))
    hit = np.zeros(n, bool)
    for v in np.where(mask > 0.01)[0]:
        p, nv = Vector(co[v]), Vector(normals[v])
        best = None
        for dirn in (nv, -nv):
            loc, _, _, dist = tree.ray_cast(p, dirn, RAY)
            if loc is not None and (best is None or dist < best[1]):
                best = (loc, dist)
        if best:
            disp[v] = np.array(best[0]) - co[v]
            hit[v] = True
    log(sex, "wrapped", int(hit.sum()), "of", int((mask > 0.01).sum()), "vertices")
    # Missed vertices take their neighbours' displacement, then the field is smoothed against ray noise.
    for _ in range(30):
        missing = np.where(~hit & (mask > 0.01))[0]
        if not len(missing):
            break
        for v in missing:
            got = [u for u in adj[v] if hit[u]]
            if got:
                disp[v] = disp[got].mean(axis=0)
                hit[v] = True
    disp = smooth_field(disp, adj, mask > 0.01, 10) * mask[:, None]
    delta = disp * FACE
    idx = np.where(np.abs(delta).max(axis=1) > 1e-6)[0]

    # 3. Normal map: low = MakeHuman body in the wrapped shape, high = the sculpt at full resolution.
    set_positions(me, co + disp)
    lowbody = bpy.data.objects.new("mh_low", me.copy())
    bpy.context.scene.collection.objects.link(lowbody)
    body.hide_render = rig.hide_render = True
    import bmesh
    b = bmesh.new()
    b.from_mesh(lowbody.data)
    b.verts.ensure_lookup_table()
    # The region mask goes along as a colour attribute, baked to a second image (set before the helper geometry is
    # deleted, while vertex indices still match).
    layer = b.loops.layers.color.new("mask")
    for f in b.faces:
        f.smooth = True
        for loop in f.loops:
            w = float(mask[loop.vert.index])
            loop[layer] = (w, w, w, 1.0)
    bmesh.ops.delete(b, geom=[v for v in b.verts if not in_body[v.index]], context="VERTS")
    b.to_mesh(lowbody.data)
    b.free()
    # The engine renders the body after one Catmull-Clark step with linear UVs (Human::subdivide); bake onto the same.
    subd = lowbody.modifiers.new("subd", "SUBSURF")
    subd.levels = subd.render_levels = 1
    subd.uv_smooth = "NONE"
    multires.levels = multires.render_levels = multires.sculpt_levels = multires.total_levels
    high = evaluated_copy(sculpt, "sculpt_high")
    set_positions(high.data, place(positions(high.data)))
    for o in (sculpt, low):
        o.hide_render = True
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.samples = 1
    sc.render.bake.use_selected_to_active = True
    sc.render.bake.cage_extrusion = 0.012
    sc.render.bake.max_ray_distance = 0.03
    sc.render.bake.margin = 8

    def bake(kind, name, colorspace):
        img = bpy.data.images.new(name, TEX, TEX, float_buffer=False)
        img.colorspace_settings.name = colorspace
        img.generated_color = (0.5, 0.5, 1.0, 1.0)
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = img
        mat.node_tree.nodes.active = node
        lowbody.data.materials.clear()
        lowbody.data.materials.append(mat)
        if kind == "NORMAL":
            sc.render.bake.normal_space = "TANGENT"
            bpy.ops.object.select_all(action="DESELECT")
            high.select_set(True)
            lowbody.select_set(True)
            sc.render.bake.use_selected_to_active = True
        else:
            # The mask: emission of the colour attribute, baked from the low mesh itself.
            nodes = mat.node_tree.nodes
            attr = nodes.new("ShaderNodeVertexColor")
            attr.layer_name = "mask"
            emit = nodes.new("ShaderNodeEmission")
            mat.node_tree.links.new(attr.outputs["Color"], emit.inputs["Color"])
            mat.node_tree.links.new(emit.outputs["Emission"], nodes["Material Output"].inputs["Surface"])
            bpy.ops.object.select_all(action="DESELECT")
            lowbody.select_set(True)
            sc.render.bake.use_selected_to_active = False
        bpy.context.view_layer.objects.active = lowbody
        bpy.ops.object.bake(type=kind)
        return np.array(img.pixels[:]).reshape(TEX, TEX, 4)

    normal = bake("NORMAL", "nrm_" + sex, "Non-Color")
    region = bake("EMIT", "mask_" + sex, "Non-Color")[:, :, :1]
    flat = np.array([0.5, 0.5, 1.0, 1.0])
    out = flat + (normal - flat) * region
    img = bpy.data.images.new("out_" + sex, TEX, TEX)
    img.colorspace_settings.name = "Non-Color"
    img.pixels[:] = out.reshape(-1)
    rel = "tex/realism_%s_nrm.png" % sex
    img.filepath_raw = os.path.join(OUT, rel)
    img.file_format = "PNG"
    img.save()
    log(sex, "normal map", rel, "morph", len(idx), "vertices, max %.1f mm" % (np.abs(delta).max() * 1000))
    return rel, idx, delta[idx]


def main():
    results = {sex: process(sex) for sex in SEXES}
    # realism.bin: targets.bin format (u32 T; T x (u32 n, n x u32 base vertex, n x float3 delta in metres)).
    with open(os.path.join(OUT, "realism.bin"), "wb") as f:
        f.write(struct.pack("<I", len(results)))
        for rel, idx, d in results.values():
            f.write(struct.pack("<I", len(idx)))
            f.write(np.asarray(idx, np.uint32).tobytes())
            f.write(np.asarray(d, np.float32).tobytes())
    meta_path = os.path.join(OUT, "human.json")
    meta = json.load(open(meta_path))
    meta["realism"] = [{"sex": sex, "target": i, "normal": rel} for i, (sex, (rel, _, _)) in enumerate(results.items())]
    with open(meta_path, "w") as f:
        json.dump(meta, f)
    log("done")


main()
