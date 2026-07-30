# Configure + build MeatEngine (Ninja + MSVC from VS 2022, Release by default).
param(
    [string]$Config = "Release",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
if (-not (Test-Path $vs)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -property installationPath }
}
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64' | Out-Null

python -m pip install --user --quiet jinja2   # glad generator dependency

if ($Clean -and (Test-Path "$repo\build")) { Remove-Item -Recurse -Force "$repo\build" }
cmake -S $repo -B "$repo\build" -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build "$repo\build" --parallel
exit $LASTEXITCODE
