# M7 step 1 -- one-shot end-to-end test of the WinDivert capture spike.
#
# Run from an elevated PowerShell. Self-contained: stops any leftover spike
# / charon processes, restarts charon-svc with stdout captured, runs the
# spike alongside, dials Cisco, generates ping traffic, terminates, and
# prints a verdict.
#
# Usage:
#     # in admin PowerShell:
#     cd <repo-root>
#     .\scripts\m7-spike-test.ps1
#
# Optional switches:
#     -KeepCharon       leave existing charon-svc running, do NOT restart
#                       (useful if the user already has it in another term)
#     -PingCount N      ping count to drive ESP through the tunnel (default 10)
#     -CaptureSeconds N total spike capture window in seconds (default 30)

[CmdletBinding()]
param(
    [switch]$KeepCharon,
    [int]$PingCount = 10,
    [int]$CaptureSeconds = 30
)

$ErrorActionPreference = 'Continue'

$root  = Split-Path -Parent $PSScriptRoot
$base  = "$root\build\charon-install"
$swanctl  = "$base\sbin\swanctl.exe"
$charon   = "$base\bin\charon-svc.exe"
$spikeExe = "$root\build\src\windivert-spike\windivert-spike.exe"
$spikeLog = "$root\build\windivert-spike.log"
$charonLog = "$root\build\charon-svc.log"

# --- 0. assert elevation -------------------------------------------------
$id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$pp = New-Object System.Security.Principal.WindowsPrincipal($id)
if (-not $pp.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "ERROR: this script needs to run from an elevated PowerShell." -ForegroundColor Red
    exit 1
}

# --- 1. clean slate ------------------------------------------------------
Write-Host "==> killing any leftover windivert-spike.exe ..."
Get-Process windivert-spike -ErrorAction SilentlyContinue |
    ForEach-Object { Stop-Process -Id $_.Id -Force; Write-Host "    killed PID=$($_.Id)" }

if (-not $KeepCharon) {
    Write-Host "==> stopping any running charon-svc.exe ..."
    Get-Process charon-svc -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.Id -Force; Write-Host "    killed PID=$($_.Id)" }
    Start-Sleep -Seconds 2
}

Remove-Item $spikeLog,$charonLog -ErrorAction SilentlyContinue

# --- 2. start charon-svc with stdout capture (unless -KeepCharon) --------
$env:STRONGSWAN_CONF = "$base\etc\strongswan.conf"
$env:SWANCTL_DIR     = "$base\etc\swanctl"

