-- example.lua — MeatEngine gameplay script (server-authoritative).
-- Scripts get the `game` capability table; see engine/script/ScriptHost.h.
-- Define any of on_init / on_player_join / on_player_death / on_tick.

local ammo = 0

function on_init(seed)
  game.log("world init, seed " .. math.floor(seed))
  ammo = game.item_id("ammo9mm")
  -- Scatter a few ammo caches near spawn, deterministically from the seed.
  for i = 1, 5 do
    local x = 8 + game.randi(-6, 6)
    local z = 8 + game.randi(-6, 6)
    game.spawn_pickup(x, 5.0, z, ammo, 24)
  end
  -- Build a small stone marker pillar at spawn (replayed to every client on join).
  local stone = game.item_id("stone")
  for y = 8, 11 do game.set_block(4, y, 4, 1) end
  game.log("scattered 5 ammo caches, raised a marker pillar")
end

function on_player_join(peer)
  game.log("player " .. math.floor(peer) .. " joined — " ..
           math.floor(game.player_count()) .. " online")
end

-- Runs ~3x/second. Keep it light; it's on the server tick.
local next_drop = 200
function on_tick(t)
  if t > next_drop and game.player_count() > 0 then
    next_drop = t + 600 -- ~10 s later
    game.spawn_pickup(8, 5.0, 8, game.item_id("medkit"), 1)
    game.log("dropped a medkit at spawn")
  end
end
