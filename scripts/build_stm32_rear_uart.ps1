param(
    [string]$SourceRepository = (Join-Path $PSScriptRoot '..\..\DJY_TQV'),
    [switch]$LegacyPinned,
    [switch]$BinaryRear
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($LegacyPinned -and $BinaryRear) { throw 'Choose LegacyPinned or BinaryRear, not both' }

$evRoot = Split-Path -Parent $PSScriptRoot
$revision = 'ee9d295bae733b27dc93f7dd4871d38d284e9732'
$artifacts = Join-Path $evRoot $(if ($BinaryRear) { '.local\stm32-rear-binary' } elseif ($LegacyPinned) { '.local\stm32-rear-uart' } else { '.local\tv-stm-esp' })
$repoRoot = Join-Path ([IO.Path]::GetTempPath()) ("DJY_EvTLMT-stm32-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $repoRoot | Out-Null
New-Item -ItemType Directory -Force -Path $artifacts | Out-Null
$archive = Join-Path $repoRoot 'base.tar'
if ($LegacyPinned) {
# Export committed source only. Never build unrelated dirty vehicle-control edits.
& git -C $SourceRepository archive --format=tar -o $archive $revision firmware/rear shared/include
if ($LASTEXITCODE -ne 0) { throw 'Cannot export pinned STM32 source revision' }
& tar -xf $archive -C $repoRoot
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract pinned STM32 source' }
$overlay = Join-Path $evRoot 'firmware\stm32_rear_uart\Core'
Copy-Item -Path (Join-Path $overlay 'Inc\*') -Destination (Join-Path $repoRoot 'firmware\rear\Core\Inc') -Force
Copy-Item -Path (Join-Path $overlay 'Src\*') -Destination (Join-Path $repoRoot 'firmware\rear\Core\Src') -Force
} else {
    $firmware = Join-Path $repoRoot 'firmware'
    New-Item -ItemType Directory -Force -Path $firmware | Out-Null
    $frontProject = if ($BinaryRear) { 'firmware\stm32_front_synced' } else { 'firmware\tv_stm_esp\stm_front' }
    Copy-Item -LiteralPath (Join-Path $evRoot $frontProject) -Destination (Join-Path $firmware 'front') -Recurse
    $rearProject = if ($BinaryRear) { 'firmware\stm32_rear_binary' } else { 'firmware\tv_stm_esp\stm_back' }
    Copy-Item -LiteralPath (Join-Path $evRoot $rearProject) -Destination (Join-Path $firmware 'rear') -Recurse
}
$toolBin = Join-Path $env:USERPROFILE '.platformio\packages\toolchain-gccarmnoneeabi\bin'
$gcc = Join-Path $toolBin 'arm-none-eabi-gcc.exe'
$objcopy = Join-Path $toolBin 'arm-none-eabi-objcopy.exe'
$sizeTool = Join-Path $toolBin 'arm-none-eabi-size.exe'

foreach ($tool in $gcc, $objcopy, $sizeTool) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "ARM toolchain not found: $tool`nInstall it with: pio pkg install --global --platform ststm32"
    }
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath"
    }
}

