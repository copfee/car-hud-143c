[CmdletBinding()]
param(
    [string]$Port,
    [ValidateRange(1, 3000000)][int]$Baud = 460800,
    [switch]$VerifyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\common.ps1')

Invoke-Checked $Python (Join-Path $Root 'tools\manifest.py') '--verify'
if ($VerifyOnly) {
    exit 0
}

if (-not $Port) {
    $detectPort = @'
from serial.tools import list_ports
ports = [p for p in list_ports.comports() if p.vid is not None]
if len(ports) != 1:
    devices = '\n'.join(p.device for p in ports)
    raise SystemExit('未找到唯一的 USB 串口，请连接开发板，或用 -Port 指定：\n' + devices)
print(ports[0].device)
'@
    $detected = & $Python -c $detectPort
    if ($LASTEXITCODE -ne 0) {
        throw 'USB 串口自动检测失败。'
    }
    $Port = ($detected | Select-Object -Last 1).Trim()
}

Invoke-Checked $Python '-m' 'esptool' '--chip' 'esp32s3' '--port' $Port '--baud' "$Baud" `
    '--before' 'default_reset' '--after' 'hard_reset' 'write_flash' `
    '--flash_mode' 'keep' '--flash_freq' 'keep' '--flash_size' '8MB' `
    '0x0' (Join-Path $Root 'firmware\car-hud-143c-full.bin')
