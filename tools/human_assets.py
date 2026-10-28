# MakeHuman proxy assets (eyes, eyebrows, eyelashes, hair, clothes) for WorldWalker's morphable human. Shared by
# tools/blender/bake_human.py and usable on its own, without Blender, to add assets to an existing bake:
#
#   python tools/human_assets.py assets/human assets/baked/human
#
# The standalone mode bakes every asset of the pack that human.json does not list yet, copies its textures (Pillow)
# and appends it to human.json. Existing assets, skins and morph data stay untouched. Author, licence and source of
# every asset come from the asset packs' metadata (<asset_dir>/packs/*.json); CREDITS.md in the output lists every
# asset that requires attribution.
import io, json, os, re, struct, sys, glob
import numpy as np

FACE = np.array([-1.0, -1.0, 1.0])  # MPFB humans look along -y; rotate 180 degrees about z
MAX_TEX = 2048
KINDS = ("eyes", "eyebrows", "eyelashes", "hair", "clothes")
# The creator's clothing slots by the asset packs' categories. Outfits cover top and bottom; categories without a
# slot (nails, horns, unsorted) are not offered.
SLOTS = {"Shirt/T-shirt/Sweater": "top", "Pants": "bottom", "Skirt": "bottom", "Gown/Robe": "outfit",
         "Uniform/Armor": "outfit", "Underwear": "underwear", "Shoes/Boots/Socks": "shoes", "Hat/Helmet": "hat",
         "Gloves": "gloves", "Jewelry": "jewelry", "Accessory": "accessory", "Facialhair": "beard"}


def slot_of(asset, category):
    """Clothing slot: by the pack category, else by the name of MakeHuman's system assets."""
    if asset["kind"] != "clothes":
        return None
    if category:
        return SLOTS.get(category)
    name = asset["name"]
    return "shoes" if name.startswith("clothes_shoes") else "hat" if name.startswith("clothes_fedora") else "outfit"


# Creator labels of community assets: the folder name without its author, in English, and in German with the common
# clothing words translated (the rest stays as it is).
GERMAN = {"t": "T", "shirt": "Shirt", "tshirt": "T-Shirt", "sweater": "Pullover", "sweatshirt": "Sweatshirt",
          "hoodie": "Kapuzenpulli", "jacket": "Jacke", "coat": "Mantel", "dress": "Kleid", "gown": "Abendkleid",
          "skirt": "Rock", "pants": "Hose", "trousers": "Hose", "trouser": "Hose", "jeans": "Jeans", "shorts": "Shorts",
          "boots": "Stiefel", "boot": "Stiefel", "shoes": "Schuhe", "shoe": "Schuh", "sneakers": "Sneaker",
          "sandals": "Sandalen", "flats": "Ballerinas", "socks": "Socken", "sock": "Socken", "stockings": "Strümpfe",
          "stocking": "Strümpfe", "pantyhose": "Strumpfhose", "bra": "BH", "panty": "Slip", "panties": "Slip",
          "briefs": "Slip", "thong": "String", "string": "String", "bikini": "Bikini", "swimsuit": "Badeanzug",
          "swimming": "Bade", "trunks": "Badehose", "swimwear": "Bademode", "suit": "Anzug", "glasses": "Brille",
          "sunglasses": "Sonnenbrille", "sunglass": "Sonnenbrille", "goggles": "Schutzbrille", "gloves": "Handschuhe",
          "ring": "Ring", "necklace": "Halskette", "bracelet": "Armband", "anklet": "Fußkettchen",
          "earrings": "Ohrringe", "earring": "Ohrring", "beard": "Bart", "moustache": "Schnurrbart", "hood": "Kapuze",
          "long": "lang", "short": "kurz", "black": "schwarz", "white": "weiß", "red": "rot", "blue": "blau",
          "female": "Damen", "male": "Herren", "ladies": "Damen", "mini": "Mini", "knee": "knielang", "length": "",
          "high": "hoch", "low": "niedrig", "leather": "Leder", "lace": "Spitze", "wool": "Woll", "knitted": "Strick",
          "winter": "Winter", "with": "mit", "and": "und", "top": "Top", "tank": "Tank", "uniform": "Uniform",
          "pearl": "Perlen", "diamond": "Diamant", "wedding": "Ehe", "chain": "Ketten", "riding": "Reit",
          "running": "Lauf", "tennis": "Tennis", "cycling": "Rad", "ankle": "Knöchel", "heel": "Absatz",
          "heeled": "mit Absatz", "plain": "schlicht", "striped": "gestreift", "tight": "eng", "classic": "klassisch",
          "medieval": "mittelalterlich", "evening": "Abend", "cocktail": "Cocktail", "pleated": "Falten",
          "plaid": "kariert", "pencil": "Bleistift", "tiered": "gestuft", "full": "weit", "polo": "Polo",
          "cardigan": "Strickjacke", "kimono": "Kimono", "robe": "Robe", "apron": "Schürze", "coveralls": "Overall",
          "wetsuit": "Neoprenanzug", "jumpsuit": "Overall", "romper": "Einteiler", "camisole": "Hemdchen",
          "stiletto": "Stiletto", "booties": "Stiefeletten", "platform": "Plateau", "warmer": "Stulpen",
          "optical": "Korrektur", "round": "runde", "frames": "Gestell", "medical": "medizinische", "mask": "Maske"}


