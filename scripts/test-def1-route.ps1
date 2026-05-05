# Validation script for the "def1" routing trick on full-tunnel.
# Brings up the tunnel with remote_ts=0.0.0.0/0, installs:
#   - 0.0.0.0/1   on wintun  (overrides WLAN default by longest-prefix match)
#   - 128.0.0.0/1 on wintun
#   - <VPN-endpoint>/32 via current WLAN gateway (avoids ESP routing loop)
# then tests WITHOUT -S VIP (ping, TTL check, external IP check),
# tears everything back down.
#
# Run from ELEVATED PowerShell. New-NetRoute requires admin.
#
# Usage:    .\scripts\test-def1-route.ps1 -VpnHost vpn.example.com
#           $env:DHCVPN_HOST='vpn.example.com'; .\scripts\test-def1-route.ps1
# Output:   .\test-def1-route-YYYYMMDD-HHMMSS.log

[CmdletBinding()]
param(
    [string]$VpnHost = $env:DHCVPN_HOST
)

$ErrorActionPreference = 'Continue'
$repoRoot = Split-Path -Parent $PSScriptRoot
$base    = Join-Path $repoRoot 'build\charon-install'
$swanctl = "$base\sbin\swanctl.exe"
$conf    = "$base\etc\swanctl\conf.d\drhc-cert-gui.conf"
$charlog = "$base\charon-debug.log"
$out     = "$PSScriptRoot\..\test-def1-route-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
$out     = (Resolve-Path -LiteralPath (Split-Path $out -Parent)).Path + '\' + (Split-Path $out -Leaf)

$env:STRONGSWAN_CONF = "$base\etc\strongswan.conf"
$env:SWANCTL_DIR     = "$base\etc\swanctl"

function Log { param([string]$x) Add-Content -LiteralPath $out -Value $x }
function Section { param([string]$name) Log ''; Log ('=' * 60); Log $name; Log ('=' * 60) }
function LogObj { param($o) ($o | Out-String -Width 200).TrimEnd() | ForEach-Object { Log $_ } }

