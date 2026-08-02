$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectRoot "build"
$outputPath = Join-Path $projectRoot "bin"
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

Get-Process DJY_EvTLMT -ErrorAction SilentlyContinue | Stop-Process -Force

if (-not (Test-Path -LiteralPath $cmake)) {
    throw "Visual Studio CMake not found: $cmake"
}

New-Item -ItemType Directory -Force -Path $buildPath,$outputPath | Out-Null
& $cmake -S $projectRoot -B $buildPath -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
& $cmake --build $buildPath --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
Copy-Item -LiteralPath (Join-Path $buildPath "Release\DJY_EvTLMT.exe") -Destination (Join-Path $outputPath "DJY_EvTLMT.exe") -Force
Write-Host "Built: $outputPath\DJY_EvTLMT.exe"
