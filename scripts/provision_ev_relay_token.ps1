param([switch]$Rotate)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$config = Join-Path $projectRoot 'firmware\esp32_ev_gateway\include\config.h'

if (-not (Test-Path -LiteralPath $config)) {
    throw 'ESP config.h is missing. Copy config.example.h first.'
}

$configText = [IO.File]::ReadAllText($config)
$pattern = '(?m)^(\s*#define\s+EV_RELAY_TOKEN\s+")[^"]*(".*)$'
$match = [regex]::Match($configText, $pattern)
if (-not $match.Success) {
    throw 'EV_RELAY_TOKEN is missing from ESP config.h.'
}

$currentToken = [regex]::Match($match.Value, '"([^"]*)"').Groups[1].Value
if ($currentToken.Length -ge 16 -and -not $Rotate) {
    Write-Host 'EV relay token is already configured. No change made.'
    exit 0
}

$bytes = [byte[]]::new(32)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try {
    $rng.GetBytes($bytes)
} finally {
    $rng.Dispose()
}
$token = -join ($bytes | ForEach-Object { $_.ToString('x2') })
$replacement = $match.Groups[1].Value + $token + $match.Groups[2].Value
$updated = [regex]::Replace($configText, $pattern, [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $replacement }, 1)

[IO.File]::WriteAllText($config, $updated, [System.Text.UTF8Encoding]::new($false))
Write-Host 'Generated a private EV relay token in ignored ESP config.h.'
Write-Host 'Rebuild and flash the ESP32 before starting the Internet Pit Dashboard.'
