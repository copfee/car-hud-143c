[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\common.ps1')

Invoke-Checked $Python (Join-Path $Root 'tools\generate_profiles.py')
Invoke-Checked $Python '-m' 'platformio' 'run' '-d' $Root '-e' 'waveshare_amoled_143c'

$firmware = Join-Path $Root 'firmware'
$build = Join-Path $Root '.pio\build\waveshare_amoled_143c'
$bootApp0 = Join-Path $env:PLATFORMIO_CORE_DIR 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
New-Item -ItemType Directory -Force -Path $firmware | Out-Null

Copy-Item -LiteralPath (Join-Path $build 'firmware.bin') -Destination (Join-Path $firmware 'application.bin') -Force
Copy-Item -LiteralPath (Join-Path $build 'bootloader.bin') -Destination (Join-Path $firmware 'bootloader.bin') -Force
Copy-Item -LiteralPath (Join-Path $build 'partitions.bin') -Destination (Join-Path $firmware 'partitions.bin') -Force
Copy-Item -LiteralPath $bootApp0 -Destination (Join-Path $firmware 'boot_app0.bin') -Force

Invoke-Checked $Python '-m' 'esptool' '--chip' 'esp32s3' 'merge_bin' `
    '--flash_mode' 'keep' '--flash_freq' 'keep' '--flash_size' 'keep' `
    '-o' (Join-Path $firmware 'car-hud-143c-full.bin') `
    '0x0' (Join-Path $firmware 'bootloader.bin') `
    '0x8000' (Join-Path $firmware 'partitions.bin') `
    '0xe000' (Join-Path $firmware 'boot_app0.bin') `
    '0x10000' (Join-Path $firmware 'application.bin')

Invoke-Checked $Python (Join-Path $Root 'tools\manifest.py')
Write-Host "完整固件：$firmware\car-hud-143c-full.bin（烧录地址 0x0）"
