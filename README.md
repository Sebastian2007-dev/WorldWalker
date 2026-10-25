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

Build outputs, downloaded terrain caches, local settings and saved personal characters are excluded from version control.

Asset provenance for the MakeHuman character data is documented in [SOURCES.md](assets/baked/human/SOURCES.md).
