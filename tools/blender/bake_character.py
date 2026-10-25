# Blender background script: bakes a Mixamo character and its animation clips into WorldWalker runtime assets.
#
#   blender -b --python bake_character.py -- <character.fbx> <anim_dir> <out_dir>
#
# Outputs (in <out_dir>):
#   character.wwc            skinned mesh + skeleton (see write_character)
#   <clip>.wwa               animation clip: local bone transforms per frame (see write_clip)
#   <material>_albedo.png    RGBA8 sRGB, alpha = cutout mask
#   <material>_nrm.png       RG = tangent-space normal (GL), B = roughness
#   character.json           metadata for the engine
#
# Model space matches the trees: metres, z up, origin between the feet, the character looks along +y.
import bpy, sys, os, json, glob, struct, re
import numpy as np
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
CHAR_FBX, ANIM_DIR, OUT = argv[0], argv[1], argv[2]
os.makedirs(OUT, exist_ok=True)
MAX_INFLUENCES = 4
BODY_TEX, HAIR_TEX = 2048, 1024


def log(*a):
    print("[char]", *a, flush=True)


def import_fbx(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=path)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    return arm


def bone_key(name):
    """Mixamo prefixes bones per export (mixamorig:, mixamorig2:, ...); match clips by the bare name."""
    return name.split(":")[-1]


# Mixamo characters look along -y after import; the engine wants +y.
FACE = Matrix.Rotation(np.pi, 4, 'Z')


def unscaled(m):
    """Bone frame without the importer's centimetre scale: rotation + translation in metres."""
    return Matrix.Translation(m.translation) @ m.to_3x3().normalized().to_4x4()


def bone_order(arm):
    """Parents before children (the runtime evaluates the hierarchy in array order)."""
    order, seen = [], set()

    def visit(b):
        order.append(b)
        seen.add(b.name)
        for c in b.children:
            visit(c)
    for b in arm.data.bones:
        if b.parent is None:
            visit(b)
    return order


# ---------------------------------------------------------------- textures