def labels(key):
    """(German, English) label for the asset folder name "<author>_<words>"."""
    words = [w for w in re.split(r"[^a-z0-9]+", key.lower()) if w]
    if len(words) > 1:
        words = words[1:]
    words = [w for w in words if not re.fullmatch(r"\d+|[a-z]?\d+[a-z]?", w)] or words
    english = " ".join(words)
    german = " ".join(g for g in (GERMAN.get(w, w) for w in words) if g)
    return german[:1].upper() + german[1:], english[:1].upper() + english[1:]


# MakeHuman's asset FAQ links these licence texts.
LICENSES = {"CC0": "https://creativecommons.org/publicdomain/zero/1.0/deed.en",
            "CC-BY": "https://creativecommons.org/licenses/by/2.5/se/deed.en"}


def log(*a):
    print("[human]", *a, flush=True)


def parse_mhmat(path, asset_dir):
    mat = {}
    folder = os.path.dirname(path)
    for line in open(path, encoding="utf-8", errors="replace"):
        w = line.split()
        if len(w) >= 2 and w[0] in ("diffuseTexture", "normalmapTexture", "transparencyMapTexture"):
            p = os.path.join(folder, w[1])
            if not os.path.isfile(p):
                p = os.path.join(asset_dir, w[1])
            mat[w[0]] = p
        elif len(w) == 4 and w[0] == "diffuseColor":
            mat["diffuseColor"] = [float(x) for x in w[1:]]
    return mat


def parse_obj(path):
    vt, faces = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("vt "):
            vt.append(tuple(float(x) for x in line.split()[1:3]))
        elif line.startswith("f "):
            corners = []
            for c in line.split()[1:]:
                p = c.split("/")
                corners.append((int(p[0]) - 1, int(p[1]) - 1 if len(p) > 1 and p[1] else -1))
            faces.append(corners)
    return vt, faces


def parse_mhclo(path):
    folder = os.path.dirname(path)
    info = {"verts": [], "delete": [], "scale": {}, "material": None, "obj": None, "name": None, "z_depth": 50}
    status = ""
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        w = line.split()
        if status == "v":
            if w[0].lstrip("-").isdigit():
                if len(w) == 1:
                    info["verts"].append(((int(w[0]),) * 3, (1.0, 0.0, 0.0), (0.0, 0.0, 0.0)))
                else:
                    info["verts"].append(((int(w[0]), int(w[1]), int(w[2])), tuple(float(x) for x in w[3:6]),
                                          tuple(float(x) for x in w[6:9])))
                continue
            # Keyword lines may appear between "verts 0" and the data (bob01, long01, ...); the block continues.
        if status == "d":
            if w[0].isdigit():
                k = 0
                while k < len(w):
                    if k + 2 < len(w) and w[k + 1] == "-":
                        info["delete"].extend(range(int(w[k]), int(w[k + 2]) + 1))
                        k += 3
                    else:
                        info["delete"].append(int(w[k]))
                        k += 1
                continue
            status = ""
        key = w[0]
        if key == "verts":
            status = "v"
        elif key == "delete_verts":
            status = "d"
        elif key in ("x_scale", "y_scale", "z_scale"):
            info["scale"][key] = (int(w[1]), int(w[2]), float(w[3]))
        elif key == "material":
            info["material"] = os.path.join(folder, w[1])
        elif key == "obj_file":
            info["obj"] = os.path.join(folder, w[1])
        elif key == "name":
            info["name"] = w[1]
        elif key == "z_depth":
            info["z_depth"] = int(w[1])
    return info


