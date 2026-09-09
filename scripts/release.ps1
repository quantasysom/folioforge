<#
.SYNOPSIS
Build, test and package FolioForge on the current OS. See docs/packaging.md.
.EXAMPLE
./scripts/release.ps1 -QtRoot C:/Qt/6.11.1/msvc2022_64 -QpdfRoot C:/deps/qpdf -PdfiumRoot C:/deps/pdfium -Formats ZIP,MSI -WixVersion 4
#>
[CmdletBinding()]
param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$QpdfRoot = $env:QPDF_ROOT,
    [string]$PdfiumRoot = $env:PDFIUM_ROOT,
    [string[]]$Formats,
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$Generator = 'Ninja',
    [string]$Toolchain,
    [string]$NoticesDirectory,
    [ValidateSet('3', '4')][string]$WixVersion = '3',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -eq 'Windows_NT' -and -not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ build tools or use an MSVC developer shell.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'No MSVC C++ toolchain found.' }
    & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}
$arguments = @(
    "-DQT_ROOT=$QtRoot", "-DQPDF_ROOT=$QpdfRoot", "-DPDFIUM_ROOT=$PdfiumRoot",
    "-DFORMATS=$($Formats -join ',')", "-DBUILD_DIR=$BuildDirectory", "-DOUTPUT_DIR=$OutputDirectory",
    "-DGENERATOR=$Generator", "-DTOOLCHAIN=$Toolchain", "-DNOTICES_DIR=$NoticesDirectory",
    "-DWIX_VERSION=$WixVersion", "-DSKIP_TESTS=$($SkipTests.IsPresent)",
    '-P', (Join-Path $PSScriptRoot 'release.cmake')
)
& cmake @arguments
if ($LASTEXITCODE -ne 0) { throw "Release build failed (exit $LASTEXITCODE)." }