function Build-Target {
    param([ValidateSet('front', 'rear')][string]$Name)

    $root = Join-Path $repoRoot "firmware\$Name"
    $rootPrefix = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\') + '\'
    $output = Join-Path $root 'Release'
    New-Item -ItemType Directory -Force -Path $output | Out-Null

    $includeDirs = @(
        (Join-Path $root 'Core\Inc'),
        (Join-Path $root 'Drivers\STM32F4xx_HAL_Driver\Inc'),
        (Join-Path $root 'Drivers\STM32F4xx_HAL_Driver\Inc\Legacy'),
        (Join-Path $root 'Drivers\CMSIS\Device\ST\STM32F4xx\Include'),
        (Join-Path $root 'Drivers\CMSIS\Include'),
        (Join-Path $repoRoot 'shared\include')
    )
    if ($Name -eq 'rear') {
        $includeDirs += @(
            (Join-Path $root 'FATFS\Target'),
            (Join-Path $root 'FATFS\App'),
            (Join-Path $root 'Middlewares\Third_Party\FatFs\src')
        )
    }

    $includeFlags = foreach ($dir in $includeDirs) { '-I'; $dir }
    $commonFlags = @(
        '-mcpu=cortex-m4', '-mthumb', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard',
        '-DUSE_HAL_DRIVER', '-DSTM32F446xx', '-O2', '-ffunction-sections',
        '-fdata-sections', '-Wall', '-Wextra'
    )

    $sources = @()
    $sources += Get-ChildItem -LiteralPath (Join-Path $root 'Core\Src') -Filter '*.c' -File
    $sources += Get-ChildItem -LiteralPath (Join-Path $root 'Drivers\STM32F4xx_HAL_Driver\Src') -Filter '*.c' -File
    if ($Name -eq 'rear') {
        $sources += Get-ChildItem -LiteralPath (Join-Path $root 'FATFS\App') -Filter '*.c' -File
        $sources += Get-ChildItem -LiteralPath (Join-Path $root 'FATFS\Target') -Filter '*.c' -File
        $sources += Get-ChildItem -LiteralPath (Join-Path $root 'Middlewares\Third_Party\FatFs\src') -Filter '*.c' -File
    }

    $objects = @()
    foreach ($source in $sources) {
        $relative = $source.FullName.Substring($rootPrefix.Length)
        $objectName = ($relative -replace '[\\/:.]', '_') + '.o'
        $object = Join-Path $output $objectName
        Invoke-Checked $gcc ($commonFlags + @('-std=gnu11') + $includeFlags +
            @('-c', $source.FullName, '-o', $object))
        $objects += $object
    }

    foreach ($source in Get-ChildItem -LiteralPath (Join-Path $root 'Core\Startup') -Filter '*.s' -File) {
        $object = Join-Path $output ($source.BaseName + '_startup.o')
        Invoke-Checked $gcc ($commonFlags + $includeFlags +
            @('-x', 'assembler-with-cpp', '-c', $source.FullName, '-o', $object))
        $objects += $object
    }

    $elf = Join-Path $output "stm_$Name.elf"
    $map = Join-Path $output "stm_$Name.map"
    $linkerSource = Join-Path $root 'STM32F446RETX_FLASH.ld'
    $linker = Join-Path $output 'STM32F446RETX_FLASH.compat.ld'
    $linkerText = [IO.File]::ReadAllText($linkerSource) -replace ' \(READONLY\)', ''
    [IO.File]::WriteAllText($linker, $linkerText, [Text.UTF8Encoding]::new($false))
    $linkFlags = @(
        '-mcpu=cortex-m4', '-mthumb', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard',
        "-T$linker", '--specs=nano.specs', '--specs=nosys.specs',
        '-Wl,--gc-sections', "-Wl,-Map=$map", '-Wl,--start-group', '-lc', '-lm',
        '-Wl,--end-group', '-o', $elf
    )
    Invoke-Checked $gcc ($objects + $linkFlags)

    $bin = Join-Path $output "stm_$Name.bin"
    $hex = Join-Path $output "stm_$Name.hex"
    Invoke-Checked $objcopy @('-O', 'binary', $elf, $bin)
    Invoke-Checked $objcopy @('-O', 'ihex', $elf, $hex)
    Invoke-Checked $sizeTool @($elf)
    Write-Output "Built ${Name}: $bin"
}

$targets = if ($LegacyPinned) { @('rear') } else { @('front', 'rear') }
foreach ($target in $targets) {
    Build-Target $target
    foreach ($extension in 'elf', 'bin', 'hex', 'map') {
        Copy-Item -LiteralPath (Join-Path $repoRoot "firmware\$target\Release\stm_$target.$extension") -Destination $artifacts -Force
    }
}
Write-Output "Candidate only, NOT flashed: $artifacts"
