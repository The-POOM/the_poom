param(
    [string]$Compiler = 'gcc',
    [string]$BuildDirectory = (Join-Path ([System.IO.Path]::GetTempPath()) 'poom-marauder-host-tests')
)
$ErrorActionPreference = 'Stop'
$compilerCommand = Get-Command -Name $Compiler -CommandType Application -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$originalPath = $env:PATH
try {
# MinGW's compiler subprocesses can depend on DLLs alongside gcc.exe even
# when gcc itself was invoked by absolute path. Restore PATH on every exit.
$env:PATH = (Split-Path -Parent $compilerPath) + [System.IO.Path]::PathSeparator + $originalPath
$moduleDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../modules/poom_wifi_attacks'))
$null = New-Item -ItemType Directory -Force -Path $BuildDirectory
$common = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', "-I$moduleDirectory")
$framesSource = Join-Path $moduleDirectory 'poom_marauder_frames.c'
$frameExecutable = Join-Path $BuildDirectory 'test_marauder_frames.exe'
$frameArguments = $common + @((Join-Path $PSScriptRoot 'test_marauder_frames.c'), $framesSource, '-o', $frameExecutable)
& $compilerPath @frameArguments
if ($LASTEXITCODE -ne 0) { throw "Frame test compilation failed: $LASTEXITCODE" }
& $frameExecutable
if ($LASTEXITCODE -ne 0) { throw "Frame tests failed: $LASTEXITCODE" }

$lifecycleExecutable = Join-Path $BuildDirectory 'test_marauder_lifecycle.exe'
$lifecycleArguments = $common + @(
    '-D_WIN32_WINNT=0x0601',
    "-I$(Join-Path $PSScriptRoot 'mocks')", "-I$(Join-Path $moduleDirectory 'include')",
    (Join-Path $PSScriptRoot 'test_marauder_lifecycle.c'),
    (Join-Path $moduleDirectory 'poom_wifi_marauder.c'), $framesSource,
    '-o', $lifecycleExecutable
)
& $compilerPath @lifecycleArguments
if ($LASTEXITCODE -ne 0) { throw "Lifecycle test compilation failed: $LASTEXITCODE" }
& $lifecycleExecutable
if ($LASTEXITCODE -ne 0) { throw "Lifecycle tests failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $originalPath
}
