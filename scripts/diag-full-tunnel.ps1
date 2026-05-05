# Full-tunnel diagnostic script.
# Switches drhc-cert-gui to remote_ts=0.0.0.0/0, initiates SA via swanctl
# (bypasses GUI), runs a series of probes, captures everything to a log
# file, then restores split-tunnel and terminates.
#
# Run from regular or elevated PowerShell. Some probes need admin and are
# skipped if not elevated -- the script tells you.
#
# Usage:    .\scripts\diag-full-tunnel.ps1
# Output:   .\diag-full-tunnel-YYYYMMDD-HHMMSS.log  (paste this back to claude)

$ErrorActionPreference = 'Continue'
$repoRoot = Split-Path -Parent $PSScriptRoot
$base    = Join-Path $repoRoot 'build\charon-install'
$swanctl = "$base\sbin\swanctl.exe"
$conf    = "$base\etc\swanctl\conf.d\drhc-cert-gui.conf"
$charlog = "$base\charon-debug.log"
$out     = "$PSScriptRoot\..\diag-full-tunnel-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
$out     = (Resolve-Path -LiteralPath (Split-Path $out -Parent)).Path + '\' + (Split-Path $out -Leaf)

$env:STRONGSWAN_CONF = "$base\etc\strongswan.conf"
$env:SWANCTL_DIR     = "$base\etc\swanctl"

function Log { param([string]$x) Add-Content -LiteralPath $out -Value $x }
function Section { param([string]$name) Log ''; Log ('=' * 60); Log $name; Log ('=' * 60) }
function LogObj { param($o) ($o | Out-String -Width 200).TrimEnd() | ForEach-Object { Log $_ } }

# Admin check (some probes need it)
$id    = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdm = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)

Set-Content -LiteralPath $out -Value "diag-full-tunnel started $(Get-Date)"
Log "running elevated: $isAdm"
Log "swanctl: $swanctl"
Log "conf:    $conf"
Log "charlog: $charlog"
Log "output:  $out"

# --- Pre-state ---------------------------------------------------------
Section 'PRE-STATE: charon reachable?'
$probe = & $swanctl --list-conns 2>&1
LogObj $probe
if ($probe -match 'Connection refused') {
    Log 'FATAL: charon-svc not running. Start it first via run-charon.ps1.'
    return
}

# --- Conf swap to full-tunnel ------------------------------------------
Section 'CONF: swap remote_ts to 0.0.0.0/0'
$origConf = Get-Content -LiteralPath $conf -Raw
$origTs   = if ($origConf -match 'remote_ts\s*=\s*([^\r\n]+)') { $matches[1].Trim() } else { '<unknown>' }
Log "original remote_ts: $origTs"
$newConf = $origConf -replace 'remote_ts\s*=\s*[^\r\n]+', 'remote_ts = 0.0.0.0/0'
Set-Content -LiteralPath $conf -Value $newConf -NoNewline
Log 'wrote remote_ts = 0.0.0.0/0'
$loadOut = & $swanctl --load-all 2>&1 | Where-Object { $_ -match 'loaded connection|fail' -and $_ -notmatch 'failed: No such file' }
LogObj $loadOut

# --- Initiate via swanctl directly (no GUI) ----------------------------
Section 'INITIATE drhc-cert-gui'
& $swanctl --terminate --ike drhc-cert-gui 2>&1 | Out-Null
Start-Sleep -Seconds 1
$initOut = & $swanctl --initiate --child drhc-cert-gui 2>&1
LogObj $initOut
Start-Sleep -Seconds 3

# --- VIP ---------------------------------------------------------------
$vip = (Get-NetIPAddress -InterfaceAlias dhc-vpn -AddressFamily IPv4 -EA SilentlyContinue |
        Where-Object { $_.IPAddress -notlike '169.254.*' }).IPAddress
Log ''
Log "wintun VIP: $vip"
$haveVip = [bool]$vip
if (-not $haveVip) {
    Log 'WARN: no VIP after initiate. Skipping ping tests, going straight to cleanup.'
}

