# Bundle the engine + a game project into a standalone shippable build.
#   powershell tools/package.ps1 -Project F:\Games\MyGame -Out dist
# Produces <Out>/<GameName>/ with the exe, engine assets, the project, and a
# launcher .bat, plus a .zip. The result runs on a machine without the SDK.
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Out = "dist"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo "build\MeatEngine.exe"
if (-not (Test-Path $exe)) { throw "engine not built. Run scripts/build.ps1 first" }
if (-not (Test-Path (Join-Path $Project "game.json"))) { throw "no game.json in '$Project'" }

$game = Get-Content (Join-Path $Project "game.json") -Raw | ConvertFrom-Json
$name = if ($game.name) { $game.name } else { "Game" }
$dest = Join-Path $Out $name
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force $dest | Out-Null

# Engine binary + built-in assets (shaders/textures the engine always needs).
Copy-Item $exe $dest
Copy-Item (Join-Path $repo "assets") (Join-Path $dest "assets") -Recurse
# The game project (config + scripts + its own assets).
Copy-Item $Project (Join-Path $dest "project") -Recurse

# Launcher: play the bundled project by default.
$bat = @('@echo off', 'MeatEngine.exe --project project --play %*')
Set-Content -Path (Join-Path $dest "Play.bat") -Value $bat -Encoding ASCII

Write-Host "packaged '$name' -> $dest"
$zip = "$dest.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$dest\*" -DestinationPath $zip
Write-Host "zipped -> $zip"
