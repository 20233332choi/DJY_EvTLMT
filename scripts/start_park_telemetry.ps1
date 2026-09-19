param([switch]$OpenDashboard, [switch]$InternetOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolRoot = Join-Path $env:USERPROFILE 'telemetry-tools'
$python = Join-Path $toolRoot 'python\python.exe'
$ngrok = Join-Path $toolRoot 'ngrok\ngrok.exe'
$ngrokConfig = Join-Path $toolRoot 'ngrok\ngrok.yml'
$config = [IO.File]::ReadAllText((Join-Path $root 'firmware\esp32_ev_gateway\include\config.h'))
function ReadDefine([string]$name) {
    $m = [regex]::Match($config, ('(?m)^\s*#define\s+{0}\s+"([^"]+)"' -f $name))
    if (-not $m.Success) { throw "Missing private setting: $name" }
    return $m.Groups[1].Value
}
$relayToken = ReadDefine 'EV_RELAY_TOKEN'
$publicUrl = (ReadDefine 'EV_RELAY_URL') -replace '/api/vehicle/exchange$', ''
if ($publicUrl -notlike 'https://*') { throw 'Verified HTTPS is required.' }
foreach ($path in @($python, $ngrok, $ngrokConfig)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing runtime: $path" }
}
$existing = $null
try { $existing = Invoke-RestMethod 'http://127.0.0.1:8766/api/telemetry' -TimeoutSec 2 } catch {}
if ($null -eq $existing) {
    $saved = $env:DJY_EV_RELAY_TOKEN
    $env:DJY_EV_RELAY_TOKEN = $relayToken
    try {
        $gateway = Join-Path $root 'gateway\ev_gateway.py'
        $gatewayArgs = @('-u', ('"{0}"' -f $gateway), '--http-host', '127.0.0.1', '--enable-control')
        if ($InternetOnly) { $gatewayArgs += @('--listen-host', '127.0.0.1') }
        Start-Process -FilePath $python -WindowStyle Hidden -WorkingDirectory $root `
            -ArgumentList $gatewayArgs `
            -RedirectStandardOutput (Join-Path $toolRoot 'gateway.log') `
            -RedirectStandardError (Join-Path $toolRoot 'gateway-error.log')
    } finally { $env:DJY_EV_RELAY_TOKEN = $saved }
}
$ready = $false
for ($i = 0; $i -lt 20; $i++) {
    try {
        $state = Invoke-RestMethod 'http://127.0.0.1:8766/api/telemetry' -TimeoutSec 1
        if ($state.relay_server_enabled) { $ready = $true; break }
    } catch {}
    Start-Sleep -Milliseconds 250
}
if (-not $ready) { throw 'Gateway did not start with relay authentication; inspect telemetry-tools logs.' }
if (-not $state.gateway_control_enabled) {
    throw 'An existing read-only gateway owns port 8766. Stop it explicitly before starting bidirectional telemetry.'
}
$tunnel = $null
try {
    $agent = Invoke-RestMethod 'http://127.0.0.1:4040/api/tunnels' -TimeoutSec 2
    $tunnel = $agent.tunnels | Where-Object { $_.public_url -eq $publicUrl -and $_.config.addr -match ':8766$' }
    if (-not $tunnel -and $agent.tunnels.Count -gt 0) { throw 'A different ngrok tunnel is running. It was not stopped.' }
} catch { if ($_.Exception.Message -like 'A different*') { throw } }
if (-not $tunnel) {
    Start-Process -FilePath $ngrok -WindowStyle Hidden -ArgumentList @(
        'http', '8766', '--url', $publicUrl, '--config', ('"{0}"' -f $ngrokConfig),
        '--log', ('"{0}"' -f (Join-Path $toolRoot 'ngrok\agent.log')))
}
Write-Host 'PC dashboard: http://127.0.0.1:8766/pit'
Write-Host "Internet dashboard (read-only): $publicUrl/pit"
Write-Host 'Vehicle commands require the local pit controls; public control requests are refused.'
if ($OpenDashboard) { Start-Process 'http://127.0.0.1:8766/pit' }
