param(
    [switch]$ReplaceExisting,
    [string]$PublicUrl = ''
)

$ErrorActionPreference = "Stop"

$health = "http://127.0.0.1:8766/health"
try {
    $response = Invoke-RestMethod -Uri $health -TimeoutSec 3
    if (-not $response.ok) { throw "gateway health response was not OK" }
} catch {
    throw "Start the DJY EV gateway on port 8766 before ngrok."
}

$ngrok = Get-Command ngrok -ErrorAction SilentlyContinue
if (-not $ngrok) { throw "ngrok is not installed or not on PATH." }
$requestedScheme = if ($PublicUrl -match '^(https?)://') { $Matches[1] } else { '' }

try {
    $existing = Invoke-RestMethod -Uri "http://127.0.0.1:4040/api/tunnels" -TimeoutSec 2
    $evTunnel = $existing.tunnels | Where-Object {
        $_.config.addr -match '(:|localhost)8766$' -and
        (-not $PublicUrl -or $_.public_url.TrimEnd('/') -eq $PublicUrl.TrimEnd('/'))
    } | Select-Object -First 1
    if ($evTunnel) {
        Write-Host "Vehicle exchange: $($evTunnel.public_url)/api/vehicle/exchange"
        exit 0
    }
    if ($existing.tunnels) {
        $targets = ($existing.tunnels | ForEach-Object { $_.config.addr }) -join ", "
        if (-not $ReplaceExisting) {
            throw "Another ngrok tunnel is already active ($targets). Use -ReplaceExisting to switch it to EV."
        }
        Get-Process -Name ngrok -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
    }
} catch {
    if ($_.Exception.Message -like 'Another ngrok tunnel*') { throw }
}

$ngrokArgs = @('http', '8766')
if ($PublicUrl) { $ngrokArgs += @('--url', $PublicUrl) }
$proxyNames = @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'GIT_HTTP_PROXY', 'GIT_HTTPS_PROXY')
$savedProxy = @{}
foreach ($name in $proxyNames) {
    $savedProxy[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
try {
    Start-Process -FilePath $ngrok.Source -ArgumentList $ngrokArgs -WindowStyle Hidden
} finally {
    foreach ($name in $proxyNames) {
        [Environment]::SetEnvironmentVariable($name, $savedProxy[$name], 'Process')
    }
}
$public = $null
for ($attempt = 0; $attempt -lt 30 -and -not $public; $attempt++) {
    Start-Sleep -Milliseconds 500
    foreach ($apiPort in 4040,4041,4042) {
        try {
            $tunnels = Invoke-RestMethod -Uri "http://127.0.0.1:$apiPort/api/tunnels" -TimeoutSec 1
            $public = $tunnels.tunnels | Where-Object {
                $_.config.addr -match '(:|localhost)8766$' -and
                (-not $PublicUrl -or $_.public_url.TrimEnd('/') -eq $PublicUrl.TrimEnd('/'))
            } | Select-Object -First 1
            if ($public) { break }
        } catch {}
    }
}
if (-not $public) { throw "ngrok started, but no requested tunnel for port 8766 was found." }
Write-Host "Vehicle exchange: $($public.public_url)/api/vehicle/exchange"
