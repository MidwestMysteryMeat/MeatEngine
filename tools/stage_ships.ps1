# Stage CC-BY 4.0 Fab ship packs into assets/models/ships/ for H4.
# Safe to re-run. Does not commit binaries.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dst = Join-Path $root "assets\models\ships"

function Ensure-Dir($p) { New-Item -ItemType Directory -Force -Path $p | Out-Null }

Ensure-Dir "$dst\cyber_ship"
Ensure-Dir "$dst\star_ship"
Ensure-Dir "$dst\lowpoly\textures"
Ensure-Dir "$dst\junkyard"

$cyber = "G:\VaultCache\FabLibrary\Floating_Cyber_Ship_JFG_-_Roblox_Showcase_Prop-64a45a1d\fbx\floating-cyber-ship-jfg-_extracted"
$star = "G:\VaultCache\FabLibrary\SpaceShip-14265e80\fbx\spaceship_extracted"
$low = "G:\VaultCache\FabLibrary\Lowpoly_Spaceship-69cc1137\gltf\converted\lowpoly_spaceship_gltf_extracted"
$junk = "G:\VaultCache\FabLibrary\SpaceShips_Junk_Yard_ASSET__part2_-81e5d377\fbx\spaceships-junk-yard-ass_extracted"

Copy-Item "$cyber\source\model.fbx" "$dst\cyber_ship\ship.fbx" -Force
Copy-Item "$cyber\textures\T_FloatingCyberShip_JFG_albedo.jpeg" "$dst\cyber_ship\albedo.jpeg" -Force
Copy-Item "$cyber\textures\T_FloatingCyberShip_JFG_emissive.jpeg" "$dst\cyber_ship\emissive.jpeg" -Force

Copy-Item "$star\source\SpaceShip_extracted\SpaceShip.fbx" "$dst\star_ship\ship.fbx" -Force
Copy-Item "$star\textures\Material.001_Base_color.jpg" "$dst\star_ship\albedo.jpg" -Force
Copy-Item "$star\textures\Material.001_Emissive.jpg" "$dst\star_ship\emissive.jpg" -Force

Copy-Item "$low\scene.gltf" "$dst\lowpoly\scene.gltf" -Force
Copy-Item "$low\scene.bin" "$dst\lowpoly\scene.bin" -Force
Copy-Item "$low\textures\freeble_baseColor.png" "$dst\lowpoly\textures\" -Force
Copy-Item "$low\textures\freeble_emissive.jpeg" "$dst\lowpoly\textures\" -Force

Copy-Item "$junk\source\JunkYard2_SetTwo.fbx" "$dst\junkyard\set.fbx" -Force
Copy-Item "$junk\textures\Material__0_Base_Color.jpg" "$dst\junkyard\albedo.jpg" -Force

Write-Host "Staged ships under $dst"
Get-ChildItem $dst -Recurse -File | ForEach-Object {
  "{0,8:N1} KB  {1}" -f ($_.Length/1KB), $_.FullName.Substring($root.Length+1)
}
