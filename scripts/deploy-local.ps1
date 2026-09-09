param([string]$BuildDirectory = 'build/gui')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$destination = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
if (-not $destination.StartsWith($projectRoot + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The deployment directory must be inside this workspace.'
}
$qtRoot = 'C:\Qt\6.11.1\msvc2022_64'
$pdfiumRoot = 'C:\projects\vPDF\third_party\pdfium'
$vcpkgRoot = 'C:\projects\vPDF\vcpkg_installed\x64-windows'
$app = Join-Path $destination 'pdfeditor-desktop.exe'
& "$qtRoot\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-compiler-runtime --no-system-d3d-compiler $app
if ($LASTEXITCODE) { throw 'Qt runtime deployment failed.' }
foreach ($dll in @('qpdf30.dll', 'z.dll', 'jpeg62.dll')) {
    Copy-Item -LiteralPath (Join-Path "$vcpkgRoot\bin" $dll) -Destination $destination -Force
}
Copy-Item -LiteralPath "$pdfiumRoot\bin\pdfium.dll" -Destination $destination -Force
Copy-Item -LiteralPath "$qtRoot\plugins\platforms\qoffscreen.dll" -Destination (Join-Path $destination 'platforms') -Force
$notices = Join-Path $destination 'third-party-notices'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
Copy-Item -LiteralPath "$pdfiumRoot\LICENSE" -Destination (Join-Path $notices 'PDFium-LICENSE') -Force
Copy-Item -LiteralPath "$pdfiumRoot\licenses" -Destination (Join-Path $notices 'pdfium') -Recurse -Force
foreach ($package in @('qpdf', 'zlib', 'libjpeg-turbo')) {
    Copy-Item -LiteralPath (Join-Path $vcpkgRoot "share\$package\copyright") -Destination (Join-Path $notices "$package-copyright") -Force
}
