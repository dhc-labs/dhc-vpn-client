# dhc-vpn -- package the build into a distributable ZIP.
#
# Layout produced (matching what install.ps1 expects):
#
#   dhc-vpn-<ver>\
#     bin\        charon-svc.exe + DLLs (from build\charon-install\bin)
#     sbin\       swanctl.exe + DLLs (from build\charon-install\sbin)
#     etc\        strongswan.conf + strongswan.d\* (template)
#     gui\        dhc-vpn.exe + Qt6 + plugins (from build\src\gui)
#     scripts\    install.ps1 + uninstall.ps1
#     README.txt  short ops doc
#
# Output:  build\dhc-vpn-<ver>.zip
#
# Usage:
#   PS> .\scripts\package.ps1
#   PS> .\scripts\package.ps1 -Version 0.1.0-alpha2
#   PS> .\scripts\package.ps1 -OutputDir D:\releases

[CmdletBinding()]
param(
    [string]$Version,                   # default: short git SHA
    [string]$RepoRoot,                  # default: parent of script dir
    [string]$OutputDir                  # default: <repo>\build
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $here = Split-Path -Parent $PSCommandPath
    $RepoRoot = Split-Path -Parent $here
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $RepoRoot 'build'
}
if (-not $Version) {
    Push-Location $RepoRoot
    try {
        $sha = & git rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $sha) {
            $Version = "0.1.0-g$sha"
        } else {
            $Version = '0.1.0-dev'
        }
    } finally {
        Pop-Location
    }
}

$pkgName = "dhc-vpn-$Version"
$staging = Join-Path $OutputDir $pkgName
$zipPath = Join-Path $OutputDir "$pkgName.zip"

Write-Host "Package    : $pkgName"
Write-Host "Repo root  : $RepoRoot"
Write-Host "Staging    : $staging"
Write-Host "ZIP        : $zipPath"
Write-Host ''

# ---- precondition checks -----------------------------------------------

$srcBin = Join-Path $RepoRoot 'build\charon-install\bin'
$srcSbin= Join-Path $RepoRoot 'build\charon-install\sbin'
$srcEtc = Join-Path $RepoRoot 'build\charon-install\etc'
$srcGui = Join-Path $RepoRoot 'build\src\gui'

foreach ($p in $srcBin, $srcSbin, $srcEtc, $srcGui) {
    if (-not (Test-Path $p)) {
        throw "Required build artifact missing: $p`nRun the build first (build-charon.sh + cmake --build build)."
    }
}

if (-not (Test-Path (Join-Path $srcBin  'charon-svc.exe'))) { throw 'charon-svc.exe missing -- build incomplete.' }
if (-not (Test-Path (Join-Path $srcSbin 'swanctl.exe')))    { throw 'swanctl.exe missing -- build incomplete.' }
if (-not (Test-Path (Join-Path $srcGui  'dhc-vpn.exe')))   { throw 'dhc-vpn.exe missing -- build the GUI first.' }
if (-not (Test-Path (Join-Path $srcBin  'wintun.dll')))     { throw 'wintun.dll missing in bin/ -- check build-charon.sh.' }
if (-not (Test-Path (Join-Path $srcBin  'WinDivert64.sys'))){ throw 'WinDivert64.sys missing in bin/ -- check build-charon.sh.' }

# ---- staging -----------------------------------------------------------

if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

Write-Host 'Staging bin\ ...'
Copy-Item -Recurse -Force $srcBin  (Join-Path $staging 'bin')

Write-Host 'Staging sbin\ ...'
Copy-Item -Recurse -Force $srcSbin (Join-Path $staging 'sbin')

Write-Host 'Staging etc\ ...'
Copy-Item -Recurse -Force $srcEtc  (Join-Path $staging 'etc')
# The build tree's etc\swanctl\* is dev-only (drhc-cert + the user's
# certs). Strip that out -- a fresh install should start with no
# pre-loaded profiles.
$stagedSwanctl = Join-Path $staging 'etc\swanctl'
if (Test-Path $stagedSwanctl) {
    Remove-Item -Recurse -Force $stagedSwanctl
}

