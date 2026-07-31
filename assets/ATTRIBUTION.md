# Asset attribution

Every third-party asset bundled in `assets/` is listed here. Licenses are CC-BY 4.0 or
CC0 only; anything else does not get committed. "Modified" notes conversions (format,
scale, retopo, rig) — CC-BY requires indicating changes.

| Asset | Author | Source | License | Modified |
|---|---|---|---|---|
| models/prop_crate.obj | MeatEngine | this repo | Apache-2.0 (project) | authored for the repo |
| models/ships/cyber_ship/* | JFG (Fab) | Floating Cyber Ship JFG — Roblox Showcase Prop (`Floating_Cyber_Ship_JFG_-_Roblox_Showcase_Prop-64a45a1d`) | CC-BY 4.0 | format/path staging only; scaled at load |
| models/ships/star_ship/* | Fab listing | SpaceShip (`SpaceShip-14265e80`) | CC-BY 4.0 | format/path staging only; scaled at load |
| models/ships/lowpoly/* | Fab listing | Lowpoly Spaceship (`Lowpoly_Spaceship-69cc1137`) | CC-BY 4.0 | freeble textures only staged; scaled at load |
| models/ships/junkyard/* | Fab listing | SpaceShips Junk Yard ASSET part2 (`SpaceShips_Junk_Yard_ASSET__part2_-81e5d377`) | CC-BY 4.0 | format/path staging only; 0.01 scale as prop |
| models/ships/station/* (opt-in stage) / vault | Fab listing | Spacestation 7 Procedural (`Spacestation_7_-_Procedural-bf84b4bd`) | CC-BY 4.0 | runtime vault load by default; `MEAT_STAGE_STATION=1` to copy |
| _(other vault FBX/GLB staged locally need a row before public commit)_ | | | | |

Process: `tools/stage_assets.py` copies an asset in, converts if needed, and appends a
row here. An asset without a row (or a row without a human-verified license) fails
`tools/audit_assets.py` and must not ship.