if ($haveVip) {

# --- State snapshot ----------------------------------------------------
Section 'SA list'
LogObj (& $swanctl --list-sas 2>&1 | Select-Object -First 12)

Section 'all 0.0.0.0/0 routes (sorted by total metric)'
LogObj (Get-NetRoute -DestinationPrefix '0.0.0.0/0' -EA SilentlyContinue |
        Sort-Object {$_.RouteMetric + $_.ifMetric} |
        Select-Object DestinationPrefix, NextHop, RouteMetric, ifMetric, ifIndex, InterfaceAlias |
        Format-Table -AutoSize)

Section 'all routes on wintun'
$wintunIfx = (Get-NetAdapter -Name dhc-vpn -EA SilentlyContinue).ifIndex
LogObj (Get-NetRoute -InterfaceIndex $wintunIfx -AddressFamily IPv4 -EA SilentlyContinue |
        Select-Object DestinationPrefix, NextHop, RouteMetric, ifMetric |
        Format-Table -AutoSize)

Section 'NetIPInterface flags (wintun)'
LogObj (Get-NetIPInterface -InterfaceAlias dhc-vpn -AddressFamily IPv4 -EA SilentlyContinue |
        Format-List Forwarding, Advertising, AutomaticMetric, InterfaceMetric,
                    WeakHostSend, WeakHostReceive, ConnectionState, Dhcp)

Section 'NetConnectionProfile (firewall category)'
LogObj (Get-NetConnectionProfile -InterfaceAlias dhc-vpn -EA SilentlyContinue |
        Format-List Name, NetworkCategory, IPv4Connectivity, IPv6Connectivity)

Section 'Firewall profile defaults'
LogObj (Get-NetFirewallProfile -EA SilentlyContinue |
        Format-Table Name, Enabled, DefaultInboundAction, DefaultOutboundAction -AutoSize)

# --- Optional: try profile fix if admin --------------------------------
if ($isAdm) {
    Section 'ATTEMPT: switch wintun NetworkCategory to Private (admin)'
    try {
        Set-NetConnectionProfile -InterfaceAlias dhc-vpn -NetworkCategory Private -EA Stop
        Log 'OK: switched to Private'
    } catch {
        Log "FAIL: $($_.Exception.Message)"
    }
    LogObj (Get-NetConnectionProfile -InterfaceAlias dhc-vpn -EA SilentlyContinue |
            Select-Object Name, NetworkCategory)
} else {
    Log ''
    Log 'NOTE: not elevated -- skipping NetworkCategory + WeakHostReceive fixes.'
}

# --- LAN ping (baseline -- should always work) -------------------------
Section 'TEST 1: ping LAN 192.168.99.1 -S VIP (should reply)'
$tx0 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx0 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
$p1  = ping -n 3 -w 2000 -S $vip 192.168.99.1 2>&1
LogObj $p1
$tx1 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx1 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
Log "WINTUN delta: TX=$($tx1-$tx0) RX=$($rx1-$rx0)"

# --- Public ping 8.8.8.8 -----------------------------------------------
Section 'TEST 2: ping 8.8.8.8 -S VIP (the failing case)'
Start-Sleep -Seconds 2
$tx0 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx0 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
$p2  = ping -n 3 -w 2000 -S $vip 8.8.8.8 2>&1
LogObj $p2
$tx1 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx1 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
Log "WINTUN delta: TX=$($tx1-$tx0) RX=$($rx1-$rx0)"

# --- Try alternate destination -----------------------------------------
Section 'TEST 3: ping 1.1.1.1 -S VIP (cross-check different upstream)'
Start-Sleep -Seconds 2
$tx0 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx0 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
$p3  = ping -n 3 -w 2000 -S $vip 1.1.1.1 2>&1
LogObj $p3
$tx1 = (Get-NetAdapterStatistics -Name dhc-vpn).SentBytes
$rx1 = (Get-NetAdapterStatistics -Name dhc-vpn).ReceivedBytes
Log "WINTUN delta: TX=$($tx1-$tx0) RX=$($rx1-$rx0)"

# --- pktmon (admin only) ------------------------------------------------
if ($isAdm) {
    Section 'PKTMON capture on wintun during ping (admin)'
    $etl = "$PSScriptRoot\..\diag-pktmon.etl"
    $txt = "$PSScriptRoot\..\diag-pktmon.txt"
    pktmon stop 2>&1 | Out-Null
    pktmon filter remove 2>&1 | Out-Null
    pktmon filter add Ping1 -p ICMP 2>&1 | LogObj
    pktmon start --capture -c "dhc-vpn" -p 0 -f $etl 2>&1 | LogObj
    Start-Sleep -Milliseconds 500
    ping -n 3 -w 2000 -S $vip 8.8.8.8 2>&1 | Out-Null
    Start-Sleep -Milliseconds 500
    pktmon stop 2>&1 | LogObj
    if (Test-Path $etl) {
        pktmon format $etl -o $txt 2>&1 | Out-Null
        Section 'PKTMON formatted output (last 60 lines)'
        Get-Content -LiteralPath $txt -Tail 60 -EA SilentlyContinue | ForEach-Object { Log $_ }
        Remove-Item -LiteralPath $etl -EA SilentlyContinue
        Remove-Item -LiteralPath $txt -EA SilentlyContinue
    } else {
        Log 'pktmon ETL not produced -- skipping format'
    }
}

} # end if $haveVip

# --- Cleanup -----------------------------------------------------------
Section 'CLEANUP: terminate + restore conf'
& $swanctl --terminate --ike drhc-cert-gui 2>&1 | LogObj
Set-Content -LiteralPath $conf -Value $origConf -NoNewline
Log "restored remote_ts to original: $origTs"
& $swanctl --load-all 2>&1 | Where-Object { $_ -match 'loaded connection|fail' -and $_ -notmatch 'failed: No such file' } | LogObj

# --- Charon log tail ---------------------------------------------------
Section 'CHARON LOG: last 100 lines (filtered)'
Get-Content -LiteralPath $charlog -Tail 200 -EA SilentlyContinue |
    Where-Object { $_ -notmatch 'vici client \d+' } |
    Select-Object -Last 100 |
    ForEach-Object { Log $_ }

Log ''
Log "=== DONE. Paste $out back to claude. ==="
Write-Host "diagnostic written to $out"
