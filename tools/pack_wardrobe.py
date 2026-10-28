# Packs the baked wardrobe (the extra/ directories written by install_asset_packs.py and bake_mblab_proxies.py) into
# one zip for a GitHub release, and describes it in assets/wardrobe/package.json, which the game reads
# (src/data/Wardrobe.cpp: downloads the zip, unpacks it into the same directories).
#
#   python tools/pack_wardrobe.py <version> [out_dir]
#
# Upload <out_dir>/wardrobe-v<version>.zip as an asset of the release tagged wardrobe-v<version>, then commit
# package.json. Paths in the zip are relative to assets/baked; images are stored, everything else deflated; the
# catalogs come last.
import json, os, sys, zipfile

REPO = "https://github.com/Sebastian2007-dev/WorldWalker"
BAKED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "baked")
DIRS = ["human/extra", "mblab/female/extra", "mblab/male/extra"]


def main(version, out_dir):
    name = "wardrobe-v%d.zip" % version
    path = os.path.join(out_dir, name)
    files = []
    for d in DIRS:
        root = os.path.join(BAKED, d)
        if not os.path.exists(os.path.join(root, "human.json")):
            sys.exit("missing %s/human.json: bake the wardrobe first" % d)
        for base, _, names in os.walk(root):
            for n in names:
                if n == "wardrobe.json":
                    continue  # the game's marker of an unpacked version
                full = os.path.join(base, n)
                files.append((os.path.relpath(full, BAKED).replace(os.sep, "/"), full))
    files.sort(key=lambda f: (f[0].endswith("human.json"), f[0]))
    with zipfile.ZipFile(path, "w") as z:
        for arc, full in files:
            stored = arc.endswith((".png", ".jpg"))
            z.write(full, arc, zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED)
    size = os.path.getsize(path)
    package = {"version": version, "bytes": size, "files": len(files),
               "url": "%s/releases/download/wardrobe-v%d/%s" % (REPO, version, name)}
    os.makedirs(os.path.join(BAKED, "..", "wardrobe"), exist_ok=True)
    with open(os.path.join(BAKED, "..", "wardrobe", "package.json"), "w", encoding="utf-8") as f:
        json.dump(package, f, indent=1)
    print(path, "%.0f MB," % (size / 1e6), len(files), "files")


if __name__ == "__main__":
    main(int(sys.argv[1]), sys.argv[2] if len(sys.argv) > 2 else ".")
