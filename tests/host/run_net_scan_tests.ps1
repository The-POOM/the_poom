param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$outputDir = Join-Path $sourceRoot 'build\host-net-scan'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
    $program = Join-Path $outputDir 'test_net_scan.exe'
    & $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' `
        '-I' (Join-Path $PSScriptRoot 'net_stubs') `
        '-I' (Join-Path $sourceRoot 'applications\poom_wifi_arp\include') `
        (Join-Path $sourceRoot 'applications\poom_wifi_arp\poom_wifi_net_scan.c') `
        (Join-Path $PSScriptRoot 'test_net_scan.c') '-o' $program
    if ($LASTEXITCODE -ne 0) { throw 'Network scan test compilation failed' }
    & $program
    if ($LASTEXITCODE -ne 0) { throw 'Network scan tests failed' }
} finally {
    $env:PATH = $savedPath
}
