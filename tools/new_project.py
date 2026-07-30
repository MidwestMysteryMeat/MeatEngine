"""Scaffold a new MeatEngine game project.

    python tools/new_project.py MyGame [--dir F:\\Games]

Creates <dir>/MyGame/ with game.json, scripts/main.lua, assets/, and a README —
a working game you can immediately run:  MeatEngine.exe --project <dir>/MyGame --play
"""
import argparse
import json
from pathlib import Path

MAIN_LUA = """\
-- {name} — MeatEngine game script (server-authoritative).
-- The `game` table is your API; see the engine's ScriptHost for the full surface.

function on_init(seed)
  game.log("{name} starting, seed " .. math.floor(seed))
  -- Scatter some starting loot deterministically from the seed.
  for i = 1, 8 do
    local x = 8 + game.randi(-8, 8)
    local z = 8 + game.randi(-8, 8)
    game.spawn_pickup(x, 5.0, z, game.item_id("ammo9mm"), 24)
  end
end

function on_player_join(peer)
  game.log("welcome, player " .. math.floor(peer))
end

function on_tick(t)
  -- Runs ~3x/second on the server. Put recurring game logic here.
end
"""

README = """\
# {name}

A game built on MeatEngine.

## Run
    MeatEngine.exe --project . --play        # singleplayer
    MeatEngine.exe --project . --host         # host multiplayer
    MeatEngine.exe --project . --server       # dedicated server

## Layout
- game.json    — name, seed, and rules (inventory model, ammo/economy toggles)
- scripts/     — Lua gameplay (edit main.lua; save reloads on next launch)
- assets/      — your textures/models/sounds (optional; engine assets are the fallback)

## Package to a shippable build
    powershell tools/package.ps1 -Project . -Out dist
"""


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--dir", default=".")
    args = ap.parse_args()

    root = Path(args.dir) / args.name
    if root.exists():
        raise SystemExit(f"'{root}' already exists")
    (root / "scripts").mkdir(parents=True)
    (root / "assets").mkdir()

    game = {
        "name": args.name,
        "seed": 1337,
        "inventoryModel": "hotbar",   # hotbar | grid | weapons
        "finiteAmmo": True,
        "minedBlockDrops": True,
        "penetration": True,
        "blockDamage": True,
    }
    (root / "game.json").write_text(json.dumps(game, indent=2), encoding="utf-8")
    (root / "scripts" / "main.lua").write_text(MAIN_LUA.format(name=args.name), encoding="utf-8")
    (root / "README.md").write_text(README.format(name=args.name), encoding="utf-8")
    print(f"created project '{args.name}' at {root}")
    print(f"run it:  MeatEngine.exe --project {root} --play")


if __name__ == "__main__":
    main()