Write-Host 'Staging gui\ ...'
$stagedGui = Join-Path $staging 'gui'
New-Item -ItemType Directory -Force -Path $stagedGui | Out-Null
# Copy only what windeployqt + deploy-mingw-deps.sh produced. Skip the
# CMake leftovers (CMakeFiles\, Makefile, *.o etc).
Get-ChildItem -Path $srcGui -Recurse -File | Where-Object {
    $_.FullName -notmatch '\\CMakeFiles\\' -and
    $_.FullName -notmatch '\\.*_autogen\\' -and
    $_.Name -notin @('Makefile','cmake_install.cmake','build.ninja') -and
    $_.Extension -notin @('.o','.obj','.cmake')
} | ForEach-Object {
    $rel = $_.FullName.Substring($srcGui.Length).TrimStart('\','/')
    $dst = Join-Path $stagedGui $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -Force $_.FullName $dst
}

Write-Host 'Staging scripts\ ...'
$stagedScripts = Join-Path $staging 'scripts'
New-Item -ItemType Directory -Force -Path $stagedScripts | Out-Null
Copy-Item -Force (Join-Path $RepoRoot 'scripts\install.ps1')   $stagedScripts
Copy-Item -Force (Join-Path $RepoRoot 'scripts\uninstall.ps1') $stagedScripts

# ---- README -----------------------------------------------------------

$readme = @"
dhc-vpn  $Version
==================

Files in this package
---------------------

  bin\                 charon-svc.exe + dependencies (Wintun, WinDivert)
  sbin\                swanctl.exe (control plane)
  etc\                 strongswan.conf + plugin configs (templates)
  gui\                 dhc-vpn.exe + Qt6 runtime
  scripts\install.ps1  installer  (run as Administrator)
  scripts\uninstall.ps1 uninstaller (run as Administrator)


Quick start
-----------

  1. Right-click on PowerShell -> Run as Administrator.
  2. cd <unzipped folder>
  3. .\scripts\install.ps1
  4. sc.exe start charon-svc
  5. Launch "dhc-vpn" from the Start Menu.
  6. Use "New Profile..." to create a connection (cert or username/password).
  7. Connect.

Locations after install:

  Program files     : %ProgramFiles%\dhc-vpn\
  Profiles, certs   : %ProgramData%\dhc-vpn\swanctl\
  Logs              : %ProgramData%\dhc-vpn\logs\charon.log


Auth modes
----------

  Certificate (pubkey)
    Pick client cert + key + (optionally) CA cert in the New-Profile
    dialog. Files are copied into the swanctl directory automatically.

  Username + Password (EAP-MSCHAPv2)
    Enter username and password. The password is encrypted with DPAPI
    (current-user scope) and stored under
    %ProgramData%\dhc-vpn\swanctl\secrets.d\<profile>.dat. It is
    materialized to a temporary file in conf.d\ only for the duration
    of one swanctl --load-all (typically <100 ms) and then wiped.


Removing
--------

  .\scripts\uninstall.ps1                 (keeps profiles + saved passwords)
  .\scripts\uninstall.ps1 -PurgeData      (removes everything)


Notes
-----

  - Wintun and WinDivert drivers install themselves on first use --
    no separate driver-installer is needed.
  - The Windows service runs as LocalSystem (required for the Wintun
    adapter and routing-table edits).
  - The GUI does NOT auto-start charon-svc. Start the service first
    (sc.exe start charon-svc) or set it to auto-start.
"@

$readme | Set-Content -Path (Join-Path $staging 'README.txt') -Encoding ASCII

# ---- zip --------------------------------------------------------------

if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Write-Host "Compressing -> $zipPath ..."
Compress-Archive -Path $staging -DestinationPath $zipPath -CompressionLevel Optimal

# Also compute a sha256 next to the zip so testers can verify.
$hash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash
"$hash *$pkgName.zip" | Set-Content -Path "$zipPath.sha256" -Encoding ASCII

Write-Host ''
Write-Host 'Done.'
Write-Host "  ZIP   : $zipPath"
Write-Host "  SHA256: $hash"
