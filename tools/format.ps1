# Formats the C/C++ sources with the repo's .clang-format. -Check only lists the files that need formatting and exits
# with 1 if there are any. Generated files, vendored code, asm and resource scripts are left alone.
param([switch]$Check)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$exe = (Get-Command clang-format.exe -ErrorAction SilentlyContinue).Source
if (-not $exe) {
    $exe = @(
        'C:\Program Files\LLVM\bin\clang-format.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe'
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $exe) { throw 'clang-format.exe not found: install LLVM or the VS "C++ Clang tools" component' }

$files = @(
    Get-ChildItem (Join-Path $root 'loader') -Recurse -Include *.cpp, *.h
    Get-Item (Join-Path $root 'include\dk2ml.h'), (Join-Path $root 'include\dk2ml.hpp')
    Get-Item (Join-Path $root 'tools\SymTest.cpp')
    Get-ChildItem (Join-Path $root 'tests') -Recurse -Include *.cpp
    Get-ChildItem (Join-Path $root 'template\src') -Include *.cpp -Recurse
    Get-Item (Join-Path $root 'docs\ExamplePlugin.cpp')
) | Select-Object -ExpandProperty FullName

if ($Check) {
    $unformatted = $files | Where-Object { & $exe --dry-run --Werror $_ 2>$null; $LASTEXITCODE -ne 0 }
    if ($unformatted) {
        $unformatted | ForEach-Object { Write-Host "needs formatting: $_" }
        exit 1
    }
    Write-Host "all $($files.Count) files formatted"
    return
}

& $exe -i @files
Write-Host "formatted $($files.Count) files with $exe"
