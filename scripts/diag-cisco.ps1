# Trivial diagnostic helper - no functions, no fancy stuff.
# Run elevated, after charon-svc is already started via run-charon.ps1.

$ErrorActionPreference = 'Continue'

$repoRoot = Split-Path -Parent $PSScriptRoot
$base    = Join-Path $repoRoot 'build\charon-install'
$swanctl = "$base\sbin\swanctl.exe"
$env:STRONGSWAN_CONF = "$base\etc\strongswan.conf"
$env:SWANCTL_DIR     = "$base\etc\swanctl"
$log     = Join-Path $repoRoot 'diag-cisco.log'

if (Test-Path $log) { Remove-Item -Force $log }
"diag-cisco.ps1 run at $(Get-Date)" | Out-File -FilePath $log -Encoding utf8

Write-Host "=== check swanctl.exe path ==="
Write-Host "swanctl = $swanctl"
if (-not (Test-Path $swanctl)) {
    Write-Host "ERROR: swanctl.exe not found" -ForegroundColor Red
    return
}

Write-Host "=== check VICI on 127.0.0.1:4502 ==="
$tcp = New-Object System.Net.Sockets.TcpClient
$ok  = $tcp.ConnectAsync('127.0.0.1', 4502).Wait(2000)
$tcp.Close()
Write-Host "VICI reachable: $ok"
if (-not $ok) {
    Write-Host "ERROR: charon-svc not listening - start run-charon.ps1 first" -ForegroundColor Red
    return
}

"=== swanctl --load-all ===" | Tee-Object -FilePath $log -Append
& $swanctl --load-all 2>&1   | Tee-Object -FilePath $log -Append

"=== swanctl --list-conns ===" | Tee-Object -FilePath $log -Append
& $swanctl --list-conns 2>&1   | Tee-Object -FilePath $log -Append

"=== swanctl --list-certs ===" | Tee-Object -FilePath $log -Append
& $swanctl --list-certs 2>&1   | Tee-Object -FilePath $log -Append

"=== swanctl --initiate --child drhc-cert ===" | Tee-Object -FilePath $log -Append
& $swanctl --initiate --child drhc-cert 2>&1   | Tee-Object -FilePath $log -Append

Start-Sleep -Seconds 3

"=== swanctl --list-sas ===" | Tee-Object -FilePath $log -Append
& $swanctl --list-sas 2>&1   | Tee-Object -FilePath $log -Append

"=== Get-NetIPAddress dhc-vpn ===" | Tee-Object -FilePath $log -Append
Get-NetIPAddress -InterfaceAlias dhc-vpn -ErrorAction SilentlyContinue |
    Format-Table InterfaceAlias, IPAddress, PrefixLength, AddressState |
    Out-String | Tee-Object -FilePath $log -Append

Write-Host ""
Write-Host "Log file: $log" -ForegroundColor Yellow
Write-Host "Full content:"
Get-Content $log
