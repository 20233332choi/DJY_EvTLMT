$ErrorActionPreference = "Stop"
$viewerPath = (Resolve-Path (Join-Path $PSScriptRoot "..\fsk-energymeter\viewer")).Path
Push-Location -LiteralPath $viewerPath

try {
    if (-not (Test-Path -LiteralPath (Join-Path $viewerPath "node_modules"))) {
        Write-Host "Installing the existing FSK-EEM viewer dependencies..."
        npm install
    }

    Write-Host "Viewer: http://localhost:9800"
    Write-Host "Open an EV_ErgMt output or sessions .log file from the Data Viewer tab."
    npm run dev
}
finally {
    Pop-Location
}
