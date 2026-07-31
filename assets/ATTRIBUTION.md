# Asset attribution

Every third-party asset used by MeatEngine (staged under `assets/` **or** loaded at
runtime from the local Fab vault) is listed here. Licenses are **CC-BY 4.0** or **CC0**
only for art we ship or load for gameplay; anything else does not get committed.

**CC-BY 4.0 requires** naming the author(s) and indicating changes. “Modified” covers
format conversion, path staging, runtime scale/center at import, and texture path overrides.

Author names below come from the local Fab listing cache (`G:\VaultCache\FabLibrary\listings_v1.db`,
field `user_seller_name`), cross-checked against listing titles and product folder IDs.

---

## Ships & space decor (H4)

| Local path / role | Author (Fab seller) | Listing title | Fab product id | License | Modified |
|---|---|---|---|---|---|
| `models/ships/cyber_ship/*` — primary pilot hull | **JamyzGenius** | Floating Cyber Ship JFG - Roblox Showcase Prop | `64a45a1d-8c22-4097-b2ba-bf352105b446` | CC-BY 4.0 | staged FBX + albedo/emissive; scaled/centered at load |
| `models/ships/star_ship/*` — alternate pilot hull | **JazOone3D** | SpaceShip | `14265e80-7cc8-4238-873c-8b970f44f552` | CC-BY 4.0 | staged FBX + base color/emissive; scaled/centered at load |
| `models/ships/lowpoly/*` — alternate pilot hull | **ABJVNK** | Lowpoly Spaceship | `69cc1137-9240-4381-adac-bc460ca62c38` | CC-BY 4.0 | staged glTF + freeble baseColor/emissive only; scaled/centered at load |
| `models/ships/junkyard/*` — Space-template wreck prop | **Sebastian Sosnowski** | SpaceShips Junk Yard ASSET (part2) | `81e5d377-fcff-4033-a0e9-47f1d3552196` | CC-BY 4.0 | staged FBX + base color; sized via world transform |
| `models/ships/station/*` (opt-in) or vault runtime | **Gerardo Justel** ([ArtStation](https://www.artstation.com/re1mon)) | Spacestation 7 - Procedural | `bf84b4bd-71d7-447d-96ba-9491646fd00f` | CC-BY 4.0 | vault load by default (`MEAT_STAGE_STATION=1` to copy); sized via world transform |
| *(related, not currently loaded)* | **Sebastian Sosnowski** | SpaceShips Junk Yard ASSET (part1) | `0bd74307-0a43-48e6-b346-cf1ef26d4c15` | CC-BY 4.0 | vault present; not wired in code yet |

**Suggested credit line** (README, About box, or pause screen):

> Spaceship models: © JamyzGenius, JazOone3D, ABJVNK; junkyard wreck © Sebastian Sosnowski;
> spacestation © Gerardo Justel. Used under CC BY 4.0. Modified: format staging and runtime scale.

**Re-stage binaries** (not committed; see `.gitignore`):

```powershell
powershell -File tools/stage_ships.ps1
# optional large station pack:
$env:MEAT_STAGE_STATION = "1"; powershell -File tools/stage_ships.ps1
```

Path map and vault fallbacks: `src/game/ShipHulls.h`, `assets/models/ships/README.md`.

---

## Other project assets

| Asset | Author | Source | License | Modified |
|---|---|---|---|---|
| `models/prop_crate.obj` | MeatEngine | this repo | Apache-2.0 (project) | authored for the repo |

Vault FBX/GLB staged for local testing (NPC packs, Mixamo, etc.) must gain a row here with a
**verified** license and author before they are committed or shipped. See also
`docs/ANIMATION_RETARGETING.md` for animation pack licensing notes.

---

## Process

1. Prefer CC-BY 4.0 / CC0 only for gameplay art.
2. Record **author**, **listing title**, **Fab product id** (folder suffix UUID), and **modifications**.
3. `tools/stage_assets.py` / `tools/stage_ships.ps1` copy assets in; this file stays the credit ledger.
4. An asset without a complete row must not ship in a public package.
