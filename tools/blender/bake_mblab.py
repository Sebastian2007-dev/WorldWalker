# Blender background script: bakes MB-Lab's realistic humans (AGPL-3, https://github.com/animate1978/MB-Lab) into
# WorldWalker's morphable human format, one directory per sex (the two sexes have different topologies):
#
#   blender -b --python tools/blender/bake_mblab.py -- <mblab_dir> <out_dir>
#
# This only converts data, nothing is fitted: the mesh, UVs and rig come from data/humanoid_library.blend, the base
# shapes, morphs, joints and skin weights from MB-Lab's JSON databases. Per sex (in <out_dir>/female, <out_dir>/male):
#   human.json   metadata as written by bake_human.py, plus MB-Lab's morph model:
#                "modifiers"   morph combinations (MB-Lab's "smart combo"), their targets by tag combination; face
#                              morphs of the ancestry characters have one target per ancestry
#                "properties"  morph properties (0..1, neutral 0.5), "transformations" age/mass/tone tables
#   base.bin, targets.bin, asset_*.bin, tex/*.png in the formats of bake_human.py
# The caucasian character (f_ca01 / m_ca01) is the base shape; the african and asian characters are targets.
# Model space: metres, z up, looking along +y. MB-Lab looks along -y, so x and y flip (a rotation).
import bpy, sys, os, json, struct
import numpy as np
from mathutils import Matrix
from mathutils.kdtree import KDTree

argv = sys.argv[sys.argv.index("--") + 1:]
MBLAB, OUT_ROOT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
# MakeHuman bake (bake_human.py) whose skins set the mean skin colours.
MATCH = os.path.abspath(argv[2]) if len(argv) > 2 else os.path.join(os.path.dirname(OUT_ROOT), "human")
DATA = os.path.join(MBLAB, "data")
FLIP = np.array([-1.0, -1.0, 1.0])
TEX = 2048
RACES = {"caucasian": "ca01", "african": "af01", "asian": "as01"}
SKINS = {"caucasian": "cauc", "african": "afro", "asian": "asian", "latino": "latino"}
# MB-Lab bone -> engine (Mixamo) name. Metacarpals and twist bones are left out: their weights go to the parent, so
# that every animated bone's first child is the one the animation retargeting expects.
MIXAMO = {"pelvis": "Hips", "spine01": "Spine", "spine02": "Spine1", "spine03": "Spine2", "neck": "Neck", "head": "Head"}
for s, side in (("L", "Left"), ("R", "Right")):
    MIXAMO.update({f"clavicle_{s}": side + "Shoulder", f"upperarm_{s}": side + "Arm", f"lowerarm_{s}": side + "ForeArm",
                   f"hand_{s}": side + "Hand", f"thigh_{s}": side + "UpLeg", f"calf_{s}": side + "Leg",
                   f"foot_{s}": side + "Foot", f"toes_{s}": side + "ToeBase", f"breast_{s}": side + "Breast"})
    for f in ("thumb", "index", "middle", "ring", "pinky"):
        for k in (1, 2, 3):
            MIXAMO[f"{f}0{k}_{s}"] = f"{side}Hand{f.capitalize()}{k}"
# Eye colours (the creator's swatch ids): the iris texture's brightness in these colours (linear-ish sRGB).
EYE_COLORS = {"blue": (0.27, 0.45, 0.72), "bluegreen": (0.24, 0.52, 0.52), "brown": (0.33, 0.19, 0.09),
              "brownlight": (0.56, 0.38, 0.19), "deepblue": (0.13, 0.24, 0.58), "green": (0.3, 0.52, 0.28),
              "grey": (0.53, 0.57, 0.6), "ice": (0.7, 0.82, 0.9), "lightblue": (0.5, 0.7, 0.9)}


# Detail sliders: the creator's groups for MB-Lab's categories, and German words for the parts of property names
# ("Nose_NostrilSizeZ" -> "Nase: Nasenloch HÃ¶he").
GROUPS = {"Abdomen": "stomach", "Armpit": "arms", "Arms": "arms", "Body": "torso", "Cheeks": "cheek", "Chest": "torso",
          "Chin": "chin", "Ears": "ears", "Elbows": "arms", "Eyebrows": "eyebrows", "Eyelids": "eyes", "Eyes": "eyes",
          "Face": "head", "Feet": "feet", "Forehead": "forehead", "Hands": "hands", "Head": "head", "Jaw": "chin",
          "Legs": "legs", "Mouth": "mouth", "Neck": "neck", "Nose": "nose", "Pelvis": "pelvis", "Shoulders": "torso",
          "Stomach": "stomach", "Torso": "torso", "Waist": "stomach", "Wrists": "arms"}
