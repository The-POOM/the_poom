param([string]$Compiler = 'gcc', [string]$CJsonDirectory = '', [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (!$CJsonDirectory) { $CJsonDirectory = Join-Path $repoRoot 'managed_components\espressif__cjson\cJSON' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $env:TEMP 'poom-observer-tests' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$exe = Join-Path $OutputDirectory 'wifi_observer.exe'
& $Compiler -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -O1 -g `
    "-I$repoRoot\modules\poom_wifi_observer\include" "-I$CJsonDirectory" `
    (Join-Path $PSScriptRoot 'test_wifi_observer.c') `
    (Join-Path $repoRoot 'modules\poom_wifi_observer\poom_wifi_observer_model.c') `
    (Join-Path $CJsonDirectory 'cJSON.c') -lm -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Observer host compilation failed' }
$savedPath = $env:PATH
try {
    $compilerPath = (Get-Command $Compiler).Source
    $env:PATH = "$(Split-Path $compilerPath);$savedPath"
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Observer host tests failed' }
    $lifecycle = Join-Path $OutputDirectory 'observer_lifecycle.exe'
    & $Compiler -std=c11 -Wall -Wextra -Werror -O1 `
        "-I$PSScriptRoot\observer_stubs" "-I$repoRoot\modules\poom_wifi_observer\include" "-I$CJsonDirectory" `
        (Join-Path $PSScriptRoot 'test_observer_lifecycle.c') `
        (Join-Path $repoRoot 'modules\poom_wifi_observer\poom_wifi_observer.c') `
        (Join-Path $repoRoot 'modules\poom_wifi_observer\poom_wifi_observer_model.c') `
        (Join-Path $CJsonDirectory 'cJSON.c') -lm -o $lifecycle
    if ($LASTEXITCODE -ne 0) { throw 'Observer lifecycle compilation failed' }
    & $lifecycle
    if ($LASTEXITCODE -ne 0) { throw 'Observer lifecycle tests failed' }
} finally { $env:PATH = $savedPath }
