[CmdletBinding()]
param(
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$CertificateThumbprint,

    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (!$PackageDirectory) { $PackageDirectory = Join-Path $ProjectRoot 'out/virtual-driver/package-unsigned' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $ProjectRoot 'out/virtual-driver/submission' }
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Driver submission cabinets must be created and signed on Windows.'
}

$Files = @('ChurchStreamVirtual.inf', 'ChurchStreamVirtual.sys', 'ChurchStreamVirtual.cat')
foreach ($Name in $Files) {
    $Path = Join-Path $PackageDirectory $Name
    if (!(Test-Path $Path -PathType Leaf)) { throw "Driver file is missing: $Path" }
}

$MakeCab = Get-Command makecab.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (!$MakeCab) { throw 'makecab.exe was not found.' }

$SignTool = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (!$SignTool) {
    $SignTool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
        Where-Object FullName -Match '\\x64\\' | Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (!$SignTool) { throw 'signtool.exe was not found. Install the Windows SDK/WDK.' }

New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$CabName = 'ChurchStreamVirtual-attestation.cab'
$DdfPath = Join-Path $OutputDirectory 'ChurchStreamVirtual.ddf'
$CabPath = Join-Path $OutputDirectory $CabName

@(
    '.OPTION EXPLICIT',
    ".Set CabinetNameTemplate=$CabName",
    ".Set DiskDirectoryTemplate=$OutputDirectory",
    '.Set CompressionType=MSZIP',
    '.Set Cabinet=on',
    '.Set Compress=on',
    ('"' + (Join-Path $PackageDirectory 'ChurchStreamVirtual.inf') + '" "ChurchStreamVirtual.inf"'),
    ('"' + (Join-Path $PackageDirectory 'ChurchStreamVirtual.sys') + '" "ChurchStreamVirtual.sys"'),
    ('"' + (Join-Path $PackageDirectory 'ChurchStreamVirtual.cat') + '" "ChurchStreamVirtual.cat"')
) | Set-Content $DdfPath -Encoding ASCII

& $MakeCab /F $DdfPath
if ($LASTEXITCODE -ne 0 -or !(Test-Path $CabPath -PathType Leaf)) {
    throw "MakeCab failed with exit code $LASTEXITCODE."
}

& $SignTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $CabPath
if ($LASTEXITCODE -ne 0) { throw "Cabinet signing failed with exit code $LASTEXITCODE." }
& $SignTool verify /pa /v $CabPath
if ($LASTEXITCODE -ne 0) { throw 'The signed submission cabinet failed Authenticode validation.' }

Write-Host "Signed Hardware Dev Center submission cabinet: $CabPath"
Write-Host 'Upload this CAB to the Microsoft Hardware Dev Center attestation/HLK workflow; do not install the unsigned package as production.'
Get-FileHash $CabPath -Algorithm SHA256
