# dhc-vpn -- build the MSI installer from the staging tree produced by
# scripts/package.ps1.
#
# Prerequisites:
#   - .NET 8 SDK (pre-installed on the GitHub windows-2022 runners; locally,
#     install via `winget install Microsoft.DotNet.SDK.8`).
#   - The WiX dotnet tool (`dotnet tool install --global wix`). Auto-installed
#     by this script if missing.
#   - A staged build at build/dhc-vpn-<version>/ -- run scripts/package.ps1
#     first.
#
# Usage:
#   PS> .\scripts\build-msi.ps1
#   PS> .\scripts\build-msi.ps1 -Version 0.1.0-alpha2
#   PS> .\scripts\build-msi.ps1 -StagingDir D:\path\to\dhc-vpn-0.1.0
#
# Output:
#   build\dhc-vpn-<version>.msi
#   build\dhc-vpn-<version>.msi.sha256

[CmdletBinding()]
param(
    [string]$Version,
    [string]$StagingDir,
    [string]$OutputDir,
    [string]$WixVersion = '5.0.2'
)

$ErrorActionPreference = 'Stop'

$here     = Split-Path -Parent $PSCommandPath
$repoRoot = Split-Path -Parent $here
$assets   = Join-Path $repoRoot 'installer'

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot 'build'
}

# ---- locate the staging dir ----------------------------------------------

if (-not $StagingDir) {
    if ($Version) {
        $StagingDir = Join-Path $OutputDir "dhc-vpn-$Version"
    } else {
        # Newest dhc-vpn-<...> directory under build\
        $candidate = Get-ChildItem -Path $OutputDir -Directory -Filter 'dhc-vpn-*' -EA SilentlyContinue |
                     Where-Object { -not $_.Name.EndsWith('.zip') } |
                     Sort-Object LastWriteTime -Descending |
                     Select-Object -First 1
        if (-not $candidate) {
            throw "no staging directory found under $OutputDir. Run scripts/package.ps1 first."
        }
        $StagingDir = $candidate.FullName
        $Version    = $candidate.Name -replace '^dhc-vpn-',''
    }
}

if (-not (Test-Path $StagingDir)) {
    throw "staging directory not found: $StagingDir"
}
if (-not $Version) {
    $Version = (Split-Path -Leaf $StagingDir) -replace '^dhc-vpn-',''
}

# ---- derive a 4-part numeric MSI ProductVersion --------------------------

# MSI requires dot-separated numeric versions only (no -alpha / -gSHA tags).
# Strategy: take the leading numeric prefix from $Version, pad to 4 parts
# with zeros. e.g.  "0.1.0-g1ca526d"  ->  "0.1.0.0"
#                   "0.1.0-alpha2"    ->  "0.1.0.0"
#                   "1.2.3.4"         ->  "1.2.3.4"
$numericPrefix = if ($Version -match '^(\d+(?:\.\d+){0,3})') { $matches[1] } else { '0.0.0' }
$parts = $numericPrefix.Split('.')
while ($parts.Count -lt 4) { $parts += '0' }
$msiVersion = ($parts[0..3]) -join '.'

Write-Host "Build MSI"
Write-Host "  staging   : $StagingDir"
Write-Host "  version   : $Version"
Write-Host "  msi vers. : $msiVersion"
Write-Host "  output    : $OutputDir"
Write-Host "  WiX       : $WixVersion"
Write-Host ''

# ---- install WiX dotnet tool if missing ---------------------------------

function Resolve-WixCommand {
    $cmd = Get-Command wix -EA SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $local = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
    if (Test-Path $local) { return $local }
    return $null
}

$wix = Resolve-WixCommand
if (-not $wix) {
    Write-Host "Installing WiX dotnet tool ($WixVersion)..."
    & dotnet tool install --global wix --version $WixVersion
    if ($LASTEXITCODE -ne 0) {
        & dotnet tool update --global wix --version $WixVersion
        if ($LASTEXITCODE -ne 0) { throw 'dotnet tool install/update wix failed.' }
    }
    $env:PATH = "$env:USERPROFILE\.dotnet\tools;$env:PATH"
    $wix = Resolve-WixCommand
}
if (-not $wix) { throw 'wix.exe not found after install attempt.' }

Write-Host "WiX: $wix"
& $wix --version

# ---- ensure required WiX extensions -------------------------------------

foreach ($ext in 'WixToolset.Firewall.wixext','WixToolset.Util.wixext') {
    # Pin the extension to the same version as WiX itself; without the
    # version suffix, `wix extension add` resolves the latest (e.g. 7.x)
    # which is incompatible with a WiX-5 host (warning WIX6101).
    Write-Host "Adding WiX extension: $ext/$WixVersion"
    & $wix extension add -g "$ext/$WixVersion"
    if ($LASTEXITCODE -ne 0) { throw "wix extension add $ext/$WixVersion failed." }
}

# ---- override the upstream strongswan.conf in the staging tree ----------

# The upstream-build template lacks a Windows-friendly filelog block; our
# installer/strongswan.conf has a hard-coded log path under %ProgramData%.
# We overwrite the staging copy so the bulk Files harvest in Product.wxs
# picks up our version.
$stagedSwan = Join-Path $StagingDir 'etc\strongswan.conf'
$ourSwan    = Join-Path $assets    'strongswan.conf'
Write-Host "Override staged strongswan.conf with installer/strongswan.conf"
Copy-Item -Force $ourSwan $stagedSwan

# ---- prepare cleaned BulkDir for WiX <Files> harvest --------------------

# Product.wxs declares charon-svc.exe and dhc-vpn.exe as explicit
# Components (with ServiceInstall / Shortcut respectively). The bulk
# <Files> include cannot exclude them in WiX v5, so we mirror StagingDir
# into a sibling dir minus those two files.
$bulkDir = Join-Path $OutputDir "msi-bulk-$Version"
if (Test-Path $bulkDir) { Remove-Item -Recurse -Force $bulkDir }
Write-Host "Prepare cleaned bulk dir: $bulkDir"
Copy-Item -Recurse -Force $StagingDir $bulkDir
Remove-Item -Force (Join-Path $bulkDir 'bin\charon-svc.exe')
Remove-Item -Force (Join-Path $bulkDir 'gui\dhc-vpn.exe')

# ---- build the MSI -------------------------------------------------------

$wxs    = Join-Path $assets 'Product.wxs'
$msiOut = Join-Path $OutputDir "dhc-vpn-$Version.msi"

if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null }
if (Test-Path $msiOut)           { Remove-Item -Force $msiOut }

& $wix build $wxs `
    -d "ProductVersion=$msiVersion" `
    -d "StagingDir=$StagingDir" `
    -d "BulkDir=$bulkDir" `
    -d "InstallerAssetsDir=$assets" `
    -ext WixToolset.Firewall.wixext `
    -ext WixToolset.Util.wixext `
    -arch x64 `
    -o $msiOut

if ($LASTEXITCODE -ne 0) { throw "wix build failed (exit $LASTEXITCODE)." }
if (-not (Test-Path $msiOut)) { throw "wix exited 0 but $msiOut not produced." }

# ---- SHA256 sidecar ------------------------------------------------------

$hash = (Get-FileHash -Path $msiOut -Algorithm SHA256).Hash
"$hash *$(Split-Path -Leaf $msiOut)" | Set-Content -Path "$msiOut.sha256" -Encoding ASCII

Write-Host ''
Write-Host 'Done.'
Write-Host "  MSI    : $msiOut"
Write-Host "  SHA256 : $hash"
