param(
    [switch]$EnableControl,
    [switch]$SkipDashboard
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$gateway = Join-Path $projectRoot 'gateway\ev_gateway.py'
$dashboard = Join-Path $projectRoot 'run_dashboard.bat'
$config = Join-Path $projectRoot 'firmware\esp32_ev_gateway\include\config.h'
$ngrokScript = Join-Path $PSScriptRoot 'start_ev_ngrok.ps1'

if (-not (Test-Path -LiteralPath $config)) {
    throw 'ESP config.h is missing. Copy config.example.h and add the private relay settings.'
}

$configText = [IO.File]::ReadAllText($config)
function Get-StringDefine([string]$Name) {
    $pattern = '(?m)^\s*#define\s+{0}\s+"([^"]*)"' -f [regex]::Escape($Name)
    $match = [regex]::Match($configText, $pattern)
    if ($match.Success) { return $match.Groups[1].Value }
    return ''
}
function Get-IntegerDefine([string]$Name) {
    $pattern = '(?m)^\s*#define\s+{0}\s+(\d+)' -f [regex]::Escape($Name)
    $match = [regex]::Match($configText, $pattern)
    if ($match.Success) { return [int]$match.Groups[1].Value }
    return 0
}

$relayUrl = Get-StringDefine 'EV_RELAY_URL'
$relayToken = if ($env:DJY_EV_RELAY_TOKEN) {
    $env:DJY_EV_RELAY_TOKEN
} else {
    Get-StringDefine 'EV_RELAY_TOKEN'
}

if ((Get-IntegerDefine 'EV_RELAY_ENABLED') -ne 1) {
    throw 'Set EV_RELAY_ENABLED 1 in the ignored ESP config.h, rebuild, and flash the ESP32.'
}
if ($relayUrl -notmatch '^https?://.+/api/vehicle/exchange$') {
    throw 'EV_RELAY_URL must use the EV ngrok URL followed by /api/vehicle/exchange.'
}
if ($relayUrl -like 'http://*' -and (Get-IntegerDefine 'EV_ALLOW_PLAINTEXT_RELAY') -ne 1) {
    throw 'Plain HTTP relay requires EV_ALLOW_PLAINTEXT_RELAY 1 in the ignored ESP config.h.'
}
if ($relayToken.Length -lt 16) {
    throw 'Configure the same private relay token in ESP config.h and the pit environment.'
}
if ($EnableControl -and (Get-IntegerDefine 'EV_RELAY_ACCEPT_COMMANDS') -ne 1) {
    throw 'Remote commands were requested, but EV_RELAY_ACCEPT_COMMANDS is not 1 in ESP config.h.'
}
if ($EnableControl -and $relayUrl -like 'http://*') {
    throw 'Remote vehicle commands over plain HTTP are refused. Use verified HTTPS or start read-only telemetry.'
}

# One EV gateway owns HTTP 8766, command UDP 9005, and pit-forward UDP 9004.
Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'DJY_EvTLMT.+gateway[\\/]ev_gateway\.py|gateway[\\/]ev_gateway\.py' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

$oldToken = $env:DJY_EV_RELAY_TOKEN
$env:DJY_EV_RELAY_TOKEN = $relayToken
try {
    $gatewayArgs = @($gateway, '--http-host', '127.0.0.1')
    if ($EnableControl) { $gatewayArgs += '--enable-control' }
    Start-Process -FilePath 'python.exe' -WorkingDirectory $projectRoot `
        -WindowStyle Minimized -ArgumentList $gatewayArgs
} finally {
    $env:DJY_EV_RELAY_TOKEN = $oldToken
}

$ready = $false
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    Start-Sleep -Milliseconds 200
    try {
        $telemetry = Invoke-RestMethod -Uri 'http://127.0.0.1:8766/api/telemetry' -TimeoutSec 1
        if ($telemetry.relay_server_enabled) { $ready = $true; break }
    } catch {}
}
if (-not $ready) { throw 'EV gateway did not start with the relay token on localhost:8766.' }

$relayBaseUrl = $relayUrl -replace '/api/vehicle/exchange$', ''
& $ngrokScript -ReplaceExisting -PublicUrl $relayBaseUrl

try {
    $tunnels = Invoke-RestMethod -Uri 'http://127.0.0.1:4040/api/tunnels' -TimeoutSec 2
    $relayScheme = ([uri]$relayUrl).Scheme
    $evTunnel = $tunnels.tunnels |
        Where-Object { $_.public_url -like "${relayScheme}://*" -and $_.config.addr -match '(:|localhost)8766$' } |
        Select-Object -First 1
    if ($evTunnel) {
        $publicExchange = "$($evTunnel.public_url)/api/vehicle/exchange"
        if ($relayUrl.TrimEnd('/') -ne $publicExchange.TrimEnd('/')) {
            throw "ESP EV_RELAY_URL does not match the active ngrok endpoint. Update config.h and reflash: $publicExchange"
        }
        Write-Host "Vehicle exchange: $publicExchange"
    } else {
        Write-Warning 'No matching ngrok tunnel to EV port 8766 is active.'
    }
} catch {
    Write-Warning 'ngrok status API is unavailable. Start the EV 8766 tunnel.'
}

if (-not $SkipDashboard) {
    Get-Process -Name 'DJY_EvTLMT' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Join-Path $projectRoot 'bin\DJY_EvTLMT.exe') } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Process -FilePath $dashboard -WorkingDirectory $projectRoot -ArgumentList 'ev'
}
Write-Host 'Pit CMake telemetry: UDP 9004'
$controlStatus = if ($EnableControl) { 'Pit control: ENABLED with CMake deadman' } else { 'Pit control: READ ONLY' }
Write-Host $controlStatus
