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
| `levelType` | `voxel` (default) or `mesh` — mesh forces Void terrain if terrain omitted |
| `meshLevel` / `levelMesh` | string path **or** object `{ asset, scale, pos, yawDeg }` |
| `meshLevels` | array of strings or objects (multi-mesh map) |
| `meshLevelScale` | default scale for instances that omit `scale` |

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

## B2 MeshLevel examples

Single mesh:

```json
"world": {
  "levelType": "mesh",
  "meshLevel": "assets/models/prop_crate.obj",
  "meshLevelScale": 20.0
}
```

Multi-mesh map (parts with transform):

```json
"world": {
  "levelType": "mesh",
  "meshLevelScale": 1.0,
  "meshLevels": [
    { "asset": "assets/models/hangar.obj", "pos": [0, 0, 0], "scale": 1.0 },
    { "asset": "assets/models/prop_crate.obj", "pos": [12, 0, 4], "yawDeg": 90, "scale": 2.0 }
  ]
}
```

Each part gets a triangle collider on server + client. Prefer Void terrain so meshes are the
floor (automatic when `levelType` is `mesh` or any mesh level is set without explicit terrain).

## Examples

- `docs/examples/space.game.json`
- `docs/examples/mesh_level.game.json`
- `projects/SpaceSlice/game.json`
