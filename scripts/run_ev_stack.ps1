$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$gateway = Join-Path $projectRoot 'gateway\ev_gateway.py'
$dashboard = Join-Path $projectRoot 'run_dashboard.bat'
$gatewayArgs = @($gateway)

Start-Process -FilePath 'python.exe' -WorkingDirectory $projectRoot -WindowStyle Minimized -ArgumentList $gatewayArgs
Start-Sleep -Milliseconds 700
Start-Process -FilePath $dashboard -WorkingDirectory $projectRoot -ArgumentList 'ev'

Write-Host 'Steering display: http://127.0.0.1:8766/steering'
Write-Host 'Pit dashboard: EV mode, UDP 9004'