WORDS = {"Abdomen": "Bauch", "Armpit": "Achsel", "Arms": "Arme", "Body": "KÃ¶rper", "Cheeks": "Wangen", "Chest": "Brustkorb",
         "Chin": "Kinn", "Ears": "Ohren", "Elbows": "Ellbogen", "Eyebrows": "Brauen", "Eyelids": "Lider", "Eyes": "Augen",
         "Face": "Gesicht", "Feet": "FÃ¼ÃŸe", "Forehead": "Stirn", "Hands": "HÃ¤nde", "Head": "Kopf", "Jaw": "Kiefer",
         "Legs": "Beine", "Mouth": "Mund", "Neck": "Hals", "Nose": "Nase", "Pelvis": "Becken", "Shoulders": "Schultern",
         "Stomach": "Bauch", "Torso": "Rumpf", "Waist": "Taille", "Wrists": "Handgelenke",
         "Size": "GrÃ¶ÃŸe", "Pos": "Position", "Loc": "Position", "Rot": "Drehung", "Scale": "GrÃ¶ÃŸe", "Mass": "Masse",
         "Tone": "Straffheit", "Length": "LÃ¤nge", "Girth": "Umfang", "Angle": "Winkel", "Angle2": "Winkel 2",
         "Curve": "KrÃ¼mmung", "Volume": "Volumen", "Prominence": "AusprÃ¤gung", "Crease": "Falte", "Diam": "Durchmesser",
         "Breast": "Brust", "Aureola": "Warzenhof", "Nipple": "Brustwarze", "Belly": "Bauch", "Gluteus": "GesÃ¤ÃŸ",
         "Upperarm": "Oberarm", "Forearm": "Unterarm", "Upperleg": "Oberschenkel", "Upperlegs": "Oberschenkel",
         "Lowerleg": "Unterschenkel", "Lowerlegs": "Unterschenkel", "Thigh": "Oberschenkel", "Calf": "Wade",
         "Knee": "Knie", "Ankle": "KnÃ¶chel", "Heel": "Ferse", "Width": "Breite", "Fingers": "Finger", "Tip": "Spitze",
         "Inter": "Zwischen", "Dist": "Abstand", "Nails": "NÃ¤gel", "Palm": "HandflÃ¤che", "Lobe": "OhrlÃ¤ppchen",
         "Round": "Rundung", "Cleft": "GrÃ¼bchen", "Nostril": "Nasenloch", "Septum": "Septum", "Flat": "flach",
         "Rolled": "gerollt", "Wing": "NasenflÃ¼gel", "Back": "hinten", "Bump": "HÃ¶cker", "Ball": "Nasenkuppe",
         "Base": "Basis", "Bridge": "RÃ¼cken", "Glabella": "Nasenwurzel", "Shape": "Form", "Lowerlip": "Unterlippe",
         "Upperlip": "Oberlippe", "Ext": "Kontur", "Philtrum": "Philtrum", "Protusion": "Vorstehen",
         "Corners": "Mundwinkel", "Side": "seitlich", "Zygom": "Wangenknochen", "Infra": "unter", "Bag": "TrÃ¤nensack",
         "Inner": "innen", "Outer": "auÃŸen", "Middle": "Mitte", "Lower": "unten", "Upper": "oben", "Type": "Typ",
         "Almond": "Mandel", "Hooded": "Schlupflid", "Sinus": "Winkel", "Crosscalibration": "Ausrichtung",
         "Iris": "Iris", "Droop": "HÃ¤ngen", "Ridge": "Brauenbogen", "Temple": "SchlÃ¤fen", "Cranium": "SchÃ¤del",
         "Dolichocephalic": "lang", "Pentagonoides": "fÃ¼nfeckig", "Platycephalus": "flach", "Nucha": "Nacken",
         "Ellipsoid": "oval", "Parallelepiped": "eckig", "Triangle": "dreieckig", "Collarbone": "SchlÃ¼sselbein",
         "Trapezious": "Trapezmuskel", "Toracic": "Brustkorb", "Vshape": "V-Form", "Dorsi": "RÃ¼ckenmuskel",
         "Local": "lokal", "Fat": "Fett", "Crotch": "Schritt", "Bow": "O-Bein", "In": "innen", "Lateral": "seitlich",
         "Two": "2", "X": "Breite", "Y": "Tiefe", "Z": "HÃ¶he", "X2": "Breite 2"}


