[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CertificateThumbprint,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$AppFiles = Get-ChildItem (Join-Path $ProjectRoot 'dist/app') -Filter '*.exe' -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty FullName
if (!$AppFiles) { throw 'No staged release executables were found. Build Release first.' }

$SignTool = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (!$SignTool) {
    $SignTool = Get-ChildItem "$env:ProgramFiles(x86)\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
        Where-Object FullName -Match '\\x64\\' | Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (!$SignTool) { throw 'signtool.exe was not found in the Windows SDK.' }

foreach ($File in $AppFiles) {
    & $SignTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $File
    if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed: $File" }
    & $SignTool verify /pa /all $File
    if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed: $File" }
}

# Rebuild only after signing the staged app, otherwise the installer would
# contain the older unsigned executable even if its outer .exe were signed.
& (Join-Path $ProjectRoot 'scripts/validate-driver.ps1') -DriverDirectory (Join-Path $ProjectRoot 'dist/driver')
$IsccCandidates = @(
    "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
)
$Iscc = $IsccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$Iscc) { throw 'Inno Setup 6 was not found; cannot rebuild the signed payload.' }
& $Iscc (Join-Path $ProjectRoot 'installer/ChurchStreamProcessor.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }

$Installers = Get-ChildItem (Join-Path $ProjectRoot 'dist/installer') -Filter '*.exe' -ErrorAction Stop |
    Select-Object -ExpandProperty FullName
if (!$Installers) { throw 'No installer was generated.' }
foreach ($File in $Installers) {
    & $SignTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $File
    if ($LASTEXITCODE -ne 0) { throw "Installer signing failed: $File" }
    & $SignTool verify /pa /all $File
    if ($LASTEXITCODE -ne 0) { throw "Installer signature verification failed: $File" }
}
