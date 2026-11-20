param([string]$Compiler = 'gcc')

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$outputDir = Join-Path $sourceRoot 'build\host-passive-wifi'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
$savedPath = $env:PATH
try {
$env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH

$common = @('-std=c99', '-Wall', '-Wextra', '-Werror')
$saeTest = Join-Path $outputDir 'test_sae_filter.exe'
& $Compiler @common '-I' (Join-Path $sourceRoot 'modules\poom_pcap') `
    (Join-Path $PSScriptRoot 'test_sae_filter.c') '-o' $saeTest
if ($LASTEXITCODE -ne 0) { throw 'SAE filter test compilation failed' }
& $saeTest
if ($LASTEXITCODE -ne 0) { throw 'SAE filter tests failed' }

$scanTest = Join-Path $outputDir 'test_wifi_scan_cache.exe'
& $Compiler @common '-DPOOM_WIFI_SCANNER_ENABLE_LOG=0' `
    '-I' (Join-Path $PSScriptRoot 'stubs') `
    '-I' (Join-Path $sourceRoot 'modules\poom_wifi_scanner\include') `
    (Join-Path $sourceRoot 'modules\poom_wifi_scanner\poom_wifi_scanner.c') `
    (Join-Path $PSScriptRoot 'test_wifi_scan_cache.c') '-o' $scanTest
if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi scan cache test compilation failed' }
& $scanTest
if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi scan cache tests failed' }
} finally {
    $env:PATH = $savedPath
}
