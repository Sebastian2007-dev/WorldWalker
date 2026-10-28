# Adds a curated selection of the MakeHuman community asset packs (https://www.makehumancommunity.org/assets/
# assetpacks/) to the MakeHuman bake: clothes, shoes, underwear, swimwear, accessories, jewellery and beards that look
# like real ones (no costumes, wings, superhero suits or psychedelic prints).
#
#   python tools/install_asset_packs.py <extracted_packs_dir> tools/asset_pack_selection.json [makehuman_bake_dir]
#
# The packs: tools/asset_packs.txt (pack, licence suffix), from
# https://files.makehumancommunity.org/asset_packs/<pack>/<pack>_<suffix>.zip, each unzipped into its own directory.
#
# <extracted_packs_dir> holds one directory per downloaded pack zip (clothes/<asset>/..., packs/<pack>.json).
# selection.json: {"exclude": [asset, ...], "unsorted": [asset, ...]}: assets left out of the kept categories, and the
# ones taken from "Unsorted/other". The selection is copied to <extracted_packs_dir>/../staging and baked with
# human_assets.py (textures at most 1024 px, JPEG where opaque). Then bake_mblab_proxies.py moves them onto MB-Lab.
import glob, json, os, re, shutil, sys

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
    human_assets.main(staging, os.path.abspath(bake), max_size=1024, jpeg=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "assets/baked/human")
