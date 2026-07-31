# H4 ship hull placeholders (CC-BY 4.0)

Local staging of [Fab](https://www.fab.com) free packs used as temporary pilot ships and
Space-template decor. Binary FBX/glTF/textures are **not committed** (see root `.gitignore`);
re-stage with:

```powershell
powershell -File tools/stage_ships.ps1
```

## Credits (required under CC-BY 4.0)

| Folder / role | Author | Listing | Fab id |
|---|---|---|---|
| `cyber_ship/` — primary pilot | **JamyzGenius** | Floating Cyber Ship JFG - Roblox Showcase Prop | `64a45a1d-…` |
| `star_ship/` — alternate pilot | **JazOone3D** | SpaceShip | `14265e80-…` |
| `lowpoly/` — alternate pilot | **ABJVNK** | Lowpoly Spaceship | `69cc1137-…` |
| `junkyard/` — wreck prop | **Sebastian Sosnowski** | SpaceShips Junk Yard ASSET (part2) | `81e5d377-…` |
| `station/` (opt-in / vault) | **Gerardo Justel** | Spacestation 7 - Procedural | `bf84b4bd-…` |

Full credit table, license, and modification notes: **[`assets/ATTRIBUTION.md`](../../ATTRIBUTION.md)**.

Suggested one-liner for an in-game About / credits screen:

> Spaceship models: © JamyzGenius, JazOone3D, ABJVNK; junkyard wreck © Sebastian Sosnowski;
> spacestation © Gerardo Justel. CC BY 4.0. Modified: format staging and runtime scale.

## Engine resolve order

`src/game/ShipHulls.h` tries `assets/models/ships/...` first, then the matching
`G:\VaultCache\FabLibrary\...` path so a clean clone still works if the vault is present.
