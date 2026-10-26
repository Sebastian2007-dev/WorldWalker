The body mesh, morph targets, Mixamo rig definition and weights were exported from the locally installed MPFB extension's MakeHuman data using `tools/blender/bake_human.py`.

Eyes, eyebrows, eyelashes, hair, clothing and skin textures come from the official MakeHuman system assets pack, released under CC0:
https://static.makehumancommunity.org/assets/assetpacks/makehuman_system_assets.html

Pack URL: https://files.makehumancommunity.org/asset_packs/makehuman_system_assets/makehuman_system_assets_cc0.zip

The MakeHuman base mesh and system asset headers identify the September 2020 CC0 release and the copyright holders: Data Collection AB, Joel Palmius and Jonas Hauquier. Original source headers and asset provenance remain in `assets/human/`, including `packs/makehuman_system_assets.json`.

Additional hairstyles come from the CC0 community pack `hair01` (25 styles by Cortu, culturalibre, Elvaerwyn, Faydaen, learning, littleright, punkduck/o4saken, RehmanPolanski, sonntag78 and Margaret Toigo):
https://static.makehumancommunity.org/assets/assetpacks/hair01.html

Pack URL: https://files.makehumancommunity.org/asset_packs/hair01/hair01_cc0.zip

Further hairstyles come from the community packs `hair02` (21 styles by Elvaerwyn) and `hair03` (14 styles by culturalibre, grinsegold and punkduck/o4saken). These are licensed **CC-BY** and require attribution; every one of them is credited in [CREDITS.md](CREDITS.md), and the character creator names the author while such a style is selected.
https://static.makehumancommunity.org/assets/assetpacks/hair02.html
https://static.makehumancommunity.org/assets/assetpacks/hair03.html

Pack URLs: https://files.makehumancommunity.org/asset_packs/hair02/hair02_ccby.zip, https://files.makehumancommunity.org/asset_packs/hair03/hair03_ccby.zip

Per-asset authors, sources and licences are listed in `assets/human/packs/*.json`. These packs were baked without Blender by `tools/human_assets.py`, which also regenerates `CREDITS.md`.

CC0 1.0 Universal: https://creativecommons.org/publicdomain/zero/1.0/

CC-BY (as linked by the MakeHuman asset FAQ): https://creativecommons.org/licenses/by/2.5/se/deed.en

## Genitals

Baked by `tools/blender/bake_genitals.py` (see `tools/CHARACTER_CREATOR.md`). The sources are MakeHuman 1.1 community topologies, kept in `assets/human/genitals/`; only the part around the crotch is used. They were downloaded from the former MakeHuman asset repository through the Internet Archive (the repository itself is offline):

| Asset | Author | Licence | Source |
|---|---|---|---|
| `genitals_adult_male_genitalia` ("Männlich (realistisch)") | MakeHuman team (Manuel Bastioni), re-uploaded by XSuprem3X | **AGPL-3.0** (file header; the upload page says CC0, which the uploader could not grant) | https://web.archive.org/web/2025/http://www.makehumancommunity.org/proxy/adult_male_genitalia.html_0 |
| `genitals_adult_female_genitalia_remapped` ("Weiblich (realistisch)") | MakeHuman team (Manuel Bastioni), UV remap by MTKnife | **AGPL-3.0** (derived from the AGPL "Adult Female Genitalia"; the remap itself is marked CC0) | https://web.archive.org/web/2025/http://www.makehumancommunity.org/proxy/adult_female_genitalia_remapped.html |
| `genitals_female_generic_with_simplified_genitals_fixed` ("Weiblich (vereinfacht)") | wolgade, fix by spamrakuen | CC0 | https://web.archive.org/web/2025/http://www.makehumancommunity.org/proxy/female_generic_with_simplified_genitals_fixed.html |
| `genitals_male_mpfb` ("Männlich (MakeHuman)") | MakeHuman base mesh genital helper and penis targets | CC0 | MPFB system data |

AGPL-3.0: https://www.gnu.org/licenses/agpl-3.0.html. The two AGPL assets are fine for private use. Before the game is distributed, either ship their sources (`assets/human/genitals/`) under the AGPL and check what that means for the rest of the game, or remove them from `human.json`; the CC0 assets remain.
