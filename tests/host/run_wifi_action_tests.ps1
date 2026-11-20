param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$outputDir = Join-Path $sourceRoot 'build\host-wifi-action'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
    $program = Join-Path $outputDir 'test_wifi_action.exe'
    & $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' `
        '-I' (Join-Path $PSScriptRoot 'action_stubs') `
        '-I' (Join-Path $sourceRoot 'applications\poom_app_pack\include') `
        '-I' (Join-Path $sourceRoot 'applications\poom_wifi_detector\include') `
        '-I' (Join-Path $sourceRoot 'modules\poom_wifi_attacks\include') `
        '-I' (Join-Path $sourceRoot 'modules\poom_wifi_scanner\include') `
        (Join-Path $PSScriptRoot 'test_wifi_action.c') '-o' $program
    if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi action UI test compilation failed' }
    & $program
    if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi action UI tests failed' }
} finally {
    $env:PATH = $savedPath
}
