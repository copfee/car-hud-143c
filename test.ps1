[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\common.ps1')

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw '未找到 CMake。请安装 CMake 3.19+ 与 Visual Studio 2022 C++ Build Tools，并将 cmake 加入 PATH。'
}
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
    throw '未找到 CTest。请确认 CMake 的 bin 目录已加入 PATH。'
}

Invoke-Checked $Python (Join-Path $Root 'tools\generate_profiles.py')
$native = Join-Path $Root '.tools\native'
Invoke-Checked 'cmake' '-S' (Join-Path $Root 'tests') '-B' $native '-DCMAKE_BUILD_TYPE=Debug' "-DPython3_EXECUTABLE=$Python"
Invoke-Checked 'cmake' '--build' $native '--config' 'Debug' '--parallel' '8'
Invoke-Checked 'ctest' '--test-dir' $native '-C' 'Debug' '--output-on-failure'
Invoke-Checked $Python '-m' 'unittest' 'discover' '-s' (Join-Path $Root 'tests') '-p' 'test_*.py'