def asset_name(kind, mhclo_path):
    info_name = None
    for raw in open(mhclo_path, encoding="utf-8", errors="replace"):
        w = raw.split()
        if len(w) >= 2 and w[0] == "name":
            info_name = w[1]
            break
    return kind + "_" + re.sub(r"[^a-z0-9]+", "_", (info_name or os.path.basename(mhclo_path)).lower()).strip("_")


def write_asset(kind, mhclo_path, out, asset_dir, copy_texture):
    """Fitted asset: every render vertex follows three base vertices plus an offset scaled by the body's size
    (MakeHuman's proxy fitting, evaluated by the engine for every body shape). copy_texture(path, name) copies a
    texture into <out>/tex and returns its path relative to out (or None)."""
    info = parse_mhclo(mhclo_path)
    if not info["obj"] or not os.path.isfile(info["obj"]) or not info["verts"]:
        return None
    vt, faces = parse_obj(info["obj"])
    name = kind + "_" + re.sub(r"[^a-z0-9]+", "_", (info["name"] or os.path.basename(mhclo_path)).lower()).strip("_")
    # Offsets: MPFB's mhclo reader maps (d0, d1, d2) -> Blender (d0, -d2, d1); each Blender axis is scaled by the
    # body's extent between two reference vertices: x by x_scale (along x), y by z_scale (along y), z by y_scale
    # (along z). Model space flips x and y.
    scale_pairs = [info["scale"].get("x_scale"), info["scale"].get("z_scale"), info["scale"].get("y_scale")]
    if any(p is None for p in scale_pairs):
        scale_pairs = None
    key_to_rv, rverts, tris = {}, [], []
    for face in faces:
        corners = []
        for vi, ti in face:
            uv = vt[ti] if 0 <= ti < len(vt) else (0.0, 0.0)
            key = (vi, ti)
            rv = key_to_rv.get(key)
            if rv is None:
                refs, ws, d = info["verts"][vi]
                off = np.array([d[0], -d[2], d[1]]) * FACE
                rv = key_to_rv[key] = len(rverts)
                rverts.append((refs, ws, off, uv[0], 1.0 - uv[1]))
            corners.append(rv)
        for k in range(1, len(corners) - 1):
            tris.append((corners[0], corners[k], corners[k + 1]))
    mat = parse_mhmat(info["material"], asset_dir) if info["material"] and os.path.isfile(info["material"]) else {}
    albedo = copy_texture(mat.get("diffuseTexture"), name + "_albedo")
    # The engine expects tangent space normal maps; object space ones ("*_objnorm") would light the hair wrongly.
    nrm = mat.get("normalmapTexture")
    normal = None if nrm and "objnorm" in os.path.basename(nrm).lower() else copy_texture(nrm, name + "_nrm")
    alpha_tex = mat.get("transparencyMapTexture")
    # asset_<name>.bin: u32 R; R x (u32x3 refs, float3 weights, float3 offset, float u, float v); u32 T, T x u32x3;
    #                   u8 has_scale + 3 x (u32 a, u32 b, float reference); u32 D, D x u32 deleted base vertices
    with open(os.path.join(out, "asset_" + name + ".bin"), "wb") as f:
        f.write(struct.pack("<I", len(rverts)))
        for refs, ws, off, u, v in rverts:
            f.write(struct.pack("<3I3f3f2f", *refs, *ws, *off, u, v))
        f.write(struct.pack("<I", len(tris)))
        f.write(np.asarray(tris, np.uint32).tobytes())
        f.write(struct.pack("<B", 1 if scale_pairs else 0))
        for p in (scale_pairs or [(0, 0, 1.0)] * 3):
            f.write(struct.pack("<IIf", p[0], p[1], p[2]))
        f.write(struct.pack("<I", len(info["delete"])))
        f.write(np.asarray(info["delete"], np.uint32).tobytes())
    log("asset", kind, name, len(rverts), "vertices", len(tris), "triangles", "delete", len(info["delete"]))
    return {"kind": kind, "name": name, "file": "asset_" + name + ".bin", "albedo": albedo, "normal": normal,
            "alphaTest": kind in ("hair", "eyebrows", "eyelashes", "eyes") or bool(alpha_tex), "zDepth": info["z_depth"],
            "color": mat.get("diffuseColor")}


def png_essentials(data):
    """The PNG with only its critical chunks (header, palette, transparency, pixels)."""
    chunks, o = [data[:8]], 8
    while o + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[o:o + 8])
        if kind in (b"IHDR", b"PLTE", b"tRNS", b"IDAT", b"IEND"):
            chunks.append(data[o:o + 12 + n])
        if kind == b"IEND":
            break
        o += 12 + n
    return b"".join(chunks)


