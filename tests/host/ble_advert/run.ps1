param(
    [string]$Compiler = 'gcc',
    [string]$BuildDirectory = (Join-Path ([System.IO.Path]::GetTempPath()) 'poom-ble-advert-host-tests')
)
$ErrorActionPreference = 'Stop'
$compilerPath = (Get-Command -Name $Compiler -CommandType Application -ErrorAction Stop).Source
$originalPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $compilerPath) + [System.IO.Path]::PathSeparator + $originalPath
    $moduleDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../applications/poom_ble_spam'))
    $null = New-Item -ItemType Directory -Force -Path $BuildDirectory
    $appIncludes = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../applications/poom_app_pack/include'))
    $common = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', "-I$moduleDirectory/include", "-I$appIncludes")
    foreach ($test in @('payload', 'backend', 'ui')) {
        $executable = Join-Path $BuildDirectory "test_ble_advert_$test.exe"
        $arguments = $common + @("-I$(Join-Path $PSScriptRoot 'mocks')", (Join-Path $PSScriptRoot "test_$test.c"),
            (Join-Path $moduleDirectory 'poom_ble_payload.c'), '-o', $executable)
        & $compilerPath @arguments
        if ($LASTEXITCODE -ne 0) { throw "$test test compilation failed: $LASTEXITCODE" }
        & $executable
        if ($LASTEXITCODE -ne 0) { throw "$test tests failed: $LASTEXITCODE" }
    }
} finally {
    $env:PATH = $originalPath
}
