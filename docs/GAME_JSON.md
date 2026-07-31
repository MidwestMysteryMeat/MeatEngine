# game.json schema (B5)

A **game project** is a folder with `game.json` plus optional `scripts/` and `assets/`.
Run with:

```text
MeatEngine.exe --project path/to/project --play
```

CLI flags (`--template`, `--terrain`, `--env`, `--seed`, …) still override after load.

## Top-level keys

| Key | Type | Notes |
|-----|------|--------|
| `name` | string | Server / project display name |
| `seed` | uint | World seed (also allowed under `world`) |
| `inventoryModel` | string | `hotbar` / `grid` / `weapons` |
| `finiteAmmo`, `minedBlockDrops`, `penetration`, `blockDamage` | bool | Combat / mining rules |
| `voxelSize` | float | Metres per voxel (default 0.5) |
| `template`, `terrain`, `environment`, `perspective`, `hemisphereAmbient` | | **Legacy** top-level map keys (still work) |

## B5 nested `world` object

Preferred place for map defaults. When both top-level and `world` set the same field,
**`world` wins**.

```json
{
  "name": "MyGame",
  "seed": 1337,
  "inventoryModel": "hotbar",
  "world": {
    "template": "space",
    "terrain": "void",
    "environment": "space",
    "perspective": "first",
    "hemisphereAmbient": false
  }
}
```

### `world` / map field values

| Field | Values |
|-------|--------|
| `template` | `fps`, `tps` / `third`, `space` / `spaceship`, `racer` / `race` |
| `terrain` | `normal`, `superflat`, `void` |
| `environment` | `surface`, `underwater`, `space` |
| `perspective` | `first`, `third` / `tps` |
| `seed` | unsigned int |
| `hemisphereAmbient` | bool (A3 sky/ground fill) |

### Template presets

Applying `template` first sets genre defaults; explicit `terrain` / `environment` /
`perspective` after it (same object) override.

| Template | Terrain | Environment | Perspective | Notes |
|----------|---------|-------------|-------------|--------|
| `fps` | (unchanged) | (unchanged) | first | |
| `tps` | (unchanged) | (unchanged) | third | |
| `space` | void | space | first | hemi off |
| `racer` | superflat | surface | third | |

## Scaffold

```text
python tools/new_project.py MyGame --template space
```

Writes a project with nested `world` for the chosen genre (`fps` / `tps` / `space` / `racer`).

## Examples

- `docs/examples/space.game.json`
- `projects/SpaceSlice/game.json`
