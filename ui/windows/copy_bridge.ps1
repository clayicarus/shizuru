# copy_bridge.ps1 — copies shizuru_bridge.dll from the CMake build into the
# Flutter Windows runner output directory so 'flutter run -d windows' can find it.

param(
    [string]$BuildDir   = "$PSScriptRoot\..\..\build",
    [string]$Config     = "Debug",
    [string]$OutputDir  = "$PSScriptRoot\..\build\windows\x64\runner\$Config"
)

$dll = Join-Path $BuildDir "ui\bridge\$Config\shizuru_bridge.dll"

if (-not (Test-Path $dll)) {
    Write-Error "shizuru_bridge.dll not found at '$dll'.`nBuild it first:`n  cmake --build build --config $Config --target shizuru_bridge"
    exit 1
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
Copy-Item -Force $dll $OutputDir
Write-Host "Copied shizuru_bridge.dll -> $OutputDir"
