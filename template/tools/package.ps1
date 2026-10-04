# Builds Release and lays out the mod for the Workshop in dist\<name>\ (mod\ + native\<name>.dll), plus the DLL's
# SHA-256 for your release notes in dist\<name>.sha256.txt. To publish, copy dist\<name> to
# %LOCALAPPDATA%\KillHouseGames\DoorKickers2\mods_upload\<name> and upload it from the game's Mods menu. install.ps1
# copies to the same folder but keeps files from earlier installs, so remove anything stale there first. Players also
# need the Native Mod Loader, so link it on your Workshop page.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent # the project root, one up from tools\
$name = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\((\w+)').Groups[1].Value

& (Join-Path $PSScriptRoot 'build.ps1')
$dist = Join-Path $root 'dist'
$mod = Join-Path $dist $name
if (Test-Path $mod) { Remove-Item -LiteralPath $mod -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $mod 'native') | Out-Null
Copy-Item (Join-Path $root 'mod\*') $mod -Recurse
$dll = Join-Path $mod "native\$name.dll"
Copy-Item (Join-Path $root "build\$name.dll") $dll

$hash = '{0}  {1}' -f (Get-FileHash $dll -Algorithm SHA256).Hash.ToLower(), (Split-Path $dll -Leaf)
Set-Content (Join-Path $dist "$name.sha256.txt") $hash
Write-Host "Mod folder: $mod"
Write-Host "  $hash"
