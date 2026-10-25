# Bakes Poly Haven ground texture sets into engine-ready tiles.
#   blender -b --python bake_ground.py -- <texture_root> <out_dir> [size]
# For every <name>.blend/textures folder it writes:
#   <name>_albedo.png  RGB = diffuse (sRGB), A = displacement (for height-based blending)
#   <name>_nrm.png     RG = tangent normal (GL), B = roughness
import bpy, sys, os, glob, json
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:]
ROOT, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
SIZE = int(argv[2]) if len(argv) > 2 else 2048
os.makedirs(OUT, exist_ok=True)


def load(path):
    img = bpy.data.images.load(path, check_existing=False)
    img.colorspace_settings.name = 'Non-Color'
    img.scale(SIZE, SIZE)
    px = np.empty(SIZE * SIZE * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(SIZE, SIZE, 4)


def save(path, px, srgb):
    img = bpy.data.images.new("out", SIZE, SIZE, alpha=True)
    img.colorspace_settings.name = 'sRGB' if srgb else 'Non-Color'
    img.pixels.foreach_set(np.clip(px, 0, 1).astype(np.float32).ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)


def find(tex_dir, kind):
    hits = sorted(glob.glob(os.path.join(tex_dir, "*_%s_*" % kind)))
    return hits[0] if hits else None


names = []
for tex_dir in sorted(glob.glob(os.path.join(ROOT, "*", "textures"))):
    name = os.path.basename(os.path.dirname(tex_dir)).replace("_4k.blend", "").replace(".blend", "")
    diff = find(tex_dir, "diff")
    if not diff:
        continue
    albedo = load(diff)
    disp = find(tex_dir, "disp")
    albedo[..., 3] = load(disp)[..., 0] if disp else 0.5
    nrm = np.zeros((SIZE, SIZE, 4), np.float32)
    nrm[..., 0:2] = 0.5
    nrm[..., 2] = 0.9
    nrm[..., 3] = 1.0
    nor = find(tex_dir, "nor_gl")
    if nor:
        nrm[..., 0:2] = load(nor)[..., 0:2]
    rough = find(tex_dir, "rough")
    if rough:
        nrm[..., 2] = load(rough)[..., 0]
    save(os.path.join(OUT, name + "_albedo.png"), albedo, True)
    save(os.path.join(OUT, name + "_nrm.png"), nrm, False)
    names.append(name)
    print("[ground]", name, flush=True)

with open(os.path.join(OUT, "index.json"), "w") as f:
    json.dump({"textures": names}, f, indent=2)