def slider(prop):
    """Creator detail slider for an MB-Lab property (-1..1 maps to 0..1, neutral 0.5)."""
    import re
    category, part = prop.split("_", 1)
    tokens = re.findall(r"[A-Z][a-z]+\d?|[A-Z]\d?|[a-z]+\d?|\d+", part)
    words = [WORDS.get(t, WORDS.get(t.capitalize(), t)) for t in tokens]
    size = any(t in ("Size", "Scale", "Length", "Girth", "Volume", "Diam", "Mass", "X", "Y", "Z") for t in tokens)
    return {"group": GROUPS.get(category, category.lower()), "name": prop, "prop": prop,
            "label": WORDS.get(category, category) + ": " + " ".join(words),
            "lo": "kleiner" if size else "weniger", "hi": "grÃ¶ÃŸer" if size else "mehr"}


def log(*a):
    print("[mblab]", *a, flush=True)


def load(*parts):
    with open(os.path.join(DATA, *parts)) as f:
        return json.load(f)


def model(v):
    return np.asarray(v, np.float64).reshape(-1, 3) * FLIP


bpy.ops.wm.open_mainfile(filepath=os.path.join(DATA, "humanoid_library.blend"))
config = load("characters_config.json")


def bake(sex):
    out = os.path.join(OUT_ROOT, sex)
    os.makedirs(os.path.join(out, "tex"), exist_ok=True)
    s = sex[0]
    chars = {r: f"{s}_{c}" for r, c in RACES.items()}
    cfg = config[chars["caucasian"]]
    obj = bpy.data.objects[cfg["template_model"]]
    me = obj.data
    N = len(me.vertices)
    mats = [slot.material.name for slot in obj.material_slots]

    # ------------------------------------------------------------ shapes and morphs
    shapes = {r: model(load("vertices", c + "_verts.json")) for r, c in chars.items()}
    assert all(len(v) == N for v in shapes.values()), "vertex database does not match the mesh"
    co = shapes["caucasian"]
    targets = []

    def add_target(idx, d):
        d = np.asarray(d, np.float32).reshape(-1, 3)
        keep = np.abs(d).max(axis=1) > 1e-7
        targets.append((np.asarray(idx, np.uint32)[keep], d[keep]))
        return len(targets) - 1

    macro_targets = {}
    for r in ("african", "asian"):
        delta = shapes[r] - co
        idx = np.nonzero(np.abs(delta).max(axis=1) > 1e-7)[0]
        macro_targets["race/" + r] = add_target(idx, delta[idx])

    def morph_target(entries):
        e = np.asarray(entries, np.float64)
        return add_target(e[:, 0].astype(np.uint32), model(e[:, 1:4]))

    # Modifier "<Category>_<A>-<B>" with morphs "<Category>_<A>-<B>_<tagA>-<tagB>"; its properties are
    # "<Category>_<A>", "<Category>_<B>". Character morphs (the face) load after the shared ones and win, as in MB-Lab.
    modifiers = {}

    def add_morph(name, target, race=None):
        parts = name.split("_")
        if len(parts) != 3:
            return
        mod = parts[0] + "_" + parts[1]
        m = modifiers.setdefault(mod, {"name": mod, "props": [parts[0] + "_" + c for c in parts[1].split("-")],
                                       "targets": {}})
        if race is None:
            m["targets"][parts[2]] = target
        else:
            t = m["targets"].get(parts[2])
            if not isinstance(t, dict):
                t = m["targets"][parts[2]] = {}
            t[race] = target

    for name, entries in load("morphs", cfg["shared_morphs_file"]).items():
        if entries:
            add_morph(name, morph_target(entries))
    for r, c in chars.items():
        for name, entries in load("morphs", c + "_morphs.json").items():
            if entries:
                add_morph(name, morph_target(entries), r)
    modifiers = sorted(modifiers.values(), key=lambda m: m["name"])
    properties = sorted({p for m in modifiers for p in m["props"]})
    sliders = [slider(p) for p in properties]
    log(sex, len(modifiers), "modifiers,", len(properties), "properties,", len(targets), "targets")

    # ------------------------------------------------------------ skeleton and skin weights
    arm = bpy.data.objects["MBLab_skeleton_base_fk"]
    joints = load("joints", cfg["joints_base_file"])
    offsets = load("joints", cfg["joints_offset_file"])
    flip = np.diag([-1.0, -1.0, 1.0])

    def kept_parent(b):
        b = b.parent
        while b is not None and b.name not in MIXAMO:
            b = b.parent
        return b

    order = []

    def visit(b):
        if b.name in MIXAMO:
            order.append(b)
        for c in b.children:
            visit(c)

    for b in arm.data.bones:
        if b.parent is None:
            visit(b)
    index = {b.name: i for i, b in enumerate(order)}
    bones = []
    for b in order:
        m = arm.matrix_world @ b.matrix_local
        rot = np.asarray(m.to_3x3())
        q = __import__("mathutils").Matrix((flip @ rot).tolist()).to_quaternion().normalized()
        head, tail = joints[b.name + "_head"], joints[b.name + "_tail"]
        assert head and tail, b.name + ": no joint vertices"
        p = kept_parent(b)
        bone = {"name": MIXAMO[b.name], "parent": index[p.name] if p else -1, "head": head, "tail": tail,
                "rot": [q.x, q.y, q.z, q.w],
                "neutralHead": list(model(arm.matrix_world @ b.head_local)[0]),
                "neutralTail": list(model(arm.matrix_world @ b.tail_local)[0])}
        for key, joint in (("headOffset", b.name + "_head"), ("tailOffset", b.name + "_tail")):
            if joint in offsets:
                bone[key] = list(model(offsets[joint])[0])
        bones.append(bone)
    side = np.mean(co[joints["thigh_L_head"]], axis=0)[0]
    log(sex, len(bones), "bones; left thigh at x = %.3f (model space, should be negative)" % side)

    def owner(name):
        b = arm.data.bones.get(name)
        while b is not None and b.name not in MIXAMO:
            b = b.parent
        return index[b.name] if b else -1

    acc = [dict() for _ in range(N)]
    for group, entries in load("vgroups", cfg["vertexgroup_base_file"]).items():
        bi = owner(group)
        if bi < 0:
            continue
        for v, w in entries:
            acc[int(v)][bi] = acc[int(v)].get(bi, 0.0) + w
    weighted = np.array([bool(a) for a in acc])
    tree = KDTree(int(weighted.sum()))
    for i in np.nonzero(weighted)[0]:
        tree.insert(co[i], int(i))
    tree.balance()
    missing = 0
    for i in np.nonzero(~weighted)[0]:
        acc[i] = dict(acc[tree.find(co[i])[1]])
        missing += 1
    weights = np.zeros((N, 4), np.float32)
    jidx = np.zeros((N, 4), np.uint8)
    for i, a in enumerate(acc):
        top = sorted(((w, b) for b, w in a.items()), reverse=True)[:4]
        total = sum(w for w, _ in top)
        for k, (w, b) in enumerate(top):
            weights[i, k] = w / total
            jidx[i, k] = b
    log(sex, missing, "vertices without weights took their nearest neighbour's")

    # ------------------------------------------------------------ render geometry
    uv_main = me.uv_layers["UVMap"].data
    uv_iris = me.uv_layers["Eyes_iris"].data
    body_mats = {"MBLab_skin3", "MBLab_nails", "MBLab_tongue", "MBlab_human_teeth", "MBlab_generic"}
    eye_mats = {"MBlab_human_eyes", "MBlab_pupil", "MBLab_Iris_V4"}
    cent = {}
    for p in me.polygons:
        cent.setdefault(mats[p.material_index], []).append(co[p.vertices[0]])
    for k, v in cent.items():
        log(sex, "material %-20s %5d faces, centre %s" % (k, len(v), np.round(np.mean(v, axis=0), 3)))

    def mesh(face_ok, uv_of):
        """Triangles of the faces passing face_ok (quads fanned as (a,b,c),(a,c,d)), split where UVs differ."""
        key_to_rv, rverts, tris = {}, [], []
        for poly in me.polygons:
            if not face_ok(poly):
                continue
            corners = []
            for li in poly.loop_indices:
                vi = me.loops[li].vertex_index
                u, v = uv_of(poly, li)
                key = (vi, round(u, 6), round(v, 6))
                rv = key_to_rv.get(key)
                if rv is None:
                    rv = key_to_rv[key] = len(rverts)
                    rverts.append((vi, u, v))
                corners.append(rv)
            for k in range(1, len(corners) - 1):
                tris.append((corners[0], corners[k], corners[k + 1]))
        return rverts, tris

    def main_uv(poly, li):
        uv = uv_main[li].uv
        return uv[0], 1.0 - uv[1]

    body_rverts, body_tris = mesh(lambda p: mats[p.material_index] in body_mats, main_uv)
    body = np.zeros(N, bool)
    for vi, _, _ in body_rverts:
        body[vi] = True
    ground = float(co[body][:, 2].min())
    with open(os.path.join(out, "base.bin"), "wb") as f:
        f.write(struct.pack("<I", N))
        f.write(co.astype(np.float32).tobytes())
        f.write(struct.pack("<I", len(body_rverts)))
        for vi, u, v in body_rverts:
            f.write(struct.pack("<Iff", vi, u, v))
        f.write(struct.pack("<I", len(body_tris)))
        f.write(np.asarray(body_tris, np.uint32).tobytes())
        f.write(jidx.tobytes())
        q = np.round(weights * 255).astype(np.int32)
        q[:, 0] += 255 - q.sum(axis=1)
        f.write(np.clip(q, 0, 255).astype(np.uint8).tobytes())
    with open(os.path.join(out, "targets.bin"), "wb") as f:
        f.write(struct.pack("<I", len(targets)))
        for idx, d in targets:
            f.write(struct.pack("<I", len(idx)))
            f.write(idx.astype(np.uint32).tobytes())
            f.write(d.astype(np.float32).tobytes())
    log(sex, "body:", len(body_rverts), "render vertices,", len(body_tris), "triangles")

    # ------------------------------------------------------------ textures
    # Blender's image API (its Python has no Pillow). Arrays are float RGBA, rows from the bottom (v up).
    def pixels(img):
        w, h = img.size
        px = np.empty(w * h * 4, np.float32)
        img.pixels.foreach_get(px)
        return px.reshape(h, w, 4)

    def image(name, size=None):
        img = bpy.data.images.load(os.path.join(DATA, "textures", name), check_existing=False)
        img.colorspace_settings.name = "Non-Color"
        if size:
            img.scale(*size)
        px = pixels(img)
        bpy.data.images.remove(img)
        return px

    def resize(px, w, h):
        img = bpy.data.images.new("resize", px.shape[1], px.shape[0], alpha=True, float_buffer=True)
        img.pixels.foreach_set(px.ravel())
        img.scale(w, h)
        out_px = pixels(img)
        bpy.data.images.remove(img)
        return out_px

    def save(px, name):
        if px.shape[2] == 3:
            px = np.concatenate([px, np.ones(px.shape[:2] + (1,), np.float32)], -1)
        img = bpy.data.images.new(name, px.shape[1], px.shape[0], alpha=True)
        img.colorspace_settings.name = "Non-Color"
        img.pixels.foreach_set(np.clip(px, 0, 1).astype(np.float32).ravel())
        img.filepath_raw = os.path.join(out, "tex", name + ".png")
        img.file_format = "PNG"
        img.save()
        bpy.data.images.remove(img)
        return "tex/" + name + ".png"

    # Skin detail: the bump map as a tangent space normal map (OpenGL convention), halved to the albedo's size.
    h = image(cfg["texture_bump"])[..., 0]
    strength = 20.0
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * strength
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * strength
    n = np.stack([-dx, -dy, np.ones_like(h)], -1)
    while n.shape[0] > TEX:
        n = 0.25 * (n[0::2, 0::2] + n[1::2, 0::2] + n[0::2, 1::2] + n[1::2, 1::2])
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    normal = save(n * 0.5 + 0.5, f"skin_{sex}_nrm")
    # MB-Lab's skins are far redder than MakeHuman's (red is clipped on most of the body): each channel is scaled
    # (linearly) to the mean of the matching MakeHuman skin, so the creator's tone and warmth sliders behave alike.
    def linear(c):
        return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

    def srgb(c):
        c = np.clip(c, 0, 1)
        return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)

    skins = []
    for race, tag in SKINS.items():
        px = linear(image(f"hum_{s}_{tag}_albedo.png")[..., :3])
        reference = os.path.join(MATCH, "tex", f"skin_young_{'asian' if race == 'latino' else race}_{sex}.png")
        if os.path.isfile(reference):
            img = bpy.data.images.load(reference, check_existing=False)
            img.colorspace_settings.name = "Non-Color"
            ref = linear(pixels(img)[..., :3])
            bpy.data.images.remove(img)
            skin = px.sum(-1) < 2.9  # skin, not the white of eyes and teeth
            gain = ref.reshape(-1, 3).mean(0) / px[skin].mean(0)
            log(sex, race, "skin gain", np.round(gain, 3))
            px = px * gain
        skins.append({"name": f"young_{race}_{sex}", "albedo": save(srgb(px), f"skin_{race}_{sex}"), "normal": normal})

    assets = []

    def write_asset(name, kind, rverts, tris, **extra):
        with open(os.path.join(out, "asset_" + name + ".bin"), "wb") as f:
            f.write(struct.pack("<I", len(rverts)))
            for vi, u, v in rverts:
                f.write(struct.pack("<3I3f3f2f", vi, vi, vi, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, u, v))
            f.write(struct.pack("<I", len(tris)))
            f.write(np.asarray(tris, np.uint32).tobytes())
            f.write(struct.pack("<B", 0))
            for _ in range(3):
                f.write(struct.pack("<IIf", 0, 0, 1.0))
            f.write(struct.pack("<I", 0))
        assets.append({"kind": kind, "name": name, "file": "asset_" + name + ".bin", "normal": None,
                       "license": "AGPL-3.0", "author": "Manuel Bastioni (MB-Lab)", **extra})
        log(sex, "asset", name, len(rverts), "vertices", len(tris), "triangles")

    # Eyes: sclera and pupil from the skin atlas (left half of the eye texture), the iris from MB-Lab's detailed iris
    # texture (right half), recoloured per eye colour.
    albedo = image(cfg["texture_albedo"])
    W = albedo.shape[1]
    uvs = np.array([uv_main[li].uv[:] for p in me.polygons if mats[p.material_index] in eye_mats - {"MBLab_Iris_V4"}
                    for li in p.loop_indices])
    c0 = np.floor((uvs.min(0) - 0.002) * W).astype(int)
    c1 = np.ceil((uvs.max(0) + 0.002) * W).astype(int)
    crop = albedo[c0[1]:c1[1], c0[0]:c1[0]]
    lo, hi = c0 / W, c1 / W

    def eye_uv(poly, li):
        if mats[poly.material_index] == "MBLab_Iris_V4":
            uv = np.clip(uv_iris[li].uv[:], 0.0, 1.0)
            return 0.5 + 0.5 * uv[0], 1.0 - uv[1]
        uv = (np.array(uv_main[li].uv[:]) - lo) / (hi - lo)
        return 0.5 * uv[0], 1.0 - uv[1]

    rverts, tris = mesh(lambda p: mats[p.material_index] in eye_mats, eye_uv)
    write_asset("eyes_mblab", "eyes", rverts, tris, albedo=None, alphaTest=True, label="MB-Lab")
    iris = image(cfg["texture_iris_color"], (1024, 1024))[..., :3]
    lum = iris @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    lum = lum / max(np.percentile(lum, 99), 1e-3)
    left = resize(crop, 1024, 1024)[..., :3]
    eye_materials = []
    for name, rgb in EYE_COLORS.items():
        tinted = np.clip(lum[..., None] ** 0.8 * np.array(rgb, np.float32) * 1.6, 0, 1)
        eye_materials.append({"name": name, "albedo": save(np.concatenate([left, tinted], axis=1), "eye_" + name)})
    assets[-1]["albedo"] = eye_materials[0]["albedo"]

    # Eyelashes: dark strands on white in MB-Lab's eyelash texture; the darkness is the coverage.
    lash = image(cfg["texture_eyelash_albedo"], (TEX, TEX))[..., :3]
    cover = np.clip((1.0 - lash @ np.array([0.2126, 0.7152, 0.0722], np.float32) - 0.35) * 2.5, 0, 1)
    rgba = np.concatenate([np.full(lash.shape, 0.06, np.float32), cover[..., None]], -1)
    rverts, tris = mesh(lambda p: mats[p.material_index] == "MBlab_eyelash", main_uv)
    write_asset("eyelashes_mblab", "eyelashes", rverts, tris, albedo=save(rgba, "eyelashes_" + sex), alphaTest=True,
                label="MB-Lab")

    with open(os.path.join(out, "human.json"), "w") as f:
        json.dump({"vertices": N, "ground": ground, "sex": sex, "bones": bones, "sliders": sliders, "macro": {},
                   "macroTargets": macro_targets, "modifiers": modifiers, "properties": properties,
                   "transformations": load("transformations", cfg["transformations_file"]),
                   "assets": assets, "skins": skins, "eyeMaterials": eye_materials,
                   "license": "AGPL-3.0 (MB-Lab data)"}, f)
    log(sex, "done")


for sex in ("female", "male"):
    bake(sex)
