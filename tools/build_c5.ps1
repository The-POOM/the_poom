param(
    [ValidateSet('build', 'reconfigure', 'size', 'size-components', 'size-files')]
    [string]$Action = 'build',
    [string]$IdfPath = $env:IDF_PATH,
    [string]$PythonEnvironment = $env:IDF_PYTHON_ENV_PATH,
    [string]$IdfToolsPath = $env:IDF_TOOLS_PATH,
    [string[]]$ToolBin = @(),
    [string]$RomElfDir = $env:ESP_ROM_ELF_DIR,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\esp32c5'),
    [string]$FirmwareVersion = ''
)

$ErrorActionPreference = 'Stop'
$expectedSdkCommit = 'fff9895c82d744c7237be8847347bdd1b07c6643'
$expectedCompilerBuild = 'esp-15.2.0_20251204'
$sourceDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

if (-not $IdfPath -or -not $PythonEnvironment) {
    throw 'Activate the ESP-IDF v6.1 PowerShell environment or pass -IdfPath and -PythonEnvironment. See docs/BUILD_C5.md.'
}
$IdfPath = (Resolve-Path -LiteralPath $IdfPath).Path
$PythonEnvironment = (Resolve-Path -LiteralPath $PythonEnvironment).Path
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$idfPython = Join-Path $PythonEnvironment 'Scripts\python.exe'
$idfScript = Join-Path $IdfPath 'tools\idf.py'
foreach ($required in @($idfPython, $idfScript)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing build tool: $required"
    }
}

$sdkCommit = & git -C $IdfPath rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'The ESP-IDF path must be a Git checkout of the pinned release.' }
if ($sdkCommit.Trim() -ne $expectedSdkCommit) {
    throw "Expected ESP-IDF v6.1 commit $expectedSdkCommit; found $sdkCommit"
}

$environmentNames = @('PATH', 'IDF_PATH', 'IDF_TOOLS_PATH', 'IDF_PYTHON_ENV_PATH',
                      'IDF_TARGET', 'ESP_ROM_ELF_DIR', 'POOM_FIRMWARE_VERSION',
                      'ESP_IDF_VERSION', 'PYTHONUTF8')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
    $env:IDF_PATH = $IdfPath
    $env:IDF_PYTHON_ENV_PATH = $PythonEnvironment
    $env:IDF_TARGET = 'esp32c5'
    $env:ESP_IDF_VERSION = '6.1'
    $env:PYTHONUTF8 = '1'
    if ($IdfToolsPath) { $env:IDF_TOOLS_PATH = $IdfToolsPath }
    if ($RomElfDir) { $env:ESP_ROM_ELF_DIR = $RomElfDir }
    $pathEntries = @((Join-Path $PythonEnvironment 'Scripts')) + $ToolBin + @($env:PATH)
    $env:PATH = $pathEntries -join [IO.Path]::PathSeparator
    if ($FirmwareVersion) { $env:POOM_FIRMWARE_VERSION = $FirmwareVersion }

    foreach ($command in @('riscv32-esp-elf-gcc', 'cmake', 'ninja')) {
        if (-not (Get-Command $command -CommandType Application -ErrorAction SilentlyContinue)) {
            throw "Missing $command. Activate the IDF environment or include its directory in -ToolBin."
        }
    }
    $compilerVersion = (& riscv32-esp-elf-gcc --version | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or $compilerVersion -notlike "*$expectedCompilerBuild*") {
        throw "Expected RISC-V GCC build $expectedCompilerBuild; found $compilerVersion"
    }

    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $defaults = @((Join-Path $sourceDir 'sdkconfig.defaults'),
                  (Join-Path $sourceDir 'sdkconfig.esp32c5.ram_optimization'),
                  (Join-Path $sourceDir 'sdkconfig.esp32c5.release')) -join ';'
    $idfArguments = @($idfScript, '-C', $sourceDir, '-B', $BuildDir,
                      '-D', 'IDF_TARGET=esp32c5',
                      '-D', "SDKCONFIG=$BuildDir\sdkconfig",
                      '-D', "SDKCONFIG_DEFAULTS=$defaults")
    # CMake does not track changes to the firmware-version environment variable.
    if ($Action -ne 'reconfigure' -and $env:POOM_FIRMWARE_VERSION) {
        $idfArguments += 'reconfigure'
    }
    $idfArguments += $Action
    & $idfPython @idfArguments
    if ($LASTEXITCODE -ne 0) {
        throw "ESP-IDF $Action failed with exit code $LASTEXITCODE"
    }
} finally {
    foreach ($name in $environmentNames) {
        if ($null -eq $savedEnvironment[$name]) {
            Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
        }
    }
}