if (-not $KeepCharon) {
    Write-Host "==> starting charon-svc.exe (log: $charonLog)"
    $charonProc = Start-Process -FilePath $charon `
        -RedirectStandardOutput $charonLog `
        -RedirectStandardError "$charonLog.err" `
        -PassThru -WindowStyle Hidden
    Write-Host "    charon PID=$($charonProc.Id)"

    # Wait for VICI to come up on 127.0.0.1:4502 (charon's TCP control socket).
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
        if (Test-Path $charonLog) { Get-Content $charonLog -Tail 20 }
        exit 2
    }
    Write-Host "    VICI ready"
} else {
    Write-Host "==> -KeepCharon: assuming charon-svc is already running"
}

# --- 3. start the spike --------------------------------------------------
Write-Host "==> starting windivert-spike.exe (log: $spikeLog)"
$spikeProc = Start-Process -FilePath $spikeExe `
    -WorkingDirectory (Split-Path $spikeExe) `
    -RedirectStandardOutput $spikeLog `
    -RedirectStandardError "$spikeLog.err" `
    -PassThru -WindowStyle Hidden
Write-Host "    spike PID=$($spikeProc.Id)"
Start-Sleep -Seconds 2
if ($spikeProc.HasExited) {
    Write-Host "ERROR: spike exited prematurely (code=$($spikeProc.ExitCode))" -ForegroundColor Red
    if (Test-Path $spikeLog) { Get-Content $spikeLog }
    exit 3
}

# --- 4. baseline IPsec.sys invalid-SPI counter ---------------------------
function Get-IPsecInvalidSpi {
    # netsh outputs "Unzul{ae}ssige SPI-Pakete : N" on DE locale, or
    # "Invalid SPI Packets : N" on EN. Match either, extract the number.
    $line = netsh ipsec dynamic show all 2>$null |
        Select-String -Pattern 'Unzul.*SPI|invalid.*SPI' |
        Select-Object -First 1
    if (-not $line) { return $null }
    if ($line.Line -match ':\s*(\d+)') { return [int]$Matches[1] }
    return $null
}

$spiBefore = Get-IPsecInvalidSpi
Write-Host "==> IPsec.sys invalid-SPI counter (before): $spiBefore"

# --- 5. dial Cisco -------------------------------------------------------
Write-Host "==> swanctl --load-all"
& $swanctl --load-all 2>&1 | Select-Object -Last 4

Write-Host "==> swanctl --initiate --child drhc-cert"
& $swanctl --initiate --child drhc-cert 2>&1 | Select-Object -Last 8
$initRc = $LASTEXITCODE
Write-Host "    rc=$initRc"
if ($initRc -ne 0) {
    Write-Host "ERROR: initiate failed; aborting" -ForegroundColor Red
}

Start-Sleep -Seconds 2

# --- 6. snapshot adapter / route state (M6 regression check) ------------
Write-Host "`n==> dhc-vpn adapter IP addresses"
Get-NetIPAddress -InterfaceAlias dhc-vpn -ErrorAction SilentlyContinue |
    Format-Table InterfaceAlias,IPAddress,PrefixLength,AddressState | Out-String | Write-Host

Write-Host "==> route to 192.168.99.0/24"
$route = Get-NetRoute -DestinationPrefix 192.168.99.0/24 -ErrorAction SilentlyContinue
if ($route) {
    $route | Format-Table InterfaceAlias,NextHop,RouteMetric,ifIndex | Out-String | Write-Host
} else {
    Write-Host "    NO ROUTE -- M6 regression: VIP/route never installed" -ForegroundColor Yellow
}

# --- 7. drive ESP traffic ------------------------------------------------
Write-Host "==> ping 192.168.99.1 -n $PingCount (drives ESP outbound)"
ping.exe 192.168.99.1 -n $PingCount -w 1000 2>&1 | Select-Object -Last 6

# --- 8. snapshot charon SA counters --------------------------------------
Write-Host "`n==> swanctl SA counters"
& $swanctl --list-sas 2>&1 | Select-String -Pattern 'in  |out |bytes' | Select-Object -First 4

# --- 9. snapshot IPsec.sys counter again ---------------------------------
$spiAfter = Get-IPsecInvalidSpi
Write-Host "`n==> IPsec.sys invalid-SPI counter (after):  $spiAfter"
if ($spiBefore -ne $null -and $spiAfter -ne $null) {
    Write-Host "    delta = $($spiAfter - $spiBefore) packets dropped by IPsec.sys during the test"
}

# --- 10. terminate IKE_SA ------------------------------------------------
Write-Host "`n==> swanctl --terminate --ike drhc-cert"
& $swanctl --terminate --ike drhc-cert 2>&1 | Select-Object -Last 3

# --- 11. let the spike drain, then stop it -------------------------------
$elapsed = (Get-Date) - $spikeProc.StartTime
$remaining = $CaptureSeconds - [int]$elapsed.TotalSeconds
if ($remaining -gt 0) {
    Write-Host "==> letting spike drain for $remaining s ..."
    Start-Sleep -Seconds $remaining
}

Write-Host "==> stopping spike (PID=$($spikeProc.Id))"
Stop-Process -Id $spikeProc.Id -Force -ErrorAction SilentlyContinue

# --- 12. analyze the spike log ------------------------------------------
Start-Sleep -Milliseconds 500
if (-not (Test-Path $spikeLog)) {
    Write-Host "ERROR: spike log missing" -ForegroundColor Red
    exit 4
}

Write-Host "`n--- spike log (full) ---"
Get-Content $spikeLog
Write-Host "--- end spike log ---`n"

$espLines = (Select-String -Path $spikeLog -Pattern '^\[.*\] ESP' -ErrorAction SilentlyContinue) | Measure-Object
$ikeLines = (Select-String -Path $spikeLog -Pattern '^\[.*\] IKE' -ErrorAction SilentlyContinue) | Measure-Object

Write-Host "===== M7 STEP 1 VERDICT =====" -ForegroundColor Cyan
Write-Host ("Spike captured  IKE  : {0}" -f $ikeLines.Count)
Write-Host ("Spike captured  ESP  : {0}" -f $espLines.Count)
if ($spiBefore -ne $null -and $spiAfter -ne $null) {
    Write-Host ("IPsec.sys drops      : {0}" -f ($spiAfter - $spiBefore))
}
Write-Host ""
if ($espLines.Count -gt 0) {
    Write-Host "PASS: ESP-in-UDP/4500 inbound captured by WinDivert NETWORK layer." -ForegroundColor Green
    Write-Host "      M7 hypothesis confirmed: capture path is *before* IPsec.sys drop."
} elseif ($ikeLines.Count -gt 0) {
    Write-Host "PARTIAL: IKE inbound captured (proves stack works)," -ForegroundColor Yellow
    Write-Host "         but ESP never arrived -- Cisco never sent it." -ForegroundColor Yellow
    Write-Host "         Likely root cause: M6 VIP/route regression (see step 6 above)."
} else {
    Write-Host "FAIL: no UDP/4500 inbound captured at all." -ForegroundColor Red
    Write-Host "      Check: is filter compiling, is driver loaded, did spike crash?"
}

# --- 13. dump charon's [KNL] lines if VIP/route went missing -------------
if (-not $route -and (Test-Path $charonLog)) {
    Write-Host "`n--- charon [KNL]/[NET] entries (M6 diagnosis) ---" -ForegroundColor Yellow
    Select-String -Path $charonLog -Pattern '\[KNL\]|\[NET\]|VIP|route|wintun' |
        ForEach-Object { Write-Host "    $($_.Line)" }
    Write-Host "--- end charon entries ---"
}

if (-not $KeepCharon) {
    Write-Host "`n==> stopping charon-svc.exe (PID=$($charonProc.Id))"
    Stop-Process -Id $charonProc.Id -Force -ErrorAction SilentlyContinue
}

Write-Host "`nLogs:"
Write-Host "    spike : $spikeLog"
Write-Host "    charon: $charonLog (filter for [KNL] to find route/VIP issues)"
