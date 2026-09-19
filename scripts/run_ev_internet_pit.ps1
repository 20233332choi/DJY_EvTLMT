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
    # The firmware applies the same upgrade before opening its socket. Keep an
    # old stable ngrok hostname usable without enabling plaintext transport.
    $relayUrl = 'https://' + $relayUrl.Substring(7)
    Write-Host 'Legacy relay URL upgraded to verified HTTPS.'
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
        -WindowStyle Hidden -ArgumentList $gatewayArgs
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

# The ESP may use a plaintext HTTP endpoint on the bench, while phone browser
# geolocation requires a secure HTTPS origin. Keep both schemes on the same
# ngrok development domain and forward them to the same read-only gateway.
$relayUri = [uri]$relayBaseUrl
$phoneBaseUrl = "https://$($relayUri.Host)"
$phoneTunnel = $null
foreach ($apiPort in 4040,4041,4042) {
    try {
        $agent = Invoke-RestMethod -Uri "http://127.0.0.1:$apiPort/api/tunnels" -TimeoutSec 1
        $phoneTunnel = $agent.tunnels |
            Where-Object { $_.public_url.TrimEnd('/') -eq $phoneBaseUrl.TrimEnd('/') -and $_.config.addr -match '(:|localhost)8766$' } |
            Select-Object -First 1
        if ($phoneTunnel) { break }
    } catch {}
}
if (-not $phoneTunnel) {
    $ngrok = Get-Command ngrok -ErrorAction Stop
    Start-Process -FilePath $ngrok.Source -WindowStyle Hidden `
        -ArgumentList @('http', '8766', '--url', $phoneBaseUrl)
    for ($attempt = 0; $attempt -lt 20 -and -not $phoneTunnel; $attempt++) {
        Start-Sleep -Milliseconds 300
        foreach ($apiPort in 4040,4041,4042) {
            try {
                $agent = Invoke-RestMethod -Uri "http://127.0.0.1:$apiPort/api/tunnels" -TimeoutSec 1
                $phoneTunnel = $agent.tunnels |
                    Where-Object { $_.public_url.TrimEnd('/') -eq $phoneBaseUrl.TrimEnd('/') -and $_.config.addr -match '(:|localhost)8766$' } |
                    Select-Object -First 1
                if ($phoneTunnel) { break }
            } catch {}
        }
    }
}
if ($phoneTunnel) {
    Write-Host "Phone GNSS: $phoneBaseUrl/phone"
} else {
    Write-Warning "Phone GNSS HTTPS endpoint did not start: $phoneBaseUrl/phone"
}

if (-not $SkipDashboard) {
    Start-Process 'http://127.0.0.1:8766/pit'
}
Write-Host 'HTML pit dashboard: http://127.0.0.1:8766/pit'
$controlStatus = if ($EnableControl) { 'Pit control backend: ENABLED; use run_dashboard.bat native for existing deadman controls' } else { 'Pit control: READ ONLY' }
Write-Host $controlStatus
