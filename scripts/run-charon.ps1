# Start charon-svc with verbose logging in this terminal.
# Run as Administrator. Ctrl+C stops it.
$repoRoot = Split-Path -Parent $PSScriptRoot
$base     = Join-Path $repoRoot 'build\charon-install'
$env:STRONGSWAN_CONF = "$base\etc\strongswan.conf"
$env:SWANCTL_DIR     = "$base\etc\swanctl"
& "$base\bin\charon-svc.exe"
