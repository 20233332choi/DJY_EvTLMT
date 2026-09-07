param([ValidateRange(0, 7)][int]$NetworkIndex = 0)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$evConfig = Join-Path $projectRoot 'firmware\esp32_ev_gateway\include\config.h'
$bajaConfig = 'C:\Users\user\Desktop\OHTERS\BajaTlmtProt\firmware\esp32-s3-rapidbike\micropython\config\wifi_config.py'

if (-not (Test-Path -LiteralPath $evConfig)) { throw 'EV ESP config.h is missing.' }
if (-not (Test-Path -LiteralPath $bajaConfig)) { throw 'Baja Wi-Fi config is missing.' }

$bajaText = [IO.File]::ReadAllText($bajaConfig)
$networkBlock = [regex]::Match($bajaText, '(?s)WIFI_STA_NETWORKS\s*=\s*\((.*?)\)\s*(?:#.*)?$')
if (-not $networkBlock.Success) { throw 'Could not parse WIFI_STA_NETWORKS from Baja Wi-Fi config.' }

$pairPattern = '[\(\[]\s*["'']((?:\\.|[^"''])*)["'']\s*,\s*["'']((?:\\.|[^"''])*)["'']\s*[\)\]]'
$networks = [regex]::Matches($networkBlock.Groups[1].Value, $pairPattern)
if ($networks.Count -le $NetworkIndex) { throw "Baja Wi-Fi network index $NetworkIndex is unavailable." }

function Unescape-PythonString([string]$value) {
    return [regex]::Replace($value, '\\([\\"''])', '$1')
}
function Escape-CppString([string]$value) {
    return $value.Replace('\', '\\').Replace('"', '\"')
}
function Replace-StringDefine([string]$text, [string]$name, [string]$value) {
    $pattern = '(?m)^(\s*#define\s+{0}\s+")[^"]*(".*)$' -f [regex]::Escape($name)
    $replacement = '$1' + (Escape-CppString $value) + '$2'
    $result = [regex]::Replace($text, $pattern, $replacement, 1)
    if ($result -eq $text -and -not [regex]::IsMatch($text, $pattern)) { throw "$name is missing from EV config.h." }
    return $result
}

$ssid = Unescape-PythonString $networks[$NetworkIndex].Groups[1].Value
$password = Unescape-PythonString $networks[$NetworkIndex].Groups[2].Value
if ($ssid.Length -eq 0 -or $password.Length -eq 0) { throw 'Selected Baja Wi-Fi entry is empty.' }

$evText = [IO.File]::ReadAllText($evConfig)
$evText = Replace-StringDefine $evText 'EV_WIFI_SSID' $ssid
$evText = Replace-StringDefine $evText 'EV_WIFI_PASSWORD' $password
[IO.File]::WriteAllText($evConfig, $evText, [System.Text.UTF8Encoding]::new($false))

Write-Host "Copied Baja Wi-Fi entry $NetworkIndex to ignored EV config.h (SSID/password not shown)."
