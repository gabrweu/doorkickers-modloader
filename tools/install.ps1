# Developer install: copies the freshly built loader (build\dk2ml.dll and the build\dbghelp.dll stub that loads it) into
# the game folder. Plugins install from their own repos.
# Players copy both DLLs from the release zip instead.
# Uninstall: .\tools\install.ps1 -Uninstall   (or delete dbghelp.dll and dk2ml.dll from the game folder)
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\DoorKickers2',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$files = 'dbghelp.dll', 'dk2ml.dll'

if (Get-Process DoorKickers2 -ErrorAction SilentlyContinue) { throw 'Close Door Kickers 2 first (its DLLs are locked).' }
if ($Uninstall) {
    Remove-Item ($files | ForEach-Object { Join-Path $GameDir $_ }) -ErrorAction SilentlyContinue
    Write-Host 'Loader removed; the game will start vanilla.'
    return
}
$built = $files | ForEach-Object { Join-Path $PSScriptRoot "..\build\$_" }
if ($built | Where-Object { -not (Test-Path $_) }) { throw 'Build first: .\tools\build.ps1' }

Copy-Item $built $GameDir -Force
Write-Host "Installed the loader into $GameDir. It logs to $GameDir\dk2ml.log."
