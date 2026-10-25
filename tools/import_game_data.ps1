# Imports the game files the port needs from your own copy of Run Sackboy! Run! (Android, v1.0.4):
#   APK  -> data\lib\libRSBR_arm32.so (lib/armeabi-v7a/libRSBR.so), data\assets\fonts\*.ttf, data\music\*.mp3
#   OBBs -> data\obb\main.25.com.playstation.runsackboyrun.obb, data\obb\patch.42.com.playstation.runsackboyrun.obb
# These files belong to Sony Interactive Entertainment and are not part of the repository.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\import_game_data.ps1 -Apk <game.apk> -ObbDir <folder>
param(
    [string]$Apk = "C:\sbr64\work\original.apk",
    [string]$ObbDir = "C:\sbr64\work\obb",
    [string]$Data = (Join-Path (Split-Path -Parent $PSScriptRoot) "data"),
    [switch]$Force
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

# SHA-256 of libRSBR.so in v1.0.4, the version the port was built and tested against.
$KnownLibSha256 = "80CCB30E7F0DB7C565BBEF40653C8499786FC026178BACE1664D4216CEC77B09"
$Obbs = @("main.25.com.playstation.runsackboyrun.obb", "patch.42.com.playstation.runsackboyrun.obb")

function Write-Entry($entry, [string]$dst) {
    if (-not $Force -and (Test-Path $dst) -and (Get-Item $dst).Length -eq $entry.Length) {
        "  {0,12:N0}  {1} (already there)" -f $entry.Length, $dst
        return
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dst, $true)
    "  {0,12:N0}  {1}" -f $entry.Length, $dst
}

# Run without arguments on a machine that does not have the defaults: ask (drag and drop into the console works).
if (-not (Test-Path $Apk)) {
    $Apk = (Read-Host "Path of the game's APK file (Run Sackboy! Run! 1.0.4)").Trim('"', ' ')
}
if (-not (Test-Path ([IO.Path]::Combine($ObbDir, "main.25.com.playstation.runsackboyrun.obb")))) {
    $ObbDir = (Read-Host "Folder holding main.25...obb and patch.42...obb").Trim('"', ' ')
}
if (-not (Test-Path $Apk)) { throw "APK not found: $Apk (pass -Apk <path to the game's APK>)" }
"APK: $Apk"
$zip = [IO.Compression.ZipFile]::OpenRead($Apk)
try {
    $lib = $zip.GetEntry("lib/armeabi-v7a/libRSBR.so")
    if (-not $lib) { throw "lib/armeabi-v7a/libRSBR.so is missing from the APK: is this Run Sackboy! Run!?" }
    Write-Entry $lib (Join-Path $Data "lib\libRSBR_arm32.so")
    foreach ($e in $zip.Entries) {
        if ($e.FullName -like "assets/fonts/*.ttf") { Write-Entry $e (Join-Path $Data "assets\fonts\$($e.Name)") }
        elseif ($e.FullName -like "res/raw/*.mp3") { Write-Entry $e (Join-Path $Data "music\$($e.Name)") }
    }
} finally { $zip.Dispose() }

"OBB folder: $ObbDir"
New-Item -ItemType Directory -Force (Join-Path $Data "obb") | Out-Null
foreach ($name in $Obbs) {
    $src = Join-Path $ObbDir $name
    $dst = Join-Path $Data "obb\$name"
    if (-not (Test-Path $src)) { throw "$name not found in $ObbDir (pass -ObbDir <folder holding the two OBB files>)" }
    if (-not $Force -and (Test-Path $dst) -and (Get-Item $dst).Length -eq (Get-Item $src).Length) {
        "  {0,12:N0}  {1} (already there)" -f (Get-Item $src).Length, $dst
    } else {
        Copy-Item -Force $src $dst
        "  {0,12:N0}  {1}" -f (Get-Item $dst).Length, $dst
    }
}

$hash = (Get-FileHash (Join-Path $Data "lib\libRSBR_arm32.so") -Algorithm SHA256).Hash
if ($hash -eq $KnownLibSha256) {
    "libRSBR.so is v1.0.4 (SHA-256 matches): ready."
} else {
    Write-Warning "libRSBR.so SHA-256 is $hash, not the v1.0.4 build the port targets: it may not run."
}
