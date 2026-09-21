param(
    [switch]$Wired,
    [string]$RearPort = 'auto',
    [switch]$SkipDashboard
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$gateway = Join-Path $projectRoot 'gateway\ev_gateway.py'
$gatewayArgs = @('-u', ('"' + $gateway + '"'))
$inputMode = 'WIFI_UDP'
if ($Wired) {
    if ($RearPort -eq 'auto') {
        $ports = @(python -c "import serial.tools.list_ports; print('\n'.join(p.device for p in serial.tools.list_ports.comports() if p.vid == 0x0483))")
        $ports = @($ports | Where-Object { $_ -match '^COM\d+$' })
        if ($ports.Count -ne 1) {
            throw 'Connect Rear ST-Link USB, or run scripts/run_ev_stack.ps1 -Wired -RearPort COM13 (use the actual Rear port).'
        }
        $RearPort = $ports[0]
    }
    if ($RearPort -notmatch '^COM\d+$') { throw 'RearPort must be a COM port, e.g. COM13.' }
    $inputMode = 'STM_USB'
    $gatewayArgs += @('--rear-serial', $RearPort, '--http-host', '127.0.0.1')
}

$statusUrl = 'http://127.0.0.1:8766/api/telemetry'
$existing = $null
try { $existing = Invoke-RestMethod $statusUrl -TimeoutSec 2 } catch {}
$reuse = $existing -and $existing.input_mode -eq $inputMode -and -not $existing.gateway_control_enabled
if ($Wired) { $reuse = $reuse -and $existing.input_port -eq $RearPort }
if (-not $reuse) {
    if ($existing) {
        if ($existing.recording_active) { throw 'A measurement is recording. Stop it in the dashboard before changing inputs.' }
        # Only replace the process that owns this dashboard port, never other Python tools.
        $listener = Get-NetTCPConnection -LocalPort 8766 -State Listen | Select-Object -First 1
        $owner = Get-CimInstance Win32_Process -Filter "ProcessId=$($listener.OwningProcess)"
        if (-not $owner -or $owner.CommandLine -notmatch [regex]::Escape($gateway)) {
            throw 'Port 8766 is owned by another application. Stop it explicitly before launching.'
        }
        Stop-Process -Id $owner.ProcessId
        Wait-Process -Id $owner.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
    }
    Start-Process -FilePath 'python.exe' -WorkingDirectory $projectRoot -WindowStyle Hidden -ArgumentList $gatewayArgs
}

$ready = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    try {
        $status = Invoke-RestMethod $statusUrl -TimeoutSec 1
        $ready = $status.input_mode -eq $inputMode
        if ($Wired) { $ready = $ready -and $status.input_port -eq $RearPort }
        if ($ready -and (-not $Wired -or $status.usb_link_online)) { break }
    } catch {}
    Start-Sleep -Milliseconds 200
}
if (-not $ready) { throw 'EV gateway did not start on port 8766.' }
if ($Wired) {
    Write-Host "Rear STM USB: $RearPort, 115200 baud, read-only; wireless input disabled."
    if ($status.usb_link_online) { Write-Host "Live USB packets: $($status.received_packets), $($status.receive_rate_hz) Hz" }
    else { Write-Warning 'Gateway is running, but no valid Rear USB records have arrived. Check the port/cable.' }
}
if (-not $SkipDashboard) { Start-Process 'http://127.0.0.1:8766/pit' }

Write-Host 'HTML pit dashboard: http://127.0.0.1:8766/pit'
Write-Host 'Use Start measurement in the dashboard to save raw STM records and all received values to SQLite.'
