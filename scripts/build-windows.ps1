[CmdletBinding()]
param(
    [switch]$Installer,
    [switch]$AllowMissingSignedDriver
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

cmake --preset windows-x64-release
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
cmake --build --preset windows-x64-release --target ChurchStreamProcessor ChurchStreamProcessorTests ChurchStreamProcessorBenchmark
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE" }
ctest --preset windows-x64-release
if ($LASTEXITCODE -ne 0) { throw "CTest failed with exit code $LASTEXITCODE" }

$AppExe = Join-Path $ProjectRoot 'out/build/windows-x64/ChurchStreamProcessor_artefacts/Release/Church Stream Processor.exe'
$BenchmarkExe = Join-Path $ProjectRoot 'out/build/windows-x64/ChurchStreamProcessorBenchmark_artefacts/Release/ChurchStreamProcessorBenchmark.exe'
if (!(Test-Path $AppExe)) { throw "Release executable not found: $AppExe" }

$DistApp = Join-Path $ProjectRoot 'dist/app'
New-Item -ItemType Directory -Force $DistApp | Out-Null
Copy-Item $AppExe $DistApp -Force
Copy-Item $BenchmarkExe $DistApp -Force

$DriverInf = Join-Path $ProjectRoot 'dist/driver/ChurchStreamVirtual.inf'
$DriverSys = Join-Path $ProjectRoot 'dist/driver/ChurchStreamVirtual.sys'
$DriverCat = Join-Path $ProjectRoot 'dist/driver/ChurchStreamVirtual.cat'
if (!(Test-Path $DriverInf) -or !(Test-Path $DriverSys) -or !(Test-Path $DriverCat)) {
    if (!$AllowMissingSignedDriver) {
        throw 'Signed virtual-audio driver package is missing. See driver/README.md. Use -AllowMissingSignedDriver only for a development build.'
    }
    Write-Warning 'Building a development package without Church Stream Processor Output.'
} else {
    & (Join-Path $ProjectRoot 'scripts/validate-driver.ps1') -DriverDirectory (Join-Path $ProjectRoot 'dist/driver')
    if ($LASTEXITCODE -ne 0) { throw "Virtual-driver validation failed with exit code $LASTEXITCODE" }
}

if ($Installer) {
    $Candidates = @(
        "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    )
    $Iscc = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$Iscc) { throw 'Inno Setup 6 was not found.' }
    & $Iscc (Join-Path $ProjectRoot 'installer/ChurchStreamProcessor.iss')
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }
}

Get-FileHash $AppExe -Algorithm SHA256
