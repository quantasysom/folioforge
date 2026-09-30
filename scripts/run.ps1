param([string]$Pdf)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$app = Join-Path $projectRoot 'build\gui\pdfeditor-desktop.exe'
if (-not (Test-Path -LiteralPath $app)) { throw 'Run scripts/build.ps1 first.' }
# build.ps1 deploys the runtime beside the executable; QT_ROOT/QPDF_ROOT/PDFIUM_ROOT only matter for undeployed builds.
$extra = @($env:QT_ROOT, $env:QPDF_ROOT, $env:PDFIUM_ROOT) | Where-Object { $_ } | ForEach-Object { Join-Path $_ 'bin' }
if ($extra) { $env:PATH = ($extra -join ';') + ';' + $env:PATH }
if ($Pdf) { & $app $Pdf } else { & $app }
