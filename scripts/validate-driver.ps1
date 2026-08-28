[CmdletBinding()]
param(
    [string]$DriverDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist/driver')
)

$ErrorActionPreference = 'Stop'
$Inf = Join-Path $DriverDirectory 'ChurchStreamVirtual.inf'
$Sys = Join-Path $DriverDirectory 'ChurchStreamVirtual.sys'
$Cat = Join-Path $DriverDirectory 'ChurchStreamVirtual.cat'
foreach ($File in @($Inf, $Sys, $Cat)) {
    if (!(Test-Path $File -PathType Leaf)) { throw "Required virtual-driver file is missing: $File" }
}

$InfText = Get-Content $Inf -Raw
foreach ($RequiredPattern in @(
    '(?im)^\s*Class\s*=\s*MEDIA\s*$',
    '(?im)^\s*ClassGuid\s*=\s*\{4d36e96c-e325-11ce-bfc1-08002be10318\}\s*$',
    '(?im)^\s*CatalogFile\s*=\s*ChurchStreamVirtual\.cat\s*$',
    '(?i)Root\\ChurchStreamProcessorAudio',
    '(?i)ChurchStreamVirtual\.sys',
    '(?i)Church Stream Processor Input',
    '(?i)Church Stream Processor Output'
)) {
    if ($InfText -notmatch $RequiredPattern) {
        throw "ChurchStreamVirtual.inf is missing a required production declaration: $RequiredPattern"
    }
}

$SignTool = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (!$SignTool) {
    $SignTool = Get-ChildItem "$env:ProgramFiles(x86)\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
        Where-Object FullName -Match '\\x64\\' | Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (!$SignTool) { throw 'signtool.exe was not found; kernel-policy validation cannot be skipped.' }

& $SignTool verify /kp /all /v $Cat
if ($LASTEXITCODE -ne 0) { throw 'The virtual-driver catalog does not have a valid kernel-policy signature.' }
& $SignTool verify /kp /c $Cat /v $Sys
if ($LASTEXITCODE -ne 0) { throw 'ChurchStreamVirtual.sys is not covered by the signed catalog.' }
& $SignTool verify /kp /c $Cat /v $Inf
if ($LASTEXITCODE -ne 0) { throw 'ChurchStreamVirtual.inf is not covered by the signed catalog.' }

$InfVerif = Get-ChildItem "$env:ProgramFiles(x86)\Windows Kits\10\bin" -Recurse -Filter infverif.exe -ErrorAction SilentlyContinue |
    Where-Object FullName -Match '\\x64\\' | Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if ($InfVerif) {
    & $InfVerif /w $Inf
    if ($LASTEXITCODE -ne 0) { throw "InfVerif rejected ChurchStreamVirtual.inf with exit code $LASTEXITCODE" }
} else {
    Write-Warning 'InfVerif was not found; install the WDK to run INF structural validation.'
}

Get-FileHash @($Inf, $Sys, $Cat) -Algorithm SHA256
