param(
    [string]$Port = 'COM5',
    [switch]$SkipDashboard
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$gateway = Join-Path $projectRoot 'gateway\ev_gateway.py'
$dashboard = Join-Path $projectRoot 'run_dashboard.bat'

& python.exe -c 'import serial'
if ($LASTEXITCODE -ne 0) {
    throw 'pyserial is required: python -m pip install pyserial'
}

Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'DJY_EvTLMT.+gateway[\\/]ev_gateway\.py|gateway[\\/]ev_gateway\.py' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Start-Process -FilePath 'python.exe' -WorkingDirectory $projectRoot -WindowStyle Minimized `
    -ArgumentList @($gateway, '--http-host', '127.0.0.1', '--bms-serial', $Port)

$ready = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    Start-Sleep -Milliseconds 300
    try {
        $telemetry = Invoke-RestMethod -Uri 'http://127.0.0.1:8766/api/telemetry' -TimeoutSec 1
        if ($telemetry.bms_online) { $ready = $true; break }
    } catch {}
}
if (-not $ready) {
    throw "DALY BMS did not respond on $Port at 9600 bps. Check that no other app owns the port."
}

if (-not $SkipDashboard) {
    Start-Process -FilePath $dashboard -WorkingDirectory $projectRoot -ArgumentList 'ev'
}
Write-Host "DALY BMS: LIVE on $Port at 9600 bps"
Write-Host 'Pit CMake telemetry: UDP 9004'
Write-Host 'Vehicle source: waiting on UDP 9003; future ESP wireless BMS fields use the same dashboard contract.'