def image_pixels(img, size):
    img = img.copy()
    img.colorspace_settings.name = 'Non-Color'  # stored values untouched
    img.scale(size, size)
    px = np.empty(size * size * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(size, size, 4)


def save_png(path, px, srgb):
    h, w = px.shape[:2]
    img = bpy.data.images.new("tmp_out", w, h, alpha=True)
    img.colorspace_settings.name = 'sRGB' if srgb else 'Non-Color'
    img.pixels.foreach_set(np.ascontiguousarray(np.clip(px, 0, 1), dtype=np.float32).ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)


def bake_material(mat, name, size):
    """Collects the textures wired into the Principled BSDF and writes albedo(+alpha) and normal(+roughness)."""
    tex = {}
    for n in mat.node_tree.nodes:
        if n.type != 'TEX_IMAGE' or not n.image:
            continue
        for o in n.outputs:
            for l in o.links:
                if l.to_node.type == 'BSDF_PRINCIPLED':
                    tex[l.to_socket.name] = n.image
                elif l.to_node.type == 'NORMAL_MAP':
                    tex['Normal'] = n.image
    albedo = image_pixels(tex['Base Color'], size) if 'Base Color' in tex else np.full((size, size, 4), 0.5, np.float32)
    albedo[..., 3] = image_pixels(tex['Alpha'], size)[..., 0] if 'Alpha' in tex else 1.0
    nrm = np.zeros((size, size, 4), np.float32)
    nrm[..., 0:2] = image_pixels(tex['Normal'], size)[..., 0:2] if 'Normal' in tex else 0.5
    nrm[..., 2] = image_pixels(tex['Roughness'], size)[..., 0] if 'Roughness' in tex else 0.6
    nrm[..., 3] = 1.0
    save_png(os.path.join(OUT, name + "_albedo.png"), albedo, True)
    save_png(os.path.join(OUT, name + "_nrm.png"), nrm, False)
    log("material", mat.name, "->", name, sorted(tex))
    return {"name": name, "albedo": name + "_albedo.png", "normal": name + "_nrm.png", "alphaTest": 'Alpha' in tex}


# ---------------------------------------------------------------- character

def write_character(arm):
    bones = bone_order(arm)
    index_of = {b.name: i for i, b in enumerate(bones)}
    to_model = FACE @ arm.matrix_world
    bind = [unscaled(to_model @ b.matrix_local) for b in bones]

    materials, mat_index = [], {}
    verts, tris_by_mat = [], {}
    vert_index = {}
    for obj in bpy.data.objects:
        if obj.type != 'MESH':
            continue
        me = obj.data
        me.calc_loop_triangles()
        xf = FACE @ obj.matrix_world
        nxf = xf.to_3x3().inverted().transposed()
        uv = me.uv_layers.active.data if me.uv_layers else None
        normals = me.corner_normals
        group_bone = {g.index: index_of.get(g.name, -1) for g in obj.vertex_groups}
        for tri in me.loop_triangles:
            mat = me.materials[tri.material_index]
            if mat.name not in mat_index:
                is_hair = "hair" in mat.name.lower()
                mat_index[mat.name] = len(materials)
                materials.append(bake_material(mat, "hair" if is_hair else "body", HAIR_TEX if is_hair else BODY_TEX))
            m = mat_index[mat.name]
            out = tris_by_mat.setdefault(m, [])
            for li in tri.loops:
                vi = me.loops[li].vertex_index
                t = tuple(uv[li].uv) if uv else (0.0, 0.0)
                key = (obj.name, vi, round(t[0], 5), round(t[1], 5))
                idx = vert_index.get(key)
                if idx is None:
                    v = me.vertices[vi]
                    p = xf @ v.co
                    n = (nxf @ Vector(normals[li].vector)).normalized()
                    ws = sorted(((g.weight, group_bone.get(g.group, -1)) for g in v.groups if group_bone.get(g.group, -1) >= 0),
                                reverse=True)[:MAX_INFLUENCES]
                    total = sum(w for w, _ in ws) or 1.0
                    joints = [b for _, b in ws] + [0] * (MAX_INFLUENCES - len(ws))
                    q = [int(round(w / total * 255)) for w, _ in ws] + [0] * (MAX_INFLUENCES - len(ws))
                    q[0] += 255 - sum(q)  # quantised weights must still sum to one
                    idx = len(verts)
                    verts.append((p.x, p.y, p.z, n.x, n.y, n.z, t[0], 1.0 - t[1], joints, q))
                    vert_index[key] = idx
                out.append(idx)

    indices, submeshes = [], []
    for m in sorted(tris_by_mat):
        submeshes.append((len(indices), len(tris_by_mat[m]), m))
        indices.extend(tris_by_mat[m])

    # WWC1: header, submeshes (firstIndex, indexCount, material), bones (name[32], parent, inverse bind mat4
    # column major), vertices (pos3 nrm3 uv2 float, joints u8x4, weights unorm8x4), u32 indices.
    with open(os.path.join(OUT, "character.wwc"), "wb") as f:
        f.write(b"WWC1")
        f.write(struct.pack("<4I", len(bones), len(verts), len(indices), len(submeshes)))
        for s in submeshes:
            f.write(struct.pack("<3I", *s))
        for i, b in enumerate(bones):
            parent = index_of[b.parent.name] if b.parent else -1
            inv = bind[i].inverted()
            f.write(bone_key(b.name).encode()[:31].ljust(32, b"\0"))
            f.write(struct.pack("<i", parent))
            f.write(struct.pack("<16f", *[inv[r][c] for c in range(4) for r in range(4)]))
        for v in verts:
            f.write(struct.pack("<8f4B4B", *v[:8], *v[8], *v[9]))
        f.write(np.asarray(indices, np.uint32).tobytes())
    log("character:", len(bones), "bones,", len(verts), "vertices,", len(indices) // 3, "triangles")
    return [bone_key(b.name) for b in bones], materials


# ---------------------------------------------------------------- clips

def write_clip(path, bone_names, name):
    """Clips are stored in model space so the engine can retarget them to any skeleton (other proportions, other
    rest pose): per bone the source rest pose, per frame the model space rotation of every bone and the hip
    position. The engine applies rotation deltas (pose * rest^-1) to its own skeleton."""
    arm = import_fbx(path)
    scene = bpy.context.scene
    action = arm.animation_data.action
    f0, f1 = int(action.frame_range[0]), int(action.frame_range[1])
    pose = {bone_key(pb.name): pb for pb in arm.pose.bones}
    to_model = FACE @ arm.matrix_world
    present = [bn for bn in bone_names if bn in pose]
    rest = [unscaled(to_model @ arm.data.bones[pose[bn].name].matrix_local) for bn in present]
    frames = []
    for fr in range(f0, f1 + 1):
        scene.frame_set(fr)
        rows = []
        for bn in present:
            m = unscaled(to_model @ pose[bn].matrix)
            q = m.to_quaternion().normalized()
            rows.append((q.x, q.y, q.z, q.w))
        hips = unscaled(to_model @ pose[present[0]].matrix).translation
        frames.append((rows, (hips.x, hips.y, hips.z)))
    fps = scene.render.fps / scene.render.fps_base
    # WWA2: header (bones, frames, fps); per bone name[32], parent index (-1 root), rest head xyz, rest rotation
    # quaternion xyzw (model space); per frame: hip position xyz, then per bone model rotation xyzw.
    with open(os.path.join(OUT, name + ".wwa"), "wb") as f:
        f.write(b"WWA2")
        f.write(struct.pack("<2If", len(present), len(frames), fps))
        for bn, r in zip(present, rest):
            pb = pose[bn]
            parent = present.index(bone_key(pb.parent.name)) if pb.parent and bone_key(pb.parent.name) in present else -1
            q = r.to_quaternion().normalized()
            f.write(bn.encode()[:31].ljust(32, b"\0"))
            f.write(struct.pack("<i3f4f", parent, *r.translation, q.x, q.y, q.z, q.w))
        for rows, hips in frames:
            f.write(struct.pack("<3f", *hips))
            for q in rows:
                f.write(struct.pack("<4f", *q))
    log("clip", name, len(frames), "frames @", fps, "fps, hips start/end",
        [tuple(round(c, 2) for c in frames[i][1]) for i in (0, len(frames) - 1)])
    return {"name": name, "file": name + ".wwa", "frames": len(frames), "fps": fps}


arm = import_fbx(CHAR_FBX)
bone_names, materials = write_character(arm)
clips = []
for path in sorted(glob.glob(os.path.join(ANIM_DIR, "*.fbx"))):
    if os.path.abspath(path) == os.path.abspath(CHAR_FBX):
        continue
    name = re.sub(r"[^a-z0-9]+", "_", os.path.splitext(os.path.basename(path))[0].lower()).strip("_")
    clips.append(write_clip(path, bone_names, name))

with open(os.path.join(OUT, "character.json"), "w") as f:
    json.dump({"mesh": "character.wwc", "materials": materials, "clips": clips}, f, indent=2)
log("done")
