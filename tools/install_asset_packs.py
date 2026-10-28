# Adds a curated selection of the MakeHuman community asset packs (https://www.makehumancommunity.org/assets/
# assetpacks/) to the MakeHuman bake: clothes, shoes, underwear, swimwear, accessories, jewellery and beards that look
# like real ones (no costumes, wings, superhero suits or psychedelic prints).
#
#   python tools/install_asset_packs.py [makehuman_bake_dir]
#   blender -b --python tools/blender/bake_mblab_proxies.py -- assets/baked/human assets/baked/mblab
#
# Downloads the packs listed in tools/asset_packs.txt (about 2 GB, into tools/downloads/mhpacks, kept between runs),
# unzips each into its own directory and bakes the assets kept by tools/asset_pack_selection.json ("exclude": left
# out of the kept categories, "unsorted": taken from "Unsorted/other") into <bake>/extra (textures at most 1024 px,
# JPEG where opaque). The extra/ directories are not under version control: run both commands once per checkout.
# The second moves the assets onto the MB-Lab bodies (their extra/ directories).
import glob, json, os, re, shutil, sys, urllib.request, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import human_assets

CATEGORIES = {"Shirt/T-shirt/Sweater", "Pants", "Skirt", "Gown/Robe", "Uniform/Armor", "Underwear",
              "Shoes/Boots/Socks", "Hat/Helmet", "Gloves", "Jewelry", "Accessory", "Facialhair"}


def slot(key, category):
    """The creator slot: by category, refined by the asset's name (underwear and swimwear come as separate pieces)."""
    words = re.split(r"[^a-z0-9]+", key.lower())
    has = lambda *w: any(x in words or any(p.startswith(x) for p in words) for x in w)
    if category in ("Underwear", "Unsorted/other") or has("bikini", "swimwear", "trunks"):
        if has("stocking", "stockings", "pantyhose", "sock", "socks", "warmer"):
            return "legwear"
        if has("slip", "bottom", "panties", "panty", "briefs", "string", "trunks", "booty", "bootyshorts", "shorts"):
            return "underwear"
        if has("bra", "top", "bandeau", "camisole", "babydoll", "shapewear"):
            return "undertop"
        if has("suit", "pantsuit", "wetsuit", "dress", "one", "swimsuit", "swimwear"):
            return "outfit"
        return "underwear"
    if category == "Uniform/Armor" and has("pants", "jacket"):
        return "bottom" if has("pants") else "top"
    if category == "Shoes/Boots/Socks" and has("sock", "socks", "warmer"):
        return "legwear"
    if category == "Accessory" and has("glasses", "glass", "goggles", "goggles1", "sunglass1", "sunglasses"):
        return "glasses"
    return human_assets.SLOTS.get(category)


TOOLS = os.path.dirname(os.path.abspath(__file__))
DOWNLOADS = os.path.join(TOOLS, "downloads", "mhpacks")


def fetch():
    """Downloads and unzips the packs that are not there yet; returns the directory of the unzipped packs."""
    extracted = os.path.join(DOWNLOADS, "extracted")
    os.makedirs(os.path.join(DOWNLOADS, "zips"), exist_ok=True)
    for line in open(os.path.join(TOOLS, "asset_packs.txt"), encoding="utf-8"):
        if not line.split():
            continue
        pack, suffix = line.split()
        name = "%s_%s" % (pack, suffix)
        zip_path = os.path.join(DOWNLOADS, "zips", name + ".zip")
        if not os.path.exists(zip_path):
            url = "https://files.makehumancommunity.org/asset_packs/%s/%s.zip" % (pack, name)
            print("downloading", url, flush=True)
            urllib.request.urlretrieve(url, zip_path + ".part")
            os.replace(zip_path + ".part", zip_path)
        dest = os.path.join(extracted, name)
        if not os.path.isdir(dest):
            with zipfile.ZipFile(zip_path) as z:
                for m in z.namelist():  # no paths out of the pack's directory
                    if not os.path.normpath(os.path.join(dest, m)).startswith(os.path.normpath(dest)):
                        raise ValueError("unsafe path in %s: %s" % (zip_path, m))
                z.extractall(dest)
    return extracted


def main(extracted, selection_path, bake):
    selection = json.load(open(selection_path, encoding="utf-8"))
    exclude, unsorted = set(selection["exclude"]), set(selection["unsorted"])
    staging = os.path.join(os.path.dirname(os.path.abspath(extracted)), "staging")
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(os.path.join(staging, "packs"))
    chosen = {}
    for pack in sorted(glob.glob(os.path.join(extracted, "*"))):
        for meta_path in glob.glob(os.path.join(pack, "packs", "*.json")):
            for key, item in json.load(open(meta_path, encoding="utf-8")).items():
                category = item.get("category")
                keep = (category in CATEGORIES and key not in exclude) or key in unsorted
                folder = os.path.join(pack, "clothes", key)
                if item.get("type") != "clothes" or not keep or not glob.glob(os.path.join(folder, "*.mhclo")):
                    continue
                shutil.copytree(folder, os.path.join(staging, "clothes", key), dirs_exist_ok=True)
                chosen[key] = dict(item, slot=slot(key, category), pack=os.path.basename(pack))
    with open(os.path.join(staging, "packs", "selection.json"), "w", encoding="utf-8") as f:
        json.dump(chosen, f, indent=1)
    counts = {}
    for item in chosen.values():
        counts[item["slot"]] = counts.get(item["slot"], 0) + 1
    print(len(chosen), "assets:", counts, flush=True)
    extra = os.path.join(os.path.abspath(bake), "extra")
    shutil.rmtree(extra, ignore_errors=True)
    human_assets.main(staging, extra, max_size=1024, jpeg=True)


if __name__ == "__main__":
    main(fetch(), os.path.join(TOOLS, "asset_pack_selection.json"), sys.argv[1] if len(sys.argv) > 1 else "assets/baked/human")
