# Character creator

Open **Esc → Charakter erstellen**, or run `DEV.bat --creator`.

The creator shares the pause menu's typography, gold accents, tabs and settings controls. Choose **Männlich** or **Weiblich** for a complete adult starting appearance with coordinated proportions, skin and hair, or **Zufällig** for a random plausible adult (body, subtle face variation, matching skin and eyes, hair style and colour, outfit).

- **Körper**: body macros, bust and ancestry mix.
- **Details**: 197 face and body sliders with German labels, grouped by region and searchable. The slider text names the direction (e.g. *runter* / *hoch*); right-click resets a slider.
- **Aussehen**: 22 skins with brightness and warmth, nine eye colours, hair style (70 styles, including the community packs `hair01` (CC0) and `hair02`/`hair03` (CC-BY, credited in `assets/baked/human/CREDITS.md` and next to the selection)) with a volume slider, optional hair colour (presets plus hue, saturation and brightness), eyebrows and eyelashes. A custom hair colour also tints brows and lashes.
- **Kleidung**: outfit, shoes and hat, plus a toggle to show the clothes in the preview.

In the preview area, drag with the left mouse button to turn and tilt the camera, use the wheel to zoom and drag with the right mouse button to move the view up or down; a double click returns to the full body view. **Ganzkörper**, **Oberkörper** and **Gesicht** under *Vorschau* jump to those views. Zooming in moves the focus up to the face, and the character stays centred in the free area beside the panel.

Clothing is hidden during creation until the **Kleidung** tab is opened or its preview toggle is switched on. Without an outfit the character wears plain underwear; **Unterwäsche** in the *Kleidung* tab switches it off. **Körper → Intimbereich** chooses the genitals and their sliders (see below). Each starting template includes an outfit for gameplay.

The body is smoothed with one Catmull-Clark step at load time (`Human::subdivide`). The bake fans every quad into two triangles, which are paired back into quads. Morph targets, bones and fitted assets keep working on the original vertex indices. Plain underwear (briefs, and a bandeau for female bodies) is generated on the body whenever no outfit is worn.

The character uses dedicated shading: skin with wrapped, colour-dependent scattering, two specular lobes, procedural pore relief and darkening in creases computed from the mesh curvature; hair with strand highlights (Kajiya-Kay); and glossy eyes. MakeHuman's transparent cornea shell is alpha-tested away so the iris texture shows. While the creator is open, camera-fixed studio lights (key, fill, rim) take over part of the daylight.

**Übernehmen** writes `character.json` in the project root. This character loads automatically on the next launch. **Abbrechen** or Esc restores the previous character and camera. **Gespeichert laden** reloads the saved appearance; **Vorlage zurücksetzen** restores the chosen template. Changes remain provisional until saved.

Use `DEV.bat --creator-template female`, `male`, `random` or `random:<seed>` to start directly with a particular template without changing the saved character. `--creator-distance <m>`, `--creator-orbit <deg>` and `--creator-lift <m>` set the preview camera, and `--creator-nude` switches the underwear off, for example for close-up screenshots.

## Rebuilding assets

The runtime reads `assets/baked/human/`. The source system asset pack is in `assets/human/`; provenance is documented in `assets/baked/human/SOURCES.md`. Blender requires the MPFB extension with its base data:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe' -b --python tools/blender/bake_human.py -- assets/human assets/baked/human
```

Two further Blender steps build on that bake (run them after `bake_human.py`, which rewrites `human.json`):

```powershell
# Genitals: the MakeHuman topologies in assets/human/genitals, MakeHuman's male helper geometry as a CC0 fallback,
# and/or genitals_* objects of your own .blend
$g = 'assets/human/genitals'
& 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe' -b [my_genitals.blend] --python tools/blender/bake_genitals.py -- assets/baked/human `
    --proxy "$g/adult_male_genitalia/adult_male_genitalia.proxy" 'Männlich (realistisch)' `
    --proxy "$g/adult_female_genitalia_remapped/adult_female_genitalia_remapped.proxy" 'Weiblich (realistisch)' `
    --proxy "$g/female_generic_with_simplified_genitals_fixed/female_generic_with_simplified_genitals_fixed.proxy" 'Weiblich (vereinfacht)' `
    --placeholder
# Sculpted anatomy from Blender Studio's CC0 "Human Base Meshes" bundle (blender.org/download/demo, Asset Bundles)
& 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe' -b --python tools/blender/bake_realism.py -- path/to/human_base_meshes_bundle.blend assets/baked/human
```

**Genitals** are fitted assets of the kind `genitals`. To make your own, model or sculpt a mesh named `genitals_<name>` on an MPFB human in Blender (default scale), on the body shape it is meant for. Shape keys become sliders: a `<x>-decr` / `<x>-incr` pair is one slider from -1 to 1, any other key a slider from 0 to 1. Custom properties `label` (shown in the creator) and `sex` (`male` / `female`, used by the templates) are optional. A vertex group `delete_<object name>` on the MPFB body hides that skin while the asset is shown, and shape keys `body:<x>` on the body (set to their weight) reshape the body while it is shown. The bake fits every vertex to the nearest body triangle and takes its UV from there, so the asset uses the body's skin texture and blends into it. Where it stands out from the body it follows the hip bone rigidly; where it lies on the skin it bends with the body. Genitals are only shown when the character wears neither an outfit nor underwear.

`--proxy <file.proxy> <label>` takes a MakeHuman topology (a whole-body replacement mesh for hm08 with genitals). The bake keeps only what departs from the body near the crotch plus 3.5 cm of skin around it, and hides the body skin underneath. At load time the engine moves such skin assets from the coarse body onto the smoothed one, and they take the body's normals, crease shading and skin detail where they lie on it, so no edge shows. The templates pick the first asset of each sex: the realistic MakeHuman ones, which are **AGPL-3.0** (see `assets/baked/human/SOURCES.md` before distributing the game). The CC0 alternatives are "Weiblich (vereinfacht)" and the coarse "Männlich (MakeHuman)".

**Sculpted anatomy** wraps trunk and legs of the MakeHuman body onto the realistic male and female sculpts (the "Definition" slider, blended by body form) and bakes the sculpts' detail into normal maps on the MakeHuman UVs. Head, arms, hands and feet keep MakeHuman's shape: the sculpts' arms hang at another angle and their faces are other people.

Fitted assets (hair, clothes, eyes, ...) do not need Blender. To add new assets from the pack to an existing bake, run (requires NumPy and Pillow):

```powershell
python tools/human_assets.py assets/human assets/baked/human
```

## Validation

Configure with `-DWW_BUILD_TESTS=ON`, build, then run:

```powershell
ctest --test-dir build/Release --output-on-failure
```

`HumanTests` opens an invisible Vulkan window. It checks preset round trips and rollback on corrupt input, extreme body shapes, every fitted asset, and repeated mesh replacement while GPU uploads and frames are in flight. Vulkan validation messages appear in the test output. The test requires a Vulkan-capable GPU.

A rendered preview can also be generated without opening a visible window:

```powershell
build/Release/bin/WorldWalker.exe --creator --shot build/creator-preview.png --validation
```
