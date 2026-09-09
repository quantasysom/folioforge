param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'An MSVC C++ build environment is required.' }
$devShell = Join-Path $installation 'Common7\Tools\Launch-VsDevShell.ps1'
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $projectRoot
try {
    cmake --preset dev
    if ($LASTEXITCODE) { throw 'Configure failed.' }
    cmake --build --preset dev
    if ($LASTEXITCODE) { throw 'Build failed.' }
    & (Join-Path $PSScriptRoot 'deploy-local.ps1')
    if (-not $SkipTests) {
        ctest --preset dev
        if ($LASTEXITCODE) { throw 'Tests failed.' }
    }
} finally { Pop-Location }
