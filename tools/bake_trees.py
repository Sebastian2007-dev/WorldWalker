"""Bakes all tree models in assets/models into assets/baked/trees using Blender.

    python tools/bake_trees.py [species ...]

Biome tags are matched by the engine's vegetation placer (see src/world/Biome.cpp).
"""
import json, os, subprocess, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS = os.path.join(ROOT, "assets", "models")
OUT = os.path.join(ROOT, "assets", "baked", "trees")
SCRIPT = os.path.join(ROOT, "tools", "blender", "bake_tree.py")


def find_blender():
    env = os.environ.get("BLENDER")
    if env:
        return env
    hits = sorted(glob.glob(r"C:\Program Files\Blender Foundation\Blender *\blender.exe"))
    if not hits:
        sys.exit("Blender not found; set BLENDER=path\\to\\blender.exe")
    return hits[-1]


def abc(prefix, suffix0="", suffix1=""):
    return [{"name": prefix + "_" + c, "lod0": prefix + "_" + c + suffix0, "lod1": prefix + "_" + c + suffix1} for c in "abc"]


SPECIES = [
    {"species": "fir_tree_01", "file": "fir_tree_01_4k", "biomes": ["conifer"],
     "lod0_tris": 45000, "lod1_tris": 12000, "variants": abc("fir_tree_01", "_LOD1", "_LOD2")},
    {"species": "pine_tree_01", "file": "pine_tree_01_4k", "biomes": ["conifer"],
     "lod0_tris": 60000, "lod1_tris": 15000, "min_keep": [0.3, 0.15], "variants": abc("pine_tree_01", "_LOD2", "_LOD2")},
    {"species": "fir_sapling_medium", "file": "fir_sapling_medium_4k", "biomes": ["conifer", "conifer_young"],
     "lod0_tris": 20000, "lod1_tris": 8000, "variants": abc("fir_sapling_medium", "_LOD1", "_LOD2")},
    {"species": "fir_sapling", "file": "fir_sapling_4k", "biomes": ["understory_conifer"],
     "lod0_tris": 20000, "lod1_tris": 8000, "variants": abc("fir_sapling")},
    {"species": "pine_sapling_small", "file": "pine_sapling_small_4k", "biomes": ["understory_conifer"],
     "lod0_tris": 20000, "lod1_tris": 8000, "variants": abc("pine_sapling_small")},
    {"species": "island_tree_01", "file": "island_tree_01_4k", "drop_materials": ["island_tree_01"], "biomes": ["broadleaf"],
     "lod0_tris": 40000, "lod1_tris": 10000,
     "variants": [{"name": "island_tree_01", "lod0": "island_tree_01_LOD1", "lod1": "island_tree_01_LOD1"}]},
    {"species": "island_tree_02", "file": "island_tree_02_4k", "drop_materials": ["island_tree_02"], "biomes": ["broadleaf"],
     "lod0_tris": 40000, "lod1_tris": 10000,
     "variants": [{"name": "island_tree_02", "lod0": "island_tree_02_LOD1", "lod1": "island_tree_02_LOD1"}]},
    {"species": "island_tree_03", "file": "island_tree_03_4k", "drop_materials": ["island_tree_03"], "biomes": ["broadleaf", "shrub"],
     "lod0_tris": 40000, "lod1_tris": 10000,
     "variants": [{"name": "island_tree_03", "lod0": "island_tree_03_LOD1", "lod1": "island_tree_03_LOD1"}]},
    {"species": "tree_small_02", "file": "tree_small_02_4k", "biomes": ["broadleaf", "shrub"],
     "lod0_tris": 30000, "lod1_tris": 8000,
     "variants": [{"name": "tree_small_02", "lod0": "tree_small_02_LOD1", "lod1": "tree_small_02_LOD1"}]},
    {"species": "jacaranda_tree", "file": "jacaranda_tree_4k", "biomes": ["tropical"],
     "lod0_tris": 60000, "lod1_tris": 15000,
     "variants": [{"name": "jacaranda_tree", "lod0": "jacaranda_tree_LOD1", "lod1": "jacaranda_tree_LOD1"}]},
    {"species": "quiver_tree_01", "file": "quiver_tree_01_4k", "biomes": ["arid"],
     "lod0_tris": 17000, "lod1_tris": 4000,
     "variants": [{"name": "quiver_tree_01", "lod0": "quiver_tree_01_LOD3", "lod1": "quiver_tree_01_LOD3"}]},
    {"species": "quiver_tree_02", "file": "quiver_tree_02_4k", "biomes": ["arid"],
     "lod0_tris": 10300, "lod1_tris": 2500,
     "variants": [{"name": "quiver_tree_02", "lod0": "quiver_tree_02_LOD3", "lod1": "quiver_tree_02_LOD3"}]},
]


def main():
    only = set(sys.argv[1:])
    blender = find_blender()
    os.makedirs(OUT, exist_ok=True)
    index = []
    for cfg in SPECIES:
        if only and cfg["species"] not in only:
            index.append(cfg["species"])
            continue
        blend = os.path.join(MODELS, cfg["file"] + ".blend", cfg["file"] + ".blend")
        print("==>", cfg["species"], flush=True)
        r = subprocess.run([blender, "-b", blend, "--python-exit-code", "1", "--python", SCRIPT, "--",
                            json.dumps(cfg), OUT])
        if r.returncode != 0:
            sys.exit("bake failed for " + cfg["species"])
        index.append(cfg["species"])
    with open(os.path.join(OUT, "index.json"), "w") as f:
        json.dump({"species": index}, f, indent=2)


if __name__ == "__main__":
    main()
