$ErrorActionPreference = "Stop"
$containerName = "ev-ergmt-viewer"
$image = "ghcr.io/luftaquila/fsk-energymeter:latest"
$caddyfile = (Resolve-Path (Join-Path $PSScriptRoot "docker\Caddyfile")).Path

docker pull $image
docker rm -f $containerName 2>$null
docker run -d `
    --name $containerName `
    -p 9800:9800 `
    --restart unless-stopped `
    --mount "type=bind,source=$caddyfile,target=/etc/caddy/Caddyfile,readonly" `
    $image

Write-Host "Viewer: http://localhost:9800"
