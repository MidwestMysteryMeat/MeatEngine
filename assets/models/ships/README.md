# H4 ship hull placeholders (CC-BY 4.0)

Local staging of Fab Library packs used as temporary pilot ships. Binary FBX/glTF
and textures are **not committed** (see root `.gitignore` / size); re-stage with:

```powershell
powershell -File tools/stage_ships.ps1
```

| Folder | Source (Fab / VaultCache) | License | Role |
|--------|---------------------------|---------|------|
| `cyber_ship/` | Floating Cyber Ship JFG (Roblox Showcase Prop) | CC-BY 4.0 | Primary pilot hull |
| `star_ship/` | SpaceShip-14265e80 | CC-BY 4.0 | Alternate hull |
| `lowpoly/` | Lowpoly Spaceship (glTF) | CC-BY 4.0 | PSX-friendly silhouette |
| `junkyard/` | SpaceShips Junk Yard ASSET part2 | CC-BY 4.0 | Space-template wreckage prop |

Spacestation_7 is vault-only (large); the engine can load it from
`G:\VaultCache\FabLibrary\...` when present.

**Attribution:** list every staged asset in `assets/ATTRIBUTION.md` (required by CC-BY).
Engine resolves `assets/models/ships/...` first, then the VaultCache path in
`src/game/ShipHulls.h`.
