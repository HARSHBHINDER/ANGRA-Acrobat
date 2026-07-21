# Builds dist/windows/ANGRA-Acrobat-Portable-x64.zip from a completed build.
param([string]$BuildDir = "build")
$ErrorActionPreference = "Stop"
$dist = "dist\windows"
New-Item -ItemType Directory -Force $dist | Out-Null
$stage = Join-Path $env:TEMP "angra-portable"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$BuildDir\ANGRA.exe" $stage
if (Test-Path "$BuildDir\pdfium.dll") { Copy-Item "$BuildDir\pdfium.dll" $stage }
if (Test-Path "$BuildDir\deploy") { Copy-Item "$BuildDir\deploy\*" $stage -Recurse -Force }
Copy-Item LICENSE, THIRD_PARTY_NOTICES.md $stage
Compress-Archive -Path "$stage\*" -DestinationPath "$dist\ANGRA-Acrobat-Portable-x64.zip" -Force
Write-Host "Wrote $dist\ANGRA-Acrobat-Portable-x64.zip"
