param(
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",

    # Path to cmake --install output (relative to repo root or absolute)
    [string]$InstallDir = "build\install",

    # Pre-downloaded VC Redist path; if empty, the script downloads it
    [string]$VcRedist = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot   = (Get-Item $PSScriptRoot).Parent.Parent.Parent.FullName
$DistDir    = Join-Path $RepoRoot "dist"

# Resolve InstallDir relative to repo root if not absolute
$InstallDirFull = if ([System.IO.Path]::IsPathRooted($InstallDir)) {
    $InstallDir
} else {
    Join-Path $RepoRoot $InstallDir
}

# --- Version -----------------------------------------------------------
$VersionRaw     = (Get-Content (Join-Path $RepoRoot "VERSION")).Trim()
$IsDevBuild     = $VersionRaw -match "-"
$VersionNumeric = $VersionRaw -replace "-.*$", ""

# 4th MSI version field: 0 locally, GITHUB_RUN_NUMBER in CI
$RunNumber = if ($env:GITHUB_RUN_NUMBER) {
    [int][Math]::Min([Math]::Max(0, [int]$env:GITHUB_RUN_NUMBER), 65535)
} else { 0 }
$ProductVersion = if ($IsDevBuild) { "$VersionNumeric.$RunNumber" } else { "$VersionNumeric.0" }

# --- Git SHA (dev builds only) -----------------------------------------
$ShortSha = ""
if ($IsDevBuild) {
    $ShortSha = (& git -C $RepoRoot rev-parse --short HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $ShortSha) { $ShortSha = "local" }
}

# --- Product name and install dir name ---------------------------------
if ($IsDevBuild -and $ShortSha) {
    $ProductName    = "Jellyfin Desktop (dev-$ShortSha)"
    $InstallDirName = "Jellyfin Desktop (dev-$ShortSha)"
} else {
    $ProductName    = "Jellyfin Desktop"
    $InstallDirName = "Jellyfin Desktop"
}

# --- UpgradeCode -------------------------------------------------------
# Fixed GUID shared across all release builds.
$RELEASE_MSI_UPGRADE_CODE    = "{D4E5F6A7-B8C9-4D0E-8F1A-2B3C4D5E6F70}"
$RELEASE_BUNDLE_UPGRADE_CODE = "{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}"

# Namespace GUID for UUID v5 dev-build UpgradeCode derivation.
# This value is fixed for the lifetime of this project — never change it.
$DEV_NAMESPACE_GUID = "6d5e4b3a-2c1f-47e8-9a0b-d3c4e5f60718"

if ($IsDevBuild -and $ShortSha -and $ShortSha -ne "local") {
    $MsiUpgradeCode = & python -c @"
import uuid
ns = uuid.UUID('$DEV_NAMESPACE_GUID')
print('{' + str(uuid.uuid5(ns, 'dev-$ShortSha-msi')).upper() + '}')
"@
    $BundleUpgradeCode = & python -c @"
import uuid
ns = uuid.UUID('$DEV_NAMESPACE_GUID')
print('{' + str(uuid.uuid5(ns, 'dev-$ShortSha-bundle')).upper() + '}')
"@
} else {
    $MsiUpgradeCode    = $RELEASE_MSI_UPGRADE_CODE
    $BundleUpgradeCode = $RELEASE_BUNDLE_UPGRADE_CODE
}

# --- Output filenames --------------------------------------------------
$BuildSuffix = if ($IsDevBuild -and $ShortSha -and $ShortSha -ne "local") {
    "$VersionNumeric-dev-$ShortSha-windows-$Arch"
} else {
    "$VersionNumeric-windows-$Arch"
}
$MsiName = "JellyfinDesktop-$BuildSuffix.msi"
$ExeName = "JellyfinDesktop-$BuildSuffix.exe"
$MsiPath = Join-Path $DistDir $MsiName
$ExePath = Join-Path $DistDir $ExeName

# VC Redist arch string used in registry detection key
$VcRedistArch = if ($Arch -eq "arm64") { "ARM64" } else { "X64" }

Write-Host "--- Installer build parameters ---"
Write-Host "ProductName:        $ProductName"
Write-Host "ProductVersion:     $ProductVersion"
Write-Host "IsDevBuild:         $IsDevBuild"
Write-Host "ShortSha:           $ShortSha"
Write-Host "MsiUpgradeCode:     $MsiUpgradeCode"
Write-Host "BundleUpgradeCode:  $BundleUpgradeCode"
Write-Host "MsiPath:            $MsiPath"
Write-Host "ExePath:            $ExePath"
Write-Host "InstallDirFull:     $InstallDirFull"
Write-Host "VcRedistArch:       $VcRedistArch"

# --- VC Redist --------------------------------------------------------
$VcRedistUrl = "https://aka.ms/vs/17/release/vc_redist.$Arch.exe"

if (-not $VcRedist) {
    Write-Host "Downloading VC Redist 2022 ($Arch)..."
    $VcRedist = Join-Path $env:TEMP "vc_redist_$Arch.exe"
    Invoke-WebRequest -Uri $VcRedistUrl -OutFile $VcRedist
    Write-Host "Downloaded to $VcRedist"
} elseif (-not [System.IO.Path]::IsPathRooted($VcRedist)) {
    $VcRedist = Join-Path $RepoRoot $VcRedist
}

if (-not (Test-Path $VcRedist)) {
    throw "VC Redist not found at $VcRedist"
}

# --- Build MSI --------------------------------------------------------
New-Item -ItemType Directory -Force $DistDir | Out-Null

$InstallerSrcDir = $PSScriptRoot

Write-Host "Building MSI: $MsiName"
& wix build `
  "$InstallerSrcDir\Product.wxs" `
  "$InstallerSrcDir\WixUI_Custom.wxs" `
  -ext WixToolset.UI.wixext `
  -ext WixToolset.Util.wixext `
  -d "ProductName=$ProductName" `
  -d "ProductVersion=$ProductVersion" `
  -d "UpgradeCode=$MsiUpgradeCode" `
  -d "IsDevBuild=$(if ($IsDevBuild) { '1' } else { '0' })" `
  -d "InstallDirName=$InstallDirName" `
  -d "SourceDir=$InstallDirFull" `
  -arch $Arch `
  -o $MsiPath

if ($LASTEXITCODE -ne 0) { throw "MSI build failed" }
Write-Host "MSI: $MsiPath"

# CODE SIGNING
# Uncomment and configure when a code signing certificate is available.
# The MSI must be signed BEFORE building the EXE (Burn embeds it).
#
# & signtool sign `
#     /fd SHA256 `
#     /tr http://timestamp.digicert.com `
#     /td SHA256 `
#     /d "Jellyfin Desktop" `
#     /du "https://jellyfin.org" `
#     /n "Your Certificate Subject Name" `
#     $MsiPath
# if ($LASTEXITCODE -ne 0) { throw "MSI signing failed" }

# --- Build EXE --------------------------------------------------------
Write-Host "Building EXE bundle: $ExeName"
& wix build `
  "$InstallerSrcDir\Bundle.wxs" `
  -ext WixToolset.Bal.wixext `
  -ext WixToolset.Util.wixext `
  -d "ProductName=$ProductName" `
  -d "ProductVersion=$ProductVersion" `
  -d "BundleUpgradeCode=$BundleUpgradeCode" `
  -d "MsiPath=$MsiPath" `
  -d "VcRedistPath=$VcRedist" `
  -d "VcRedistArch=$VcRedistArch" `
  -arch $Arch `
  -o $ExePath

if ($LASTEXITCODE -ne 0) { throw "EXE bundle build failed" }
Write-Host "EXE: $ExePath"

# CODE SIGNING
# Uncomment and configure when a code signing certificate is available.
# Sign the EXE after building, then re-sign the embedded Burn engine stub.
#
# & signtool sign `
#     /fd SHA256 `
#     /tr http://timestamp.digicert.com `
#     /td SHA256 `
#     /d "Jellyfin Desktop" `
#     /du "https://jellyfin.org" `
#     /n "Your Certificate Subject Name" `
#     $ExePath
# if ($LASTEXITCODE -ne 0) { throw "EXE signing failed" }
#
# Re-sign the Burn engine (required to keep Authenticode valid after Burn embeds the MSI):
# $enginePath = Join-Path $env:TEMP "burn_engine.exe"
# & insignia -ib $ExePath -o $enginePath
# & signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 `
#     /n "Your Certificate Subject Name" $enginePath
# & insignia -ab $enginePath $ExePath -o $ExePath

Write-Host ""
Write-Host "Installer build complete." -ForegroundColor Green
Write-Host "  MSI: $MsiPath"
Write-Host "  EXE: $ExePath"
