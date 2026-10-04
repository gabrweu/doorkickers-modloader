# Builds the loader (dk2ml.dll + the dbghelp.dll stub) and test tools (MSVC x64) into build\.
# -Config: CMake build type (default Release).
param([string]$Config = 'Release')

$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 build tools not found' }

$src = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $src 'build'
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# vcvars calls vswhere by name
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"

cmd /c "`"$vcvars`" >nul && cmake -S `"$src`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$build`""
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Join-Path $build 'dk2ml.dll'
Join-Path $build 'dbghelp.dll'
