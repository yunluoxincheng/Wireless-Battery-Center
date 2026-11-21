# Build both Windows executables and package the same deployment as Setup and Portable.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$QtDir,
    [Parameter(Mandatory)][string]$MingwDir,
    [Parameter(Mandatory)][string]$InnoCompiler,
    [string]$Msys2Root,
    [string]$Version,
    [string]$OutputDir,
    [ValidateRange(1, 32)][int]$Jobs = 4,
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$versionHeader = Get-Content -LiteralPath (Join-Path $root 'res/version.h') -Encoding UTF8 -Raw
if ($versionHeader -notmatch '(?m)^#define WBC_VERSION_STRING "([0-9]+\.[0-9]+\.[0-9]+)"\s*$') {
    throw 'Cannot read the release version from res/version.h.'
}
$sourceVersion = $Matches[1]
if (!$Version) { $Version = $sourceVersion }
if ($Version -ne $sourceVersion) { throw "Release version $Version does not match $sourceVersion in res/version.h." }

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $File) failed with exit code $LASTEXITCODE." }
}

function Save-ReleaseAsset([string]$Url, [string]$File, [string]$Sha256) {
    Invoke-WebRequest -Uri $Url -OutFile $File -TimeoutSec 120 -MaximumRetryCount 2 -RetryIntervalSec 2
    if ($Sha256 -and (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Checksum mismatch for $File. Packaging stopped."
    }
}

$QtDir = (Resolve-Path -LiteralPath $QtDir).Path
$MingwDir = (Resolve-Path -LiteralPath $MingwDir).Path
$InnoCompiler = (Resolve-Path -LiteralPath $InnoCompiler).Path
$gcc = Join-Path $MingwDir 'bin/g++.exe'
$windres = Join-Path $MingwDir 'bin/windres.exe'
$qtPaths = Join-Path $QtDir 'bin/qtpaths.exe'
$deployQt = Join-Path $QtDir 'bin/windeployqt.exe'
if ($Msys2Root) {
    $bash = Join-Path (Resolve-Path -LiteralPath $Msys2Root).Path 'usr/bin/bash.exe'
    $make = 'PATH="$PATH:/usr/bin" /usr/bin/make SHELL=/usr/bin/sh'
} else {
    $gitRoot = Split-Path -Parent (Split-Path -Parent (Get-Command git.exe).Source)
    $bash = Join-Path $gitRoot 'bin/bash.exe'
    if (-not (Test-Path -LiteralPath $bash)) { $bash = (Get-Command bash.exe).Source }
    $make = 'mingw32-make SHELL=sh'
}
$oldPath = $env:PATH
Push-Location $root
try {
    $env:PATH = "$(Join-Path $MingwDir 'bin');$(Join-Path $QtDir 'bin');$oldPath"
    if ((& $gcc -dumpfullversion) -ne '13.1.0' -or (& $gcc -dumpmachine) -ne 'x86_64-w64-mingw32' -or
        (& $qtPaths --qt-version) -ne '6.8.3') {
        throw 'Use the Qt 6.8.3 MinGW x64 SDK and its matching MinGW 13.1.0 toolchain.'
    }
    Write-Host "Building Wireless Battery Center $Version with Qt 6.8.3 and MinGW 13.1.0 x64."
    if (!$OutputDir) { $OutputDir = Join-Path $root "dist/release-v$Version" }
    $OutputDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
    if (Test-Path -LiteralPath $OutputDir) { throw "Output already exists. Choose an unused output directory: $OutputDir" }
    $buildRoot = (New-Item -ItemType Directory -Path (Join-Path $root "build/package-$([guid]::NewGuid().ToString('N'))")).FullName
    $panelBuild = Join-Path $buildRoot 'qt'
    $stage = (New-Item -ItemType Directory -Path (Join-Path $buildRoot 'runtime')).FullName
    $panelExe = 'WirelessBatteryCenterPanel.exe'
    $backgroundExe = 'WirelessBatteryCenter.exe'

    Invoke-Checked $bash @('-c', "$make CXX=g++ WINDRES=windres -j$Jobs")
    Invoke-Checked 'cmake' @('-S', $root, '-B', $panelBuild, '-G', 'Ninja',
        "-DCMAKE_PREFIX_PATH=$($QtDir.Replace('\', '/'))",
        "-DCMAKE_CXX_COMPILER=$($gcc.Replace('\', '/'))", "-DCMAKE_RC_COMPILER=$($windres.Replace('\', '/'))",
        '-DCMAKE_BUILD_TYPE=Release')
    Invoke-Checked 'cmake' @('--build', $panelBuild, '--parallel', "$Jobs", '--target', 'WirelessBatteryCenterPanel')
    if ($RunTests) {
        Invoke-Checked $bash @('-c', "$make CXX=g++ WINDRES=windres test")
        Invoke-Checked 'cmake' @('--build', $panelBuild, '--parallel', "$Jobs", '--target',
            'WirelessBatteryCenterPanel_qmllint', 'DriverLauncherTest')
        Invoke-Checked (Join-Path $panelBuild 'DriverLauncherTest.exe') @()
    }

    foreach ($exe in (Join-Path $root $backgroundExe), (Join-Path $panelBuild $panelExe)) {
        $resource = (Get-Item -LiteralPath $exe).VersionInfo
        if ($resource.ProductVersion -ne $Version -or $resource.ProductName -ne 'Wireless Battery Center') {
            throw "Unexpected version resource in $exe."
        }
        Copy-Item -LiteralPath $exe -Destination $stage
    }
    Invoke-Checked $deployQt @('--release', '--compiler-runtime', '--qmldir', (Join-Path $root 'panel'),
        '--no-system-d3d-compiler', '--no-system-dxc-compiler', '--no-opengl-sw', '--skip-plugin-types', 'qmltooling',
        (Join-Path $stage $panelExe))
    foreach ($file in 'Qt6Core.dll', 'Qt6Quick.dll', 'platforms/qwindows.dll', 'qml/QtQuick/qmldir',
        'qml/QtQuick/Controls/qmldir', 'qml/QtQuick/Dialogs/qmldir', 'libstdc++-6.dll',
        'libgcc_s_seh-1.dll', 'libwinpthread-1.dll') {
        if (!(Test-Path -LiteralPath (Join-Path $stage $file) -PathType Leaf)) { throw "Deployment missing $file." }
    }

    $hscRelease = 'https://github.com/Sapd/HeadsetControl/releases/download/4.3.0'
    Save-ReleaseAsset "$hscRelease/headsetcontrol-windows-x86_64.exe" (Join-Path $stage 'headsetcontrol.exe') 'e371da043710ddb8fc70af77f2ccad1cd03f8c6f49ba423e08764ad7a94a3c40'
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.txt') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $panelBuild 'LUCIDE-LICENSE.txt') -Destination $stage
    $qtLicenses = (New-Item -ItemType Directory -Path (Join-Path $stage 'licenses/Qt')).FullName
    foreach ($license in 'LGPL-3.0-only.txt', 'GPL-3.0-only.txt', 'BSD-3-Clause.txt') {
        Save-ReleaseAsset "https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/$license" (Join-Path $qtLicenses $license)
    }
    Copy-Item -Path (Join-Path $QtDir 'sbom/*.spdx.json') -Destination $qtLicenses
    foreach ($component in 'gcc', 'mingw-w64', 'winpthreads') {
        Copy-Item -LiteralPath (Join-Path $MingwDir "licenses/$component") -Destination (Join-Path $stage 'licenses') -Recurse
    }

    $null = New-Item -ItemType Directory -Path $OutputDir
    $hscSource = Join-Path $OutputDir 'headsetcontrol-4.3.0.tar.gz'
    Save-ReleaseAsset "$hscRelease/headsetcontrol-4.3.0.tar.gz" $hscSource '918dcacf8aef347744b36b2d259d76880fa64b3869f0f8ef26b6576b873d28a7'
    Invoke-Checked $InnoCompiler @("/DAppVersion=$Version", "/DStageDir=$stage", "/O$OutputDir",
        (Join-Path $PSScriptRoot 'WirelessBatteryCenter.iss'))
    $baseName = "WirelessBatteryCenter-$Version-windows-x64"
    $setup = Join-Path $OutputDir "$baseName-setup.exe"
    $portable = Join-Path $OutputDir "$baseName-portable.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $portable)
    $sums = foreach ($file in $setup, $portable, $hscSource) {
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $(Split-Path -Leaf $file)"
    }
    $sums | Set-Content -LiteralPath (Join-Path $OutputDir 'SHA256SUMS.txt') -Encoding ascii
    Write-Host "Windows artifacts: $OutputDir"
} finally {
    Pop-Location
    $env:PATH = $oldPath
}
