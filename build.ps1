# Configures and builds sbr64_pc with llvm-mingw + CMake + Ninja, then copies the executable next to this script.
# The tools are taken from PATH, or from their winget install folders:
#   winget install MartinStorsjo.LLVM-MinGW.UCRT Kitware.CMake Ninja-build.Ninja
# Extra arguments go to "cmake --build" (e.g. --target test_music_player).
$Root = $PSScriptRoot
$Winget = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"

function Add-ToolDir([string]$exe, [string]$pattern) {
    if (Get-Command $exe -ErrorAction SilentlyContinue) { return }
    $found = Get-ChildItem -Path $Winget -Filter $exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like $pattern } | Sort-Object FullName -Descending | Select-Object -First 1
    if ($found) { $env:PATH = $found.DirectoryName + ";" + $env:PATH }
    else { Write-Warning "$exe not found: install it (see the top of build.ps1) or add it to PATH" }
}
Add-ToolDir "clang++.exe" "*llvm-mingw*ucrt-x86_64*\bin\*"
Add-ToolDir "cmake.exe" "*Kitware.CMake*\bin\*"
Add-ToolDir "ninja.exe" "*Ninja-build.Ninja*"

Set-Location $Root
if (Test-Path "$Root\build\CMakeCache.txt") {
    cmake -S . -B build 2>&1 | Select-Object -Last 3
} else {
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ 2>&1 |
        Select-Object -Last 3
}
cmake --build build @args 2>&1
if (Test-Path "$Root\build\sbr64_pc.exe") {
    Copy-Item -Force "$Root\build\sbr64_pc.exe" "$Root\sbr64_pc.exe"
}