def pil_copy_texture(out, max_size=MAX_TEX, jpeg=False):
    """Texture copier for write_asset. jpeg: textures without transparency are stored as JPEG (a fraction of the
    size of a PNG; the community packs' textures are photographs)."""
    from PIL import Image

    def copy(path, name):
        if not path or not os.path.isfile(path):
            return None
        try:
            img = Image.open(path)
        except OSError:
            # Some community PNGs carry malformed ancillary chunks (zTXt before iCCP); the pixels themselves are fine.
            img = Image.open(io.BytesIO(png_essentials(open(path, "rb").read())))
        img = img.convert("RGBA" if img.mode in ("RGBA", "LA", "P") else "RGB")
        if max(img.size) > max_size:
            s = max_size / max(img.size)
            img = img.resize((max(1, int(img.width * s)), max(1, int(img.height * s))), Image.LANCZOS)
        # Normal maps never use their alpha channel.
        if jpeg and (img.mode == "RGB" or name.endswith("_nrm") or img.getchannel("A").getextrema()[0] == 255):
            rel = "tex/" + name + ".jpg"
            img.convert("RGB").save(os.path.join(out, rel), quality=90)
            return rel
        rel = "tex/" + name + ".png"
        img.save(os.path.join(out, rel))
        return rel
    return copy


def pack_metadata(asset_dir):
    """Asset folder name -> pack entry (author, license, source, ...)."""
    packs = {}
    for path in sorted(glob.glob(os.path.join(asset_dir, "packs", "*.json"))):
        packs.update(json.load(open(path, encoding="utf-8")))
    return packs


def write_credits(out, assets):
    rows = sorted((a for a in assets if a.get("license", "CC0") != "CC0"), key=lambda a: (a["author"], a["name"]))
    with open(os.path.join(out, "CREDITS.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Character asset credits\n\nGenerated by `tools/human_assets.py` from the asset packs' metadata. "
                "These assets are used under licences that require attribution:\n\n"
                "| Asset | Author | Licence | Source |\n|---|---|---|---|\n")
        for a in rows:
            lic = "[%s](%s)" % (a["license"], LICENSES[a["license"]]) if a["license"] in LICENSES else a["license"]
            f.write("| %s | %s | %s | %s |\n" % (a["folder"], a["author"], lic, a.get("source", "")))
        f.write("\nAll other character assets are CC0; see `SOURCES.md`.\n")
    log(len(rows), "assets need attribution")


def main(asset_dir, out, max_size=MAX_TEX, jpeg=False):
    meta_path = os.path.join(out, "human.json")
    meta = json.load(open(meta_path, encoding="utf-8"))
    known = {a["name"] for a in meta["assets"]}
    copy = pil_copy_texture(out, max_size, jpeg)
    packs = pack_metadata(asset_dir)
    added, folders = [], {}
    for kind in KINDS:
        for mhclo in sorted(glob.glob(os.path.join(asset_dir, kind, "*", "*.mhclo"))):
            folders[asset_name(kind, mhclo)] = os.path.basename(os.path.dirname(mhclo))
            if asset_name(kind, mhclo) in known:
                continue
            a = write_asset(kind, mhclo, out, asset_dir, copy)
            if a:
                added.append(a)
                known.add(a["name"])
    for a in meta["assets"] + added:
        folder = folders.get(a["name"])
        if folder is None and "folder" in a:
            continue  # from another asset directory: its metadata stays
        pack = packs.get(folder, {})
        a["folder"] = folder or a["name"]
        a["license"] = pack.get("license", "CC0")
        author = pack.get("author", "")
        if pack.get("original_author"):
            author += " (original: %s)" % pack["original_author"]
        a["author"] = author
        if pack.get("source"):
            a["source"] = pack["source"]
        if pack.get("category"):
            a["category"] = pack["category"]
        if folder in packs and pack.get("category") and "label" not in a:
            a["label"], a["labelEn"] = labels(folder)
        slot = pack.get("slot") or slot_of(a, pack.get("category"))
        if slot:
            a["slot"] = slot
    write_credits(out, meta["assets"] + added)
    # Keep the kinds grouped in their bake order so the creator lists stay sorted; other kinds (genitals, see
    # blender/bake_genitals.py) go last.
    order = lambda kind: KINDS.index(kind) if kind in KINDS else len(KINDS)
    meta["assets"] = sorted(meta["assets"] + added, key=lambda a: (order(a["kind"]), a["name"]))
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f)
    log(len(added), "assets added")


if __name__ == "__main__":
    main(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]))
