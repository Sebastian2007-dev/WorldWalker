# Character creator

Open **Esc → Charakter erstellen**, or run `DEV.bat --creator`.

The creator shares the pause menu's typography, gold accents, tabs and settings controls. Choose **Männlich** or **Weiblich** for a complete adult starting appearance with coordinated proportions, skin and hair. Body macros, 197 detail sliders, 22 skins and nine eye colours can be adjusted from there. Rotate and zoom with the two preview sliders.

Clothing and shoes are hidden throughout creation, including when loading an existing character. The full body is visible without clothing deletion masks. The saved outfit is preserved and restored when leaving the creator. Each starting template also includes an outfit for gameplay.

**Übernehmen** writes `character.json` in the project root. This character loads automatically on the next launch. **Abbrechen** or Esc restores the previous character and camera. **Gespeichert laden** reloads the saved appearance; **Vorlage zurücksetzen** restores the chosen template. Changes remain provisional until saved.

Use `DEV.bat --creator-template female` or `DEV.bat --creator-template male` to start directly with a particular template without changing the saved character.

## Rebuilding assets

The runtime reads `assets/baked/human/`. The source system asset pack is in `assets/human/`; provenance is documented in `assets/baked/human/SOURCES.md`. Blender requires the MPFB extension with its base data:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe' -b --python tools/blender/bake_human.py -- assets/human assets/baked/human
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
