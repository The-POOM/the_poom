param([string]$Compiler = 'g++', [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $env:TEMP 'poom-csi-tests' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$savedPath = $env:PATH
try {
    $compilerPath = (Get-Command $Compiler).Source
    $env:PATH = "$(Split-Path $compilerPath);$savedPath"
    foreach($test in @('test_wifi_csi','test_csi_lifecycle')) {
        $exe = Join-Path $OutputDirectory "$test.exe"
        $sources = @((Join-Path $PSScriptRoot "$test.cpp"), (Join-Path $repoRoot 'modules\poom_wifi_csi\CsiMotionModel.cpp'))
        if ($test -eq 'test_csi_lifecycle') { $sources += Join-Path $repoRoot 'modules\poom_wifi_csi\poom_wifi_csi.cpp' }
        & $Compiler -std=c++17 -Wall -Wextra -Werror -Wno-misleading-indentation -O1 `
            "-I$PSScriptRoot\observer_stubs" "-I$repoRoot\modules\poom_wifi_csi\include" @sources -lm -o $exe
        if ($LASTEXITCODE -ne 0) { throw "$test compilation failed" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "$test failed" }
    }
} finally { $env:PATH = $savedPath }
