# Test helper for the drhc-cert connection -- run in an elevated PowerShell.
#
# Workflow:
#   Terminal 1 (also elevated):  start charon-svc.exe directly so you see its
#                                logs live; leave it running.
#   Terminal 2 (this script):    .\scripts\test-cisco.ps1
#
# Or, in one shot if you only want load+initiate without watching daemon logs:
#   .\scripts\test-cisco.ps1 -Daemon
#
[CmdletBinding()]
param(
    [switch]$Daemon          # also start charon-svc.exe in this session
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$base    = Join-Path $repoRoot 'build\charon-install'
$bin     = "$base\bin"
$sbin    = "$base\sbin"
$env:STRONGSWAN_CONF = "$base\etc\strongswan.conf"
$env:SWANCTL_DIR     = "$base\etc\swanctl"

if ($Daemon) {
    Write-Host "==> launching charon-svc in the background"
    Start-Process -FilePath "$bin\charon-svc.exe" -WindowStyle Hidden
    Start-Sleep -Seconds 2
}

# Probe VICI (charon's TCP control socket on Windows: 127.0.0.1:4502)
try {
    $probe = (New-Object System.Net.Sockets.TcpClient).ConnectAsync('127.0.0.1', 4502).Wait(2000)
} catch { $probe = $false }
if (-not $probe) {
    Write-Warning "VICI not reachable on 127.0.0.1:4502 -- is charon-svc running (as Admin)?"
}

Write-Host "`n==> swanctl --load-all"
& "$sbin\swanctl.exe" --load-all
if ($LASTEXITCODE -ne 0) { Write-Warning "load-all exit=$LASTEXITCODE" }

Write-Host "`n==> swanctl --list-conns"
& "$sbin\swanctl.exe" --list-conns

Write-Host "`n==> swanctl --list-certs"
& "$sbin\swanctl.exe" --list-certs

Write-Host "`n==> swanctl --initiate --child drhc-cert"
& "$sbin\swanctl.exe" --initiate --child drhc-cert
$initiateRc = $LASTEXITCODE

Start-Sleep -Seconds 2

Write-Host "`n==> swanctl --list-sas"
& "$sbin\swanctl.exe" --list-sas

Write-Host "`n==> Get-NetIPAddress on dhc-vpn adapter"
Get-NetIPAddress -InterfaceAlias dhc-vpn -ErrorAction SilentlyContinue |
    Format-Table InterfaceAlias, IPAddress, PrefixLength, AddressState

if ($initiateRc -eq 0) {
    Write-Host "`n==> ping test through tunnel (one packet)"
    Test-Connection -ComputerName 192.168.99.1 -Count 1 -ErrorAction SilentlyContinue |
        Format-Table Address, ResponseTime, StatusCode
} else {
    Write-Warning "initiate failed (exit=$initiateRc) -- see charon-svc logs in Terminal 1"
}
