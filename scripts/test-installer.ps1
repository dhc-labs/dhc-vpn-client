# dhc-vpn -- installer end-to-end smoke test.
#
# Extracts the latest packaged ZIP into an alternate path, runs install.ps1
# with a non-default ServiceName/InstallDir/DataDir (so it does not clobber
# any live install), inspects the result, then runs uninstall.ps1 -PurgeData
# and verifies everything is gone.
#
# Run from ELEVATED PowerShell.
#
# Usage:    .\scripts\test-installer.ps1
# Output:   .\test-installer-YYYYMMDD-HHMMSS.log

$ErrorActionPreference = 'Continue'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$out  = Join-Path $repo "test-installer-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"

function Log     { param([string]$x) Add-Content -LiteralPath $out -Value $x; Write-Host $x }
function Section { param([string]$n) Log ''; Log ('=' * 60); Log $n; Log ('=' * 60) }
function LogObj  { param($o) ($o | Out-String -Width 200).TrimEnd() | ForEach-Object { Log $_ } }

# Admin gate
$id    = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdm = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdm) {
    Write-Host "ERROR: must run elevated." -ForegroundColor Red
    return
}

# Paths kept far away from the live dev install
$svcName    = 'charon-svc-test'
$installDir = 'C:\dhc-vpn-testinstall'
$dataDir    = 'C:\dhc-vpn-testdata'
$extractDir = 'C:\dhc-vpn-testextract'

Set-Content -LiteralPath $out -Value "test-installer started $(Get-Date)"
Log "service:   $svcName"
Log "install:   $installDir"
Log "data:      $dataDir"
Log "extract:   $extractDir"

# Pick the newest ZIP under build\
$zip = Get-ChildItem (Join-Path $repo 'build\dhc-vpn-*.zip') -EA SilentlyContinue |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1
if (-not $zip) {
    Log 'FATAL: no dhc-vpn-*.zip found under build\. Run scripts/package.ps1 first.'
    return
}
Log "zip:       $($zip.FullName)"

Section 'STEP 1: extract ZIP'
if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }
Expand-Archive -LiteralPath $zip.FullName -DestinationPath $extractDir -Force
$pkgRoot = (Get-ChildItem $extractDir -Directory)[0].FullName
Log "pkgRoot:   $pkgRoot"

Section 'STEP 2: run install.ps1 (alt paths, -NoStartMenu)'
& "$pkgRoot\scripts\install.ps1" `
    -InstallDir $installDir `
    -DataDir    $dataDir `
    -ServiceName $svcName `
    -NoStartMenu 2>&1 | LogObj

Section 'STEP 3: verify service registered'
$svc = Get-Service -Name $svcName -EA SilentlyContinue
if (-not $svc) {
    Log 'FAIL: service not registered'
} else {
    LogObj ($svc | Format-Table Name, Status, StartType -AutoSize)
}

Section 'STEP 4: verify ACL grants AU start/stop'
$sdOut = & sc.exe sdshow $svcName 2>&1
LogObj $sdOut
$sdJoin = ($sdOut | Out-String).Trim()
if ($sdJoin -match '\(A;;[A-Z]+;;;AU\)') {
    Log 'PASS: AU ACE present in DACL'
} else {
    Log 'FAIL: no AU ACE in DACL -- sc.exe sdset did not run or was reverted'
}

Section 'STEP 5: verify firewall rules'
foreach ($rule in 'dhc-vpn IKE UDP/500','dhc-vpn IKE UDP/4500','dhc-vpn allow inbound on wintun') {
    $r = Get-NetFirewallRule -DisplayName $rule -EA SilentlyContinue
    if ($r) { Log "PASS: rule '$rule'" }
    else    { Log "FAIL: rule '$rule' missing" }
}

Section 'STEP 6: verify file layout'
foreach ($p in "$installDir\bin\charon-svc.exe","$installDir\sbin\swanctl.exe","$installDir\gui\dhc-vpn.exe","$installDir\etc\strongswan.conf","$dataDir\swanctl\conf.d","$dataDir\logs") {
    $tag = if (Test-Path $p) { 'PASS' } else { 'FAIL' }
    Log "$tag $p"
}

Section 'STEP 7: verify service env (STRONGSWAN_CONF + SWANCTL_DIR)'
$env_ = Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\$svcName" -Name Environment -EA SilentlyContinue
if ($env_) {
    LogObj $env_.Environment
} else {
    Log 'FAIL: no Environment key'
}

Section 'STEP 8: try AU start (low-token via runas)'
# Quick sanity: attempt sc.exe start as the current admin token (always
# succeeds), THEN as a low-integrity-ish way: just check that the SDDL
# permits AU directly. Real cross-user test would need a separate user.
# So this is just a "does the service start at all" probe.
& sc.exe start $svcName 2>&1 | LogObj
Start-Sleep -Seconds 2
LogObj (Get-Service -Name $svcName | Format-Table Name, Status -AutoSize)
& sc.exe stop $svcName 2>&1 | LogObj
Start-Sleep -Seconds 2

Section 'STEP 9: uninstall (with -PurgeData)'
& "$pkgRoot\scripts\uninstall.ps1" `
    -InstallDir $installDir `
    -DataDir    $dataDir `
    -ServiceName $svcName `
    -PurgeData 2>&1 | LogObj

Section 'STEP 10: verify cleanup'
foreach ($check in @{p=$installDir; tag='install dir'},@{p=$dataDir; tag='data dir'}) {
    if (Test-Path $check.p) { Log "FAIL: $($check.tag) still exists at $($check.p)" }
    else                    { Log "PASS: $($check.tag) gone" }
}
$svc2 = Get-Service -Name $svcName -EA SilentlyContinue
if ($svc2) { Log "FAIL: service still registered" } else { Log 'PASS: service removed' }
foreach ($rule in 'dhc-vpn IKE UDP/500','dhc-vpn IKE UDP/4500','dhc-vpn allow inbound on wintun') {
    if (Get-NetFirewallRule -DisplayName $rule -EA SilentlyContinue) {
        Log "FAIL: rule '$rule' still present"
    } else {
        Log "PASS: rule '$rule' removed"
    }
}

Remove-Item -Recurse -Force $extractDir -EA SilentlyContinue

Log ''
Log '=== DONE ==='
Write-Host "test log: $out"
