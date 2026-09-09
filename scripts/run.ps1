param([string]$Pdf)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = "C:\Qt\6.11.1\msvc2022_64\bin;C:\projects\vPDF\vcpkg_installed\x64-windows\bin;C:\projects\vPDF\third_party\pdfium\bin;$env:PATH"
$app = Join-Path $projectRoot 'build\gui\pdfeditor-desktop.exe'
if (-not (Test-Path -LiteralPath $app)) { throw 'Run scripts/build.ps1 first.' }
if ($Pdf) { & $app $Pdf } else { & $app }
