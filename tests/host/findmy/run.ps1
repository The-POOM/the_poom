param(
    [string]$Compiler = 'gcc',
    [string]$BuildDirectory = (Join-Path ([System.IO.Path]::GetTempPath()) 'poom-findmy-host-tests')
)
$ErrorActionPreference = 'Stop'
$compilerPath = (Get-Command -Name $Compiler -CommandType Application -ErrorAction Stop).Source
$originalPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $compilerPath) + [System.IO.Path]::PathSeparator + $originalPath
    $moduleDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../modules/poom_findmy_beep'))
    $null = New-Item -ItemType Directory -Force -Path $BuildDirectory
    $common = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', "-I$moduleDirectory/include")
    foreach ($test in @('protocol', 'backend')) {
        $executable = Join-Path $BuildDirectory "test_findmy_$test.exe"
        $arguments = $common + @("-I$(Join-Path $PSScriptRoot 'mocks')", (Join-Path $PSScriptRoot "test_$test.c"),
            (Join-Path $moduleDirectory 'poom_findmy_protocol.c'), '-o', $executable)
        & $compilerPath @arguments
        if ($LASTEXITCODE -ne 0) { throw "$test test compilation failed: $LASTEXITCODE" }
        & $executable
        if ($LASTEXITCODE -ne 0) { throw "$test tests failed: $LASTEXITCODE" }
    }
} finally {
    $env:PATH = $originalPath
}
