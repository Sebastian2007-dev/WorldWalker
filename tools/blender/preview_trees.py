# Renders a contact sheet of baked tree meshes (exactly what the engine loads) for review.
#   blender -b --python preview_trees.py -- <baked_trees_dir> <out.png> [species ...]
import bpy, sys, os, json, struct, math
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
ROOT, OUT_PNG = os.path.abspath(argv[0]), os.path.abspath(argv[1])
ONLY = set(argv[2:])
CELL = 384


def load_wwm(path):
    with open(path, "rb") as f:
        assert f.read(4) == b"WWM1"
        nv, ni, ns = struct.unpack("<III", f.read(12))
        f.read(24)
        subs = [struct.unpack("<III", f.read(12)) for _ in range(ns)]
        v = np.frombuffer(f.read(nv * 32), np.float32).reshape(nv, 8)
        idx = np.frombuffer(f.read(ni * 4), np.uint32).reshape(-1, 3)
    return v, idx, subs


def make_mesh(name, v, idx, subs, mats):
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(v))
    me.vertices.foreach_set("co", v[:, 0:3].ravel())
    me.loops.add(idx.size)
    me.loops.foreach_set("vertex_index", idx.ravel().astype(np.int32))
    me.polygons.add(len(idx))
    me.polygons.foreach_set("loop_start", np.arange(0, idx.size, 3, dtype=np.int32))
    me.polygons.foreach_set("loop_total", np.full(len(idx), 3, np.int32))
    mat_idx = np.zeros(len(idx), np.int32)
    used = []
    for first, count, m in subs:
        if m not in used:
            used.append(m)
        mat_idx[first // 3:(first + count) // 3] = used.index(m)
    for m in used:
        me.materials.append(mats[m])
    me.polygons.foreach_set("material_index", mat_idx)
    uv = me.uv_layers.new()
    uvs = v[idx.ravel(), 6:8].copy()
    uvs[:, 1] = 1.0 - uvs[:, 1]
    uv.data.foreach_set("uv", uvs.ravel())
    me.update()
    return me


def make_material(sdir, info):
    m = bpy.data.materials.new(info["name"])
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(os.path.join(sdir, info["albedo"]))
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    if info["leaf"]:
        nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
        if hasattr(m, "surface_render_method"):
            m.surface_render_method = 'DITHERED'
    bsdf.inputs["Roughness"].default_value = 0.8
    return m


scene = bpy.context.scene
for eng in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
    try:
        scene.render.engine = eng
        break
    except TypeError:
        pass
scene.render.resolution_x = scene.render.resolution_y = CELL
scene.render.film_transparent = False
scene.view_settings.view_transform = 'Standard'
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
world = bpy.data.worlds.new("w"); scene.world = world; world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.55, 0.65, 0.8, 1)
world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.6
sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", 'SUN'))
sun.data.energy = 3.5
sun.rotation_euler = (math.radians(50), 0, math.radians(30))
scene.collection.objects.link(sun)
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
cam.data.lens = 50
scene.collection.objects.link(cam)
scene.camera = cam

index = json.load(open(os.path.join(ROOT, "index.json")))
tiles = []
for sp in index["species"]:
    if ONLY and sp not in ONLY:
        continue
    sdir = os.path.join(ROOT, sp)
    meta = json.load(open(os.path.join(sdir, "species.json")))
    mats = [make_material(sdir, mi) for mi in meta["materials"]]
    for var in meta["variants"]:
        for li, lod in enumerate(var["lods"]):
            v, idx, subs = load_wwm(os.path.join(sdir, lod["mesh"]))
            obj = bpy.data.objects.new("t", make_mesh("t", v, idx, subs, mats))
            scene.collection.objects.link(obj)
            h = float(v[:, 2].max())
            r = max(h, float(np.abs(v[:, 0:2]).max()) * 2)
            d = r * 2.2
            cam.location = Vector((d * 0.8, -d * 0.6, h * 0.45))
            cam.rotation_euler = (Vector((0, 0, h * 0.45)) - cam.location).to_track_quat('-Z', 'Y').to_euler()
            path = os.path.join(os.path.dirname(OUT_PNG), "_tile.png")
            scene.render.filepath = path
            bpy.ops.render.render(write_still=True)
            img = bpy.data.images.load(path)
            px = np.array(img.pixels[:], np.float32).reshape(CELL, CELL, 4)
            bpy.data.images.remove(img)
            tiles.append((f"{var['name']} L{li} {lod['tris']}", px))
            bpy.data.objects.remove(obj)
            print("[preview]", var["name"], li, lod["tris"], flush=True)

cols = 6
rows = (len(tiles) + cols - 1) // cols
sheet = np.ones((rows * CELL, cols * CELL, 4), np.float32)
for i, (_, px) in enumerate(tiles):
    r, c = divmod(i, cols)
    y0 = (rows - 1 - r) * CELL
    sheet[y0:y0 + CELL, c * CELL:(c + 1) * CELL] = px
out = bpy.data.images.new("sheet", cols * CELL, rows * CELL, alpha=True)
out.pixels.foreach_set(sheet.ravel())
out.filepath_raw = OUT_PNG
out.file_format = 'PNG'
out.save()
print("[preview] labels:", " | ".join(t[0] for t in tiles))
