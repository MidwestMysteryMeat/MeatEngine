# Bundle the engine + a game project into a standalone shippable build (C7).
#   powershell tools/package.ps1 -Project F:\Games\MyGame -Out dist
# Produces <Out>/<GameName>/ with the exe, engine assets, the project, launchers,
# credits, and a .zip. Runs on a machine without the SDK / Visual Studio.
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Out = "dist",
    [switch]$SkipZip
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$Project = (Resolve-Path $Project).Path
$exe = Join-Path $repo "build\MeatEngine.exe"
if (-not (Test-Path $exe)) { throw "engine not built. Run scripts/build.ps1 first" }
if (-not (Test-Path (Join-Path $Project "game.json"))) { throw "no game.json in '$Project'" }

$game = Get-Content (Join-Path $Project "game.json") -Raw | ConvertFrom-Json
$name = if ($game.name) { $game.name } else { "Game" }
# Sanitize folder name for Windows paths.
$safeName = ($name -replace '[<>:"/\\|?*]', '_').Trim()
if ([string]::IsNullOrWhiteSpace($safeName)) { $safeName = "Game" }

$dest = Join-Path $Out $safeName
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force $dest | Out-Null

# Engine binary + built-in assets (shaders/textures the engine always needs).
Copy-Item $exe $dest
Copy-Item (Join-Path $repo "assets") (Join-Path $dest "assets") -Recurse -Force
# Game project (config + scripts + its own assets). Exclude build artifacts.
$projDest = Join-Path $dest "project"
New-Item -ItemType Directory -Force $projDest | Out-Null
Get-ChildItem $Project -Force | Where-Object {
    $_.Name -notin @('.git', 'build', 'dist', '.vs', 'saves')
} | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $projDest $_.Name) -Recurse -Force
}

# Launchers
@(
    '@echo off',
    'cd /d "%~dp0"',
    'start "" MeatEngine.exe --project project --play %*'
) | Set-Content -Path (Join-Path $dest "Play.bat") -Encoding ASCII
@(
    '@echo off',
    'cd /d "%~dp0"',
    'start "" MeatEngine.exe --project project --host %*'
) | Set-Content -Path (Join-Path $dest "Host.bat") -Encoding ASCII
@(
    '@echo off',
    'cd /d "%~dp0"',
    'MeatEngine.exe --project project --server %*'
) | Set-Content -Path (Join-Path $dest "Server.bat") -Encoding ASCII
@(
    '@echo off',
    'cd /d "%~dp0"',
    'start "" MeatEngine.exe --project project --editor %*'
) | Set-Content -Path (Join-Path $dest "Editor.bat") -Encoding ASCII

# Credits / legal (best-effort).
foreach ($f in @('THIRD_PARTY.md', 'NOTICE', 'LICENSE', 'assets\ATTRIBUTION.md')) {
    $src = Join-Path $repo $f
    if (Test-Path $src) {
        $leaf = Split-Path $f -Leaf
        Copy-Item $src (Join-Path $dest $leaf) -Force
    }
}

$readme = @"
# $name

Packaged with MeatEngine (C7).

## Play
- Double-click **Play.bat** (single-player)
- **Host.bat** — host multiplayer
- **Server.bat** — dedicated server (console)
- **Editor.bat** — Room Designer

## Project
Bundled under ``project/`` (game.json, scripts, assets).

## Credits
See THIRD_PARTY.md / ATTRIBUTION.md if present. Ship authors require CC-BY credit when shipping H4 hulls.
"@
Set-Content -Path (Join-Path $dest "README.txt") -Value $readme -Encoding UTF8

$meta = @{
    name = $name
    packagedUtc = (Get-Date).ToUniversalTime().ToString('o')
    engine = 'MeatEngine'
} | ConvertTo-Json
Set-Content -Path (Join-Path $dest "package.json") -Value $meta -Encoding UTF8

Write-Host "packaged '$name' -> $dest"
if (-not $SkipZip) {
    $zip = "$dest.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path "$dest\*" -DestinationPath $zip
    Write-Host "zipped -> $zip"
}
