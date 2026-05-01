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
$RunNumber = if ($env:GITHUB_RUN_NUMBER) { $env:GITHUB_RUN_NUMBER } else { "0" }
$ProductVersion = if ($IsDevBuild) { "$VersionNumeric.$RunNumber" } else { $VersionNumeric }

# --- Git SHA (dev builds only) -----------------------------------------
$ShortSha = ""
if ($IsDevBuild) {
    $ShortSha = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
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
