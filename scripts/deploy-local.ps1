# Copies Qt, QPDF and PDFium runtime files beside the built desktop app (Windows only).
# Locations come from parameters or QT_ROOT / QPDF_ROOT / PDFIUM_ROOT environment variables.
param(
    [string]$BuildDirectory = 'build/gui',
    [string]$QtRoot = $env:QT_ROOT,
    [string]$QpdfRoot = $env:QPDF_ROOT,
    [string]$PdfiumRoot = $env:PDFIUM_ROOT
)
$ErrorActionPreference = 'Stop'
foreach ($required in @(@('QtRoot', $QtRoot), @('QpdfRoot', $QpdfRoot), @('PdfiumRoot', $PdfiumRoot))) {
    if (-not $required[1]) { throw "Provide -$($required[0]) or set the matching environment variable (QT_ROOT, QPDF_ROOT, PDFIUM_ROOT)." }
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$destination = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
if (-not $destination.StartsWith($projectRoot + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The deployment directory must be inside this workspace.'
}
$app = Join-Path $destination 'pdfeditor-desktop.exe'
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-compiler-runtime --no-system-d3d-compiler $app
if ($LASTEXITCODE) { throw 'Qt runtime deployment failed.' }
# QPDF's runtime DLLs (qpdf, zlib, jpeg) are discovered by pattern; names vary by version and package manager.
foreach ($pattern in @('qpdf*.dll', 'z*.dll', 'jpeg*.dll')) {
    Get-ChildItem -Path "$QpdfRoot\bin" -Filter $pattern -ErrorAction SilentlyContinue | Copy-Item -Destination $destination -Force
}
Copy-Item -LiteralPath "$PdfiumRoot\bin\pdfium.dll" -Destination $destination -Force
Copy-Item -LiteralPath "$QtRoot\plugins\platforms\qoffscreen.dll" -Destination (Join-Path $destination 'platforms') -Force
$notices = Join-Path $destination 'third-party-notices'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
Copy-Item -LiteralPath "$PdfiumRoot\LICENSE" -Destination (Join-Path $notices 'PDFium-LICENSE') -Force
if (Test-Path "$PdfiumRoot\licenses") { Copy-Item -LiteralPath "$PdfiumRoot\licenses" -Destination (Join-Path $notices 'pdfium') -Recurse -Force }
foreach ($package in @('qpdf', 'zlib', 'libjpeg-turbo')) {
    $copyright = Join-Path $QpdfRoot "share\$package\copyright"
    if (Test-Path $copyright) { Copy-Item -LiteralPath $copyright -Destination (Join-Path $notices "$package-copyright") -Force }
}
