# Character creator

Open **Esc → Charakter erstellen**, or run `DEV.bat --creator`.

The creator shares the pause menu's typography, gold accents, tabs and settings controls. Choose **Männlich** or **Weiblich** for a complete adult starting appearance with coordinated proportions, skin and hair, or **Zufällig** for a random plausible adult (body, subtle face variation, matching skin and eyes, hair style and colour, outfit).

- **Körper**: body macros, bust and ancestry mix.
- **Details**: 197 face and body sliders with German labels, grouped by region and searchable. The slider text names the direction (e.g. *runter* / *hoch*); right-click resets a slider.
- **Aussehen**: 22 skins with brightness and warmth, nine eye colours, hair style (70 styles, including the community packs `hair01` (CC0) and `hair02`/`hair03` (CC-BY, credited in `assets/baked/human/CREDITS.md` and next to the selection)) with a volume slider, optional hair colour (presets plus hue, saturation and brightness), eyebrows and eyelashes. A custom hair colour also tints brows and lashes.
- **Kleidung**: outfit, shoes and hat, plus a toggle to show the clothes in the preview.

Rotate and zoom with the two preview sliders; zooming in below about 2 m moves the camera up to the face.

Clothing is hidden during creation until the **Kleidung** tab is opened or its preview toggle is switched on. Each starting template includes an outfit for gameplay.

The character uses dedicated shading: skin with wrapped, colour-dependent scattering and two specular lobes, hair with strand highlights (Kajiya-Kay), and glossy eyes. MakeHuman's transparent cornea shell is alpha-tested away so the iris texture shows.

**Übernehmen** writes `character.json` in the project root. This character loads automatically on the next launch. **Abbrechen** or Esc restores the previous character and camera. **Gespeichert laden** reloads the saved appearance; **Vorlage zurücksetzen** restores the chosen template. Changes remain provisional until saved.

Use `DEV.bat --creator-template female`, `male`, `random` or `random:<seed>` to start directly with a particular template without changing the saved character.

## Rebuilding assets

The runtime reads `assets/baked/human/`. The source system asset pack is in `assets/human/`; provenance is documented in `assets/baked/human/SOURCES.md`. Blender requires the MPFB extension with its base data:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe' -b --python tools/blender/bake_human.py -- assets/human assets/baked/human
```

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
