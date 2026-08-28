[CmdletBinding()]
param([int]$SimulatedSeconds = 14400)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Benchmark = Join-Path $ProjectRoot 'out/build/windows-x64/ChurchStreamProcessorBenchmark_artefacts/Release/ChurchStreamProcessorBenchmark.exe'
if (!(Test-Path $Benchmark)) { throw 'Build the Release benchmark first with scripts/build-windows.ps1.' }

Write-Host "Running accelerated DSP soak for $SimulatedSeconds seconds of stereo audio..."
& $Benchmark $SimulatedSeconds
if ($LASTEXITCODE -ne 0) { throw "DSP soak failed with exit code $LASTEXITCODE" }
