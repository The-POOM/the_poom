param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$outputDir = Join-Path $sourceRoot 'build\host-ble-recon'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
    $program = Join-Path $outputDir 'test_ble_recon.exe'
    & $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' `
        '-I' (Join-Path $PSScriptRoot 'ble_stubs') `
        '-I' (Join-Path $sourceRoot 'applications\poom_ble_detector\include') `
        '-I' (Join-Path $sourceRoot 'modules\poom_ble_scan\include') `
        (Join-Path $sourceRoot 'applications\poom_ble_detector\poom_ble_detector.c') `
        (Join-Path $PSScriptRoot 'test_ble_recon.c') '-lm' '-o' $program
    if ($LASTEXITCODE -ne 0) { throw 'BLE recon compilation failed' }
    & $program
    if ($LASTEXITCODE -ne 0) { throw 'BLE recon tests failed' }
} finally {
    $env:PATH = $savedPath
}
