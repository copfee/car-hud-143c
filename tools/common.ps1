Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$ArgumentList
    )

    # Windows PowerShell 5.x turns native stderr into ErrorRecord objects. Keep
    # stderr visible, but let the native exit code decide whether the command
    # failed; otherwise harmless tool warnings can stop the script prematurely.
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $FilePath @ArgumentList
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -ne 0) {
        throw "命令执行失败（退出码 $exitCode）：$FilePath $($ArgumentList -join ' ')"
    }
}

if ($env:HUD_PYTHON) {
    $Python = $env:HUD_PYTHON
}
else {
    $sharedPython = [System.IO.Path]::GetFullPath((Join-Path $Root '..\..\work\firmware-env\Scripts\python.exe'))
    $localPython = Join-Path $Root '.tools\venv\Scripts\python.exe'

    if (Test-Path -LiteralPath $sharedPython -PathType Leaf) {
        $Python = $sharedPython
    }
    else {
        $Python = $localPython
        if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
            $venv = Join-Path $Root '.tools\venv'
            $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
            if ($pythonCommand) {
                Invoke-Checked $pythonCommand.Source '-m' 'venv' $venv
            }
            else {
                $pyCommand = Get-Command py -ErrorAction SilentlyContinue
                if (-not $pyCommand) {
                    throw '未找到 Python。请先安装 Python 3.10 或更高版本，并将 python 或 py 加入 PATH。'
                }
                Invoke-Checked $pyCommand.Source '-3' '-m' 'venv' $venv
            }
        }
    }
}

& $Python -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
if ($LASTEXITCODE -ne 0) {
    throw '需要 Python 3.10 或更高版本。'
}

$packageVersionCheck = @'
from importlib.metadata import version
try:
    valid = version('esptool') == '4.8.1' and version('platformio') == '6.1.18'
except Exception:
    valid = False
raise SystemExit(0 if valid else 1)
'@
& $Python -c $packageVersionCheck
if ($LASTEXITCODE -ne 0) {
    Invoke-Checked $Python '-m' 'pip' 'install' 'esptool==4.8.1' 'platformio==6.1.18'
}

if (-not $env:PLATFORMIO_CORE_DIR) {
    $sharedPlatformIo = [System.IO.Path]::GetFullPath((Join-Path $Root '..\..\work\platformio'))
    if (Test-Path -LiteralPath $sharedPlatformIo -PathType Container) {
        $env:PLATFORMIO_CORE_DIR = $sharedPlatformIo
    }
    else {
        $env:PLATFORMIO_CORE_DIR = Join-Path $Root '.tools\platformio'
    }
}
