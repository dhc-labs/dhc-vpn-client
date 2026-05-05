# M7 step 2 -- end-to-end test of the embedded WinDivert capture.
#
# Restart charon-svc with the new build, dial Cisco, ping the remote
# gateway, then verify that
#   (a) charon's log contains "kernel-wintun-capture: ready"
#   (b) charon's SA `in` byte counter is non-zero
#   (c) optionally, ping replies actually round-trip
#
# Run from an elevated PowerShell.

[CmdletBinding()]
param(
    [int]$PingCount = 8
)

$ErrorActionPreference = 'Continue'

$root  = Split-Path -Parent $PSScriptRoot
$base  = "$root\build\charon-install"
$swanctl  = "$base\sbin\swanctl.exe"
$charon   = "$base\bin\charon-svc.exe"
$charonLog = "$root\build\charon-svc.log"

# --- 0. assert elevation -------------------------------------------------
$id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$pp = New-Object System.Security.Principal.WindowsPrincipal($id)
if (-not $pp.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "ERROR: this script needs to run from an elevated PowerShell." -ForegroundColor Red
    exit 1
}

# --- 1. clean slate ------------------------------------------------------
Write-Host "==> killing leftover windivert-spike + charon-svc ..."
Get-Process windivert-spike -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process charon-svc      -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
Remove-Item $charonLog,"$charonLog.err" -ErrorAction SilentlyContinue

# --- 2. start charon-svc with stdout capture ----------------------------
$env:STRONGSWAN_CONF = "$base\etc\strongswan.conf"
$env:SWANCTL_DIR     = "$base\etc\swanctl"

Write-Host "==> starting charon-svc.exe (log: $charonLog)"
$p = Start-Process -FilePath $charon `
    -RedirectStandardOutput $charonLog `
    -RedirectStandardError "$charonLog.err" `
    -PassThru -WindowStyle Hidden
Write-Host "    charon PID=$($p.Id)"

# Wait for VICI and the capture component to come up.
$deadline = (Get-Date).AddSeconds(15)
$ready = $false
while ((Get-Date) -lt $deadline) {
    try {
        $tcp = New-Object System.Net.Sockets.TcpClient
        if ($tcp.ConnectAsync('127.0.0.1', 4502).Wait(500)) { $ready = $true; $tcp.Close(); break }
        $tcp.Close()
    } catch {}
    Start-Sleep -Milliseconds 500
}
if (-not $ready) {
    Write-Host "ERROR: VICI never came up. Charon log tail:" -ForegroundColor Red
    if (Test-Path $charonLog) { Get-Content $charonLog -Tail 30 }
    exit 2
}

# Give the capture worker a beat to register.
Start-Sleep -Seconds 1

Write-Host "`n--- charon log: capture-related lines so far ---"
Select-String -Path $charonLog -Pattern 'kernel-wintun|windivert' -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "    $($_.Line)" }
Write-Host "--- end ---`n"

$captureReady = Select-String -Path $charonLog -Pattern 'kernel-wintun-capture: ready' -ErrorAction SilentlyContinue
if (-not $captureReady) {
    Write-Host "WARNING: 'kernel-wintun-capture: ready' not seen in charon log yet." -ForegroundColor Yellow
    Write-Host "         Either WinDivert load failed, or the plugin feature did not register." -ForegroundColor Yellow
}

# --- 3. dial Cisco -------------------------------------------------------
Write-Host "==> swanctl --load-all"
& $swanctl --load-all 2>&1 | Select-Object -Last 4

Write-Host "==> swanctl --initiate --child drhc-cert"
& $swanctl --initiate --child drhc-cert 2>&1 | Select-Object -Last 8
$rc = $LASTEXITCODE
Write-Host "    rc=$rc"
if ($rc -ne 0) {
    Write-Host "ERROR: initiate failed; aborting" -ForegroundColor Red
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    exit 3
}

Start-Sleep -Seconds 1

# --- 4. drive ESP traffic ------------------------------------------------
Write-Host "`n==> ping 192.168.99.1 -n $PingCount"
$pingOut = ping.exe 192.168.99.1 -n $PingCount -w 1500 2>&1
$pingOut | Select-Object -Last 6

# Count successful replies in the ping output (works in DE and EN locales).
$replies = ($pingOut | Select-String -Pattern 'Antwort von|Reply from').Count

# --- 5. snapshot charon SA counters --------------------------------------
Write-Host "`n==> swanctl --list-sas (counters)"
$sasOut = & $swanctl --list-sas 2>&1
$sasOut | Select-String -Pattern 'in  |out |bytes|established' | Select-Object -First 6

# Parse `in` counter: line shape "    in  abcd1234,    1234 bytes,    10 packets, ..."
$inLine  = $sasOut | Select-String -Pattern '^\s*in\s+\w+,'  | Select-Object -First 1
$outLine = $sasOut | Select-String -Pattern '^\s*out\s+\w+,' | Select-Object -First 1
$inBytes  = if ($inLine.Line  -match '\s(\d+)\s+bytes') { [int64]$Matches[1] } else { 0 }
$outBytes = if ($outLine.Line -match '\s(\d+)\s+bytes') { [int64]$Matches[1] } else { 0 }

# --- 6. dump capture log lines from charon -------------------------------
Write-Host "`n--- charon log: capture activity ---"
Select-String -Path $charonLog -Pattern 'kernel-wintun-capture' -ErrorAction SilentlyContinue |
    Select-Object -Last 20 |
    ForEach-Object { Write-Host "    $($_.Line)" }
Write-Host "--- end ---"

# --- 7. terminate IKE_SA -------------------------------------------------
Write-Host "`n==> swanctl --terminate --ike drhc-cert"
& $swanctl --terminate --ike drhc-cert 2>&1 | Select-Object -Last 3

# --- 8. verdict ----------------------------------------------------------
Write-Host "`n===== M7 STEP 2 VERDICT =====" -ForegroundColor Cyan
Write-Host ("charon SA  in : {0} bytes" -f $inBytes)
Write-Host ("charon SA  out: {0} bytes" -f $outBytes)
Write-Host ("ping replies  : {0} of {1}" -f $replies, $PingCount)
Write-Host ""

if ($inBytes -gt 0 -and $replies -gt 0) {
    Write-Host "PASS: inbound ESP decrypted by libipsec, ping round-trip works." -ForegroundColor Green
} elseif ($inBytes -gt 0) {
    Write-Host "PARTIAL: ESP decrypted ($inBytes B in) but no ping replies." -ForegroundColor Yellow
    Write-Host "         Decrypt path works; the issue is in the wintun-deliver path or routing."
} elseif ($outBytes -gt 0) {
    Write-Host "FAIL: outbound ESP flowed but inbound did not reach libipsec." -ForegroundColor Red
    Write-Host "      Capture worker may not be queueing -- check charon log for 'kernel-wintun-capture: ESP' lines."
} else {
    Write-Host "FAIL: no traffic in either direction." -ForegroundColor Red
}

# --- 9. teardown ---------------------------------------------------------
Write-Host "`n==> stopping charon-svc.exe (PID=$($p.Id))"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

Write-Host "`nLog: $charonLog"
