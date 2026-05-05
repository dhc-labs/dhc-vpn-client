# dhc-vpn -- uninstaller. Run as Administrator.
#
# Removes:
#   - the Windows service
#   - firewall rules
#   - Start-Menu shortcut
#   - program files under $InstallDir
#
# Preserves (unless -PurgeData):
#   - %ProgramData%\dhc-vpn\swanctl\* (certs, profiles, DPAPI blobs, logs)
#
# Wintun and WinDivert drivers are left installed -- removing them is
# not our problem and other software may depend on them.

[CmdletBinding()]
param(
    [string]$InstallDir  = (Join-Path $env:ProgramFiles 'dhc-vpn'),
    [string]$DataDir     = (Join-Path $env:ProgramData  'dhc-vpn'),
    [string]$ServiceName = 'charon-svc',
    [switch]$PurgeData
)

$ErrorActionPreference = 'Stop'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'uninstall.ps1 must be run as Administrator.'
    }
}

Assert-Admin

# ---- service ------------------------------------------------------------

$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc) {
    if ($svc.Status -ne 'Stopped') {
        Write-Host "Stopping $ServiceName..."
        Stop-Service -Name $ServiceName -Force
        Start-Sleep -Seconds 1
    }
    Write-Host "Removing $ServiceName service..."
    & sc.exe delete $ServiceName | Out-Null
}

# ---- firewall -----------------------------------------------------------

foreach ($port in 500, 4500) {
    $name = "dhc-vpn IKE UDP/$port"
    if (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue) {
        Write-Host "Removing firewall rule: $name"
        Remove-NetFirewallRule -DisplayName $name | Out-Null
    }
}

$tunRule = 'dhc-vpn allow inbound on wintun'
if (Get-NetFirewallRule -DisplayName $tunRule -ErrorAction SilentlyContinue) {
    Write-Host "Removing firewall rule: $tunRule"
    Remove-NetFirewallRule -DisplayName $tunRule | Out-Null
}

# ---- Start Menu shortcut ------------------------------------------------

$lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\dhc-vpn.lnk'
if (Test-Path $lnk) {
    Write-Host "Removing Start-Menu shortcut..."
    Remove-Item -Force $lnk
}

# ---- program files ------------------------------------------------------

if (Test-Path $InstallDir) {
    Write-Host "Removing $InstallDir..."
    Remove-Item -Recurse -Force $InstallDir
}

# ---- data (only with -PurgeData) ---------------------------------------

if ($PurgeData -and (Test-Path $DataDir)) {
    Write-Host "Purging $DataDir (certs + profiles + saved passwords) ..."
    Remove-Item -Recurse -Force $DataDir
} elseif (Test-Path $DataDir) {
    Write-Host "Preserved: $DataDir  (use -PurgeData to remove)."
}

Write-Host ''
Write-Host 'Uninstall complete.'
