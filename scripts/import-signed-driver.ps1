[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SignedPackageDirectory
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$SignedPackageDirectory = [IO.Path]::GetFullPath($SignedPackageDirectory)

& (Join-Path $PSScriptRoot 'validate-driver.ps1') -DriverDirectory $SignedPackageDirectory
if ($LASTEXITCODE -ne 0) { throw "Signed driver validation failed with exit code $LASTEXITCODE" }

$Destination = Join-Path $ProjectRoot 'dist/driver'
New-Item -ItemType Directory -Force $Destination | Out-Null
foreach ($Name in @('ChurchStreamVirtual.inf', 'ChurchStreamVirtual.sys', 'ChurchStreamVirtual.cat')) {
    Copy-Item (Join-Path $SignedPackageDirectory $Name) $Destination -Force
}
Copy-Item (Join-Path $ProjectRoot 'driver/MS-PL.txt') $Destination -Force

Write-Host "Validated production driver imported to $Destination"
Get-ChildItem $Destination -File | Get-FileHash -Algorithm SHA256