# swanctl wrapper with hard timeout -- if charon hangs or dies, vici socket
# blocks indefinitely. Kill the swanctl client after $TimeoutSec.
function Invoke-Swanctl {
    param([string[]]$ArgList, [int]$TimeoutSec = 8)
    $stdout = "$env:TEMP\swanctl-out-$PID.tmp"
    $stderr = "$env:TEMP\swanctl-err-$PID.tmp"
    $p = Start-Process -FilePath $swanctl -ArgumentList $ArgList -NoNewWindow -PassThru `
                       -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        Log "TIMEOUT: swanctl $($ArgList -join ' ') -- killed after $TimeoutSec s"
    }
    if (Test-Path $stdout) { Get-Content -LiteralPath $stdout | LogObj; Remove-Item $stdout -EA SilentlyContinue }
    if (Test-Path $stderr) { Get-Content -LiteralPath $stderr | LogObj; Remove-Item $stderr -EA SilentlyContinue }
}

# Admin gate -- New-NetRoute needs it
$id    = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdm = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdm) {
    Write-Host "ERROR: must run elevated. New-NetRoute requires admin." -ForegroundColor Red
    return
}

Set-Content -LiteralPath $out -Value "test-def1-route started $(Get-Date)"
Log "swanctl: $swanctl"
Log "conf:    $conf"
Log "output:  $out"

# --- Snapshot pre-tunnel state ----------------------------------------
Section 'PRE-STATE: capture WLAN gateway BEFORE tunnel up'
# Pick the lowest-metric default route that is NOT dhc-vpn -- that is the
# real upstream gateway we need to keep reachable for ESP packets to flow.
$preRoutes = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -EA SilentlyContinue |
             Where-Object { $_.InterfaceAlias -ne 'dhc-vpn' } |
             Sort-Object { $_.RouteMetric + $_.ifMetric }
LogObj ($preRoutes | Format-Table InterfaceAlias, NextHop, RouteMetric, ifMetric -AutoSize)
$wlanGw   = $preRoutes[0].NextHop
$wlanIfx  = $preRoutes[0].ifIndex
$wlanName = $preRoutes[0].InterfaceAlias
Log "WLAN gateway: $wlanGw (ifIndex=$wlanIfx alias=$wlanName)"

# --- Resolve VPN endpoint IP(s) ---------------------------------------
if (-not $VpnHost) {
    Log 'FATAL: pass -VpnHost <hostname> or set $env:DHCVPN_HOST'
    return
}
Section "PRE-STATE: resolve $VpnHost"
$vpnIps = (Resolve-DnsName -Type A -Name $VpnHost -EA SilentlyContinue |
           Where-Object { $_.IPAddress }).IPAddress | Select-Object -Unique
LogObj $vpnIps
if (-not $vpnIps) {
    Log "FATAL: could not resolve $VpnHost"
    return
}

# --- Charon reachable? ------------------------------------------------
Section 'PRE-STATE: charon reachable?'
$probe = & $swanctl --list-conns 2>&1
LogObj ($probe | Select-Object -First 3)
if ($probe -match 'Connection refused') {
    Log 'FATAL: charon-svc not running. Start it first via run-charon.ps1.'
    return
}

# --- Conf swap to full-tunnel + initiate ------------------------------
Section 'CONF: swap remote_ts to 0.0.0.0/0 + initiate'
$origConf = Get-Content -LiteralPath $conf -Raw
$origTs   = if ($origConf -match 'remote_ts\s*=\s*([^\r\n]+)') { $matches[1].Trim() } else { '<unknown>' }
Log "original remote_ts: $origTs"
$newConf = $origConf -replace 'remote_ts\s*=\s*[^\r\n]+', 'remote_ts = 0.0.0.0/0'
Set-Content -LiteralPath $conf -Value $newConf -NoNewline
Invoke-Swanctl @('--load-all')
Invoke-Swanctl @('--terminate', '--ike', 'drhc-cert-gui')
Start-Sleep -Seconds 1
Invoke-Swanctl @('--initiate', '--child', 'drhc-cert-gui') 15
Start-Sleep -Seconds 3

# --- Get VIP ----------------------------------------------------------
$vip = (Get-NetIPAddress -InterfaceAlias dhc-vpn -AddressFamily IPv4 -EA SilentlyContinue |
        Where-Object { $_.IPAddress -notlike '169.254.*' }).IPAddress
Log ''
Log "wintun VIP: $vip"
if (-not $vip) {
    Log 'FATAL: no VIP after initiate. Aborting.'
    Set-Content -LiteralPath $conf -Value $origConf -NoNewline
    return
}

# --- Make sure wintun is on Private profile (firewall) ----------------
Section 'PRE-TEST: ensure NetworkCategory=Private on wintun'
try {
    Set-NetConnectionProfile -InterfaceAlias dhc-vpn -NetworkCategory Private -EA Stop
    Log 'OK: switched to Private (or already was)'
} catch {
    Log "WARN: $($_.Exception.Message)"
}

# --- Routes BEFORE def1 ------------------------------------------------
Section 'ROUTES: before def1 install'
LogObj (Get-NetRoute -DestinationPrefix '0.0.0.0/0' -EA SilentlyContinue |
        Sort-Object {$_.RouteMetric + $_.ifMetric} |
        Format-Table InterfaceAlias, NextHop, RouteMetric, ifMetric -AutoSize)

# --- Baseline ping WITHOUT def1 (expect: WLAN, not tunnel) ------------
Section 'BASELINE (no def1, no -S): ping 8.8.8.8'
$bp = ping -n 3 -w 2000 8.8.8.8 2>&1
LogObj $bp
$baselineTtl = if (($bp -join "`n") -match 'TTL=(\d+)') { $matches[1] } else { '?' }
Log "baseline TTL=$baselineTtl  (expect ~57-64 via WLAN; if 118 then already tunneling)"

# --- Install def1 routes ----------------------------------------------
Section 'DEF1: install /1 routes on wintun + hostroute for VPN endpoint'
$wintunIfx = (Get-NetAdapter -Name dhc-vpn -EA SilentlyContinue).ifIndex
Log "wintun ifIndex=$wintunIfx"

# Hostroute to VPN endpoint via WLAN gateway -- prevents ESP loop
foreach ($ip in $vpnIps) {
    try {
        New-NetRoute -DestinationPrefix "$ip/32" -InterfaceIndex $wlanIfx `
                     -NextHop $wlanGw -RouteMetric 1 -PolicyStore ActiveStore -EA Stop | Out-Null
        Log "OK: hostroute $ip/32 via $wlanGw on $wlanName"
    } catch {
        Log "FAIL: hostroute $ip/32 -- $($_.Exception.Message)"
    }
}

# Two /1 covers full IPv4 space, beats /0 by longest-prefix-match
foreach ($p in @('0.0.0.0/1', '128.0.0.0/1')) {
    try {
        New-NetRoute -DestinationPrefix $p -InterfaceIndex $wintunIfx `
                     -NextHop '0.0.0.0' -RouteMetric 1 -PolicyStore ActiveStore -EA Stop | Out-Null
        Log "OK: $p on wintun"
    } catch {
        Log "FAIL: $p -- $($_.Exception.Message)"
    }
}

Section 'ROUTES: after def1 install'
LogObj (Get-NetRoute -DestinationPrefix '0.0.0.0/0','0.0.0.0/1','128.0.0.0/1' -EA SilentlyContinue |
        Sort-Object DestinationPrefix, {$_.RouteMetric + $_.ifMetric} |
        Format-Table DestinationPrefix, InterfaceAlias, NextHop, RouteMetric, ifMetric -AutoSize)
foreach ($ip in $vpnIps) {
    LogObj (Get-NetRoute -DestinationPrefix "$ip/32" -EA SilentlyContinue |
            Format-Table DestinationPrefix, InterfaceAlias, NextHop, RouteMetric, ifMetric -AutoSize)
}

Start-Sleep -Seconds 2

# --- TEST 1: ping 8.8.8.8 WITHOUT -S (the real test) ------------------
Section 'TEST 1: ping 8.8.8.8 (no -S, the real full-tunnel test)'
$tx0 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx0 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
$t1  = ping -n 4 -w 2000 8.8.8.8 2>&1
LogObj $t1
$tx1 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx1 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
$t1Ttl = if (($t1 -join "`n") -match 'TTL=(\d+)') { $matches[1] } else { '?' }
Log "TTL=$t1Ttl  (expect 118 via Cisco-hop; if 57-64 then NOT tunneling)"
Log "WINTUN delta: TX=$($tx1-$tx0) RX=$($rx1-$rx0)"

# --- TEST 2: ping 1.1.1.1 ---------------------------------------------
Section 'TEST 2: ping 1.1.1.1 (no -S, cross-check)'
$tx0 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx0 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
$t2  = ping -n 3 -w 2000 1.1.1.1 2>&1
LogObj $t2
$tx1 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx1 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
Log "WINTUN delta: TX=$($tx1-$tx0) RX=$($rx1-$rx0)"

# --- TEST 3: external IP check ----------------------------------------
Section 'TEST 3: external IP (should be Cisco/VPN egress, not home WAN)'
try {
    $extIp = (Invoke-WebRequest -Uri 'https://api.ipify.org' -TimeoutSec 8 -UseBasicParsing -EA Stop).Content
    Log "external IP: $extIp"
    Log "  -- if this is your home IP -> tunnel NOT carrying default traffic"
    Log "  -- if this is the DRHC/Cisco egress IP -> def1 works"
} catch {
    Log "FAIL Invoke-WebRequest: $($_.Exception.Message)"
}

# --- TEST 4: traceroute first hop -------------------------------------
Section 'TEST 4: tracert first 3 hops to 8.8.8.8'
LogObj (tracert -d -h 3 -w 2000 8.8.8.8 2>&1)

# --- Cleanup -----------------------------------------------------------
# Order matters: terminate FIRST (while hostroute to Cisco still exists, so
# DELETE notification can reach the peer). Only then drop the routes.
Section 'CLEANUP: terminate first, then drop routes, then restore conf'
Invoke-Swanctl @('--terminate', '--ike', 'drhc-cert-gui') 10
foreach ($p in @('0.0.0.0/1', '128.0.0.0/1')) {
    Remove-NetRoute -DestinationPrefix $p -InterfaceIndex $wintunIfx `
                    -Confirm:$false -EA SilentlyContinue
    Log "removed $p"
}
foreach ($ip in $vpnIps) {
    Remove-NetRoute -DestinationPrefix "$ip/32" -InterfaceIndex $wlanIfx `
                    -Confirm:$false -EA SilentlyContinue
    Log "removed hostroute $ip/32"
}
Set-Content -LiteralPath $conf -Value $origConf -NoNewline
Log "restored remote_ts to original: $origTs"
Invoke-Swanctl @('--load-all')

# --- Charon log tail --------------------------------------------------
Section 'CHARON LOG: last 60 lines (filtered)'
Get-Content -LiteralPath $charlog -Tail 120 -EA SilentlyContinue |
    Where-Object { $_ -notmatch 'vici client \d+' } |
    Select-Object -Last 60 |
    ForEach-Object { Log $_ }

Log ''
Log "=== DONE. Paste $out back to claude. ==="
Write-Host "test written to $out"
