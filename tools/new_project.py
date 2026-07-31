"""Scaffold a new MeatEngine game project.

    python tools/new_project.py MyGame [--dir F:\\Games] [--template fps|tps|space]

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
{loot}
end

function on_player_join(peer)
  game.log("welcome, player " .. math.floor(peer))
end

function on_tick(t)
  -- Runs ~3x/second on the server. Put recurring game logic here.
end
"""

LOOT_DEFAULT = """\
  -- Scatter some starting loot deterministically from the seed.
  for i = 1, 8 do
    local x = 8 + game.randi(-8, 8)
    local z = 8 + game.randi(-8, 8)
    game.spawn_pickup(x, 5.0, z, game.item_id("ammo9mm"), 24)
  end
"""

LOOT_SPACE = """\
  -- Space template: light pad-side loot (ships are the main content).
  for i = 1, 4 do
    local x = 8 + game.randi(-4, 4)
    local z = 8 + game.randi(-4, 4)
    game.spawn_pickup(x, 9.0, z, game.item_id("rockets"), 2)
  end
"""

README = """\
# {name}

A game built on MeatEngine{template_note}.

## Run
    MeatEngine.exe --project . --play        # singleplayer
    MeatEngine.exe --project . --host         # host multiplayer
    MeatEngine.exe --project . --server       # dedicated server

## Layout
- game.json    — name, seed, rules; map defaults under `"world": { template, terrain, environment }`
- scripts/     — Lua gameplay (edit main.lua; save reloads on next launch)
- assets/      — your textures/models/sounds (optional; engine assets are the fallback)

## Credits
Third-party art is CC-BY/CC0 — see the engine's assets/ATTRIBUTION.md (ship authors
must be credited if you ship H4 hulls).

## Package to a shippable build
    powershell tools/package.ps1 -Project . -Out dist
"""


def template_rules(kind: str) -> dict:
    """Return game.json fields including B5 nested world defaults."""
    if kind == "tps":
        world = {
            "template": "tps",
            "perspective": "third",
            "terrain": "normal",
            "environment": "surface",
        }
        extra = {}
    elif kind == "space":
        world = {
            "template": "space",
            "perspective": "first",
            "terrain": "void",
            "environment": "space",
            "hemisphereAmbient": False,
        }
        extra = {"finiteAmmo": True, "minedBlockDrops": False}
    elif kind == "racer":
        world = {
            "template": "racer",
            "perspective": "third",
            "terrain": "superflat",
            "environment": "surface",
        }
        extra = {}
    else:
        world = {
            "template": "fps",
            "perspective": "first",
            "terrain": "normal",
            "environment": "surface",
        }
        extra = {}
    out = {"world": world}
    out.update(extra)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--dir", default=".")
    ap.add_argument(
        "--template",
        choices=("fps", "tps", "space", "racer"),
        default="fps",
        help="game genre preset (default: fps)",
    )
    args = ap.parse_args()

    root = Path(args.dir) / args.name
    if root.exists():
        raise SystemExit(f"'{root}' already exists")
    (root / "scripts").mkdir(parents=True)
    (root / "assets").mkdir()

    game = {
        "name": args.name,
        "seed": 1337,
        "inventoryModel": "hotbar",  # hotbar | grid | weapons
        "finiteAmmo": True,
        "minedBlockDrops": True,
        "penetration": True,
        "blockDamage": True,
    }
    game.update(template_rules(args.template))

    loot = LOOT_SPACE if args.template == "space" else LOOT_DEFAULT
    note = {
        "fps": "",
        "tps": " (third-person template)",
        "space": " (space ship template — Void + Space env, ships on pad)",
        "racer": " (racer template — superflat track pad)",
    }[args.template]

    (root / "game.json").write_text(json.dumps(game, indent=2), encoding="utf-8")
    (root / "scripts" / "main.lua").write_text(
        MAIN_LUA.format(name=args.name, loot=loot), encoding="utf-8"
    )
    (root / "README.md").write_text(
        README.format(name=args.name, template_note=note), encoding="utf-8"
    )
    print(f"created project '{args.name}' at {root}  [template={args.template}]")
    print(f"run it:  MeatEngine.exe --project {root} --play")


if __name__ == "__main__":
    main()
