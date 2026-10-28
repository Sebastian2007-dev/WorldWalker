# WorldWalker

A C++20/Vulkan world exploration project with streamed terrain, vegetation, animated characters and an in-game character creator.

## Getting started (Windows)

Install Visual Studio C++ Build Tools, CMake, Ninja, the Vulkan SDK and Git LFS. The repository includes runtime assets and Blender/MakeHuman source assets; binary files are stored with Git LFS.

```powershell
git lfs install
git clone https://github.com/Sebastian2007-dev/WorldWalker.git
cd WorldWalker
git lfs pull
.\DEV.bat
```

Use `build.bat` to build without launching. Open the character creator through **Esc → Charakter erstellen**, or start with `DEV.bat --creator`. See [the creator documentation](tools/CHARACTER_CREATOR.md) for templates, asset baking and integration tests.

### Clothing (optional, once per checkout)

The character's wardrobe (about 300 garments, shoes, underwear, accessories and the MakeHuman hairstyles on the MB-Lab bodies) is baked locally from the MakeHuman community asset packs and kept out of version control (about 700 MB). Without it the character has only its built-in hairstyles. With Python 3 (Pillow, NumPy) and Blender 5:

```powershell
python tools\install_asset_packs.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --python tools\blender\bake_mblab_proxies.py -- assets\baked\human assets\baked\mblab
```

The first downloads the packs listed in `tools/asset_packs.txt` (about 2 GB, kept in `tools/downloads`) and bakes the selection in `tools/asset_pack_selection.json` into `assets/baked/human/extra`; the second moves it onto the MB-Lab bodies (`assets/baked/mblab/*/extra`). Most of these assets are CC-BY: their authors are listed in `assets/baked/human/extra/CREDITS.md`.

## Graphics quality

**Esc → Optionen → Grafik** offers the presets Niedrig, Mittel, Hoch and Ultra. On first start the game picks one for the detected GPU (integrated or under 3 GB video memory: Niedrig, under 6 GB: Mittel, otherwise Hoch). Individual settings: render resolution (50–100 %, the UI stays sharp), dynamic resolution with a target frame rate, shadow quality, grass density, terrain and tree detail, TAA, SSAO, VSync and a frame-rate limit. The debug overlay (F3) shows the GPU time per render stage, which tells you what limits a slow machine. `--quality low|medium|high|ultra` overrides the preset for a run.

Build outputs, downloaded terrain caches, local settings and saved personal characters are excluded from version control.

Asset provenance for the MakeHuman character data is documented in [SOURCES.md](assets/baked/human/SOURCES.md). Some hairstyles are CC-BY; their authors are credited in [CREDITS.md](assets/baked/human/CREDITS.md).
