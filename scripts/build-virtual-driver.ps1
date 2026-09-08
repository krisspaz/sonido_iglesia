[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [string]$OutputDirectory,

    [switch]$PrepareOnly,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $ProjectRoot 'out/virtual-driver'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'The virtual audio driver requires Windows, Visual Studio, the Windows SDK and the WDK.'
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed with exit code $LASTEXITCODE."
    }
}

function Get-MSBuildPath {
    $Command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($Command) { return $Command.Source }

    $VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path $VsWhere -PathType Leaf)) {
        throw 'msbuild.exe was not found. Install Visual Studio 2022 with Desktop C++ and the WDK.'
    }

    $InstallPath = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if (!$InstallPath) { throw 'Visual Studio with MSBuild was not found.' }
    $Candidate = Join-Path $InstallPath 'MSBuild\Current\Bin\MSBuild.exe'
    if (!(Test-Path $Candidate -PathType Leaf)) { throw "MSBuild was not found at $Candidate" }
    return $Candidate
}

$Git = Get-Command git.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (!$Git) { throw 'git.exe was not found.' }

$LockPath = Join-Path $ProjectRoot 'driver/upstream.lock.json'
$Lock = Get-Content $LockPath -Raw | ConvertFrom-Json
$PatchPath = Join-Path (Join-Path $ProjectRoot 'driver') $Lock.patch
$ActualPatchHash = (Get-FileHash $PatchPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ActualPatchHash -ne $Lock.patchSha256.ToLowerInvariant()) {
    throw "The SysVAD patch hash does not match driver/upstream.lock.json. Expected $($Lock.patchSha256), got $ActualPatchHash."
}

$StageRoot = Join-Path $OutputDirectory 'stage'
$SourceRoot = Join-Path $StageRoot 'windows-driver-samples'
$MarkerPath = Join-Path $SourceRoot '.church-stream-prepared'

if ($Clean -and (Test-Path $SourceRoot)) {
    $AllowedRoot = [IO.Path]::GetFullPath($StageRoot).TrimEnd('\') + '\'
    $ResolvedSource = [IO.Path]::GetFullPath($SourceRoot)
    if (!$ResolvedSource.StartsWith($AllowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean an unexpected path: $ResolvedSource"
    }
    Remove-Item $SourceRoot -Recurse -Force
}

New-Item -ItemType Directory -Force $StageRoot | Out-Null

if (!(Test-Path $SourceRoot)) {
    Invoke-Checked $Git @('clone', '--depth', '1', '--filter=blob:none', '--no-checkout', $Lock.repository, $SourceRoot)
    Invoke-Checked $Git @('-C', $SourceRoot, 'sparse-checkout', 'init', '--cone')
    Invoke-Checked $Git @('-C', $SourceRoot, 'sparse-checkout', 'set', $Lock.subtree, 'wil')
    Invoke-Checked $Git @('-C', $SourceRoot, 'fetch', '--depth', '1', 'origin', $Lock.commit)
    Invoke-Checked $Git @('-C', $SourceRoot, 'checkout', '--detach', $Lock.commit)
    Invoke-Checked $Git @('-C', $SourceRoot, 'submodule', 'update', '--init', '--depth', '1', 'wil')
}

$Head = (& $Git -C $SourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $Head -ne $Lock.commit) {
    throw "Unexpected upstream revision. Expected $($Lock.commit), got $Head."
}

if (!(Test-Path $MarkerPath -PathType Leaf)) {
    Invoke-Checked $Git @('-C', $SourceRoot, 'apply', '--check', $PatchPath)
    Invoke-Checked $Git @('-C', $SourceRoot, 'apply', $PatchPath)

    $InxPath = Join-Path $SourceRoot 'audio/sysvad/TabletAudioSample/ComponentizedAudioSample.inx'
    $NewInxPath = Join-Path $SourceRoot 'audio/sysvad/TabletAudioSample/ChurchStreamVirtual.inx'
    $InfText = [IO.File]::ReadAllText($InxPath)

    $RequiredReplacements = @(
        @('CatalogFile = sysvad.cat', 'CatalogFile = ChurchStreamVirtual.cat'),
        @('DriverVer   = 02/22/2016, 1.0.0.1', 'DriverVer   = 08/26/2026, 0.1.0.0'),
        @('%MfgName%=SYSVAD,NT$ARCH$.10.0...22621', '%MfgName%=SYSVAD,NT$ARCH$.10.0'),
        @('[SYSVAD.NT$ARCH$.10.0...22621]', '[SYSVAD.NT$ARCH$.10.0]'),
        @('Root\sysvad_ComponentizedAudioSample', 'Root\ChurchStreamProcessorAudio'),
        @('CopyFiles=SYSVAD_SA.CopyList,KEYWORDDETECTORCONTOSOADAPTER.CopyList', 'CopyFiles=SYSVAD_SA.CopyList'),
        @('AddReg=SYSVAD_SA.AddReg,KEYWORDDETECTORCONTOSOADAPTER.AddReg', 'AddReg=SYSVAD_SA.AddReg'),
        @('ProviderName = "TODO-Set-Provider"', 'ProviderName = "Church Stream Processor"'),
        @('MfgName      = "TODO-Set-Manufacturer"', 'MfgName      = "Church Stream Processor"'),
        @('MsCopyRight  = "TODO-Set-Copyright"', 'MsCopyRight  = "Copyright (c) 2026 Church Stream Processor"'),
        @('SYSVAD_SA.DeviceDesc="Virtual Audio Device (WDM) - Tablet Sample"', 'SYSVAD_SA.DeviceDesc="Church Stream Processor Virtual Audio"'),
        @('SYSVAD_ComponentizedAudioSample.SvcDesc="Virtual Audio Device (WDM) - Tablet Sample Driver"', 'ChurchStreamVirtual.SvcDesc="Church Stream Processor Virtual Audio Driver"'),
        @('SYSVAD.WaveSpeaker.szPname="SYSVAD Wave Speaker"', 'SYSVAD.WaveSpeaker.szPname="Church Stream Processor Input"'),
        @('SYSVAD.TopologySpeaker.szPname="SYSVAD Topology Speaker"', 'SYSVAD.TopologySpeaker.szPname="Church Stream Processor Input"'),
        @('SYSVAD.WaveMicIn.szPname="SYSVAD Wave Microphone Headphone"', 'SYSVAD.WaveMicIn.szPname="Church Stream Processor Output"'),
        @('SYSVAD.TopologyMicIn.szPname="SYSVAD Topology Microphone Headphone"', 'SYSVAD.TopologyMicIn.szPname="Church Stream Processor Output"'),
        @('MicInCustomNameGUID = {d48deb08-fd1c-4d1e-b821-9064d49ae96e}', 'MicInCustomNameGUID = {33999afe-398e-4451-ac69-25136d0e9f7f}'),
        @('MicInCustomName= "External Microphone Headphone"', 'MicInCustomName= "Church Stream Processor Output"')
    )

    foreach ($Replacement in $RequiredReplacements) {
        if (!$InfText.Contains($Replacement[0])) {
            throw "Required INF source text was not found: $($Replacement[0])"
        }
        $InfText = $InfText.Replace($Replacement[0], $Replacement[1])
    }

    $InfText = [Regex]::Replace($InfText, 'tabletaudiosample\.sys', 'ChurchStreamVirtual.sys', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    $InfText = [Regex]::Replace($InfText, 'sysvad_ComponentizedAudioSample', 'ChurchStreamVirtual', [Text.RegularExpressions.RegexOptions]::IgnoreCase)

    $KeywordPatterns = @(
        '(?im)^keywordDetectorContosoAdapter\.dll=222\r?\n',
        '(?im)^keyworddetectorcontosoadapter\.dll=SignatureAttributes\.PETrust\r?\n',
        '(?ims)^\[SignatureAttributes\.PETrust\]\r?\nPETrust=true\r?\n',
        '(?im)^KEYWORDDETECTORCONTOSOADAPTER\.CopyList=13\s*;\r?\n',
        '(?ims)^\[KEYWORDDETECTORCONTOSOADAPTER\.CopyList\]\r?\nkeyworddetectorcontosoadapter\.dll\r?\n\r?\n',
        '(?ims)^\[KEYWORDDETECTORCONTOSOADAPTER\.AddReg\]\r?\n.*?(?=;=+\r?\n; render interfaces)'
    )
    foreach ($Pattern in $KeywordPatterns) {
        $InfText = [Regex]::Replace($InfText, $Pattern, '')
    }

    $UnicodeWithBom = [Text.UnicodeEncoding]::new($false, $true)
    [IO.File]::WriteAllText($NewInxPath, $InfText, $UnicodeWithBom)
    Remove-Item $InxPath -Force

    foreach ($ProjectPath in @(
        (Join-Path $SourceRoot 'audio/sysvad/TabletAudioSample/TabletAudioSample.vcxproj'),
        (Join-Path $SourceRoot 'audio/sysvad/EndpointsCommon/EndpointsCommon.vcxproj')
    )) {
        $ProjectText = [IO.File]::ReadAllText($ProjectPath)
        $ProjectText = $ProjectText.Replace('SYSVAD_BTH_BYPASS;', '').Replace('SYSVAD_USB_SIDEBAND;', '')
        if ($ProjectPath.EndsWith('TabletAudioSample.vcxproj')) {
            $ProjectText = $ProjectText.Replace('<TargetName>TabletAudioSample</TargetName>', '<TargetName>ChurchStreamVirtual</TargetName>')
        }
        [IO.File]::WriteAllText($ProjectPath, $ProjectText, [Text.UTF8Encoding]::new($true))
    }

    @(
        "upstream=$($Lock.commit)",
        "patchSha256=$ActualPatchHash",
        'format=48000Hz/16-bit/stereo',
        'render=Church Stream Processor Input',
        'capture=Church Stream Processor Output'
    ) | Set-Content $MarkerPath -Encoding UTF8
} else {
    $MarkerText = Get-Content $MarkerPath -Raw
    if ($MarkerText -notmatch [Regex]::Escape("patchSha256=$ActualPatchHash")) {
        throw 'The staged source was prepared with a different patch. Run again with -Clean.'
    }
}

Write-Host "Prepared virtual-driver source: $SourceRoot"
if ($PrepareOnly) { return }

$MsBuild = Get-MSBuildPath
$WdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
if (!(Test-Path (Join-Path $WdkRoot 'Include'))) {
    throw 'Windows Driver Kit 10/11 was not found.'
}

$PackageProject = Join-Path $SourceRoot 'audio/sysvad/Package/package.VcxProj'
Invoke-Checked $MsBuild @(
    $PackageProject,
    '/m',
    '/t:Rebuild',
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    '/p:SignMode=Off'
)

$BuiltInf = Get-ChildItem (Join-Path $SourceRoot 'audio/sysvad') -Recurse -File -Filter 'ChurchStreamVirtual.inf' |
    Where-Object { $_.DirectoryName -match '[\\/]package$' } |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
if (!$BuiltInf) { throw 'The WDK build completed but ChurchStreamVirtual.inf was not found in a package directory.' }

$BuiltPackage = $BuiltInf.Directory.FullName
$BuiltSys = Join-Path $BuiltPackage 'ChurchStreamVirtual.sys'
$BuiltCat = Join-Path $BuiltPackage 'ChurchStreamVirtual.cat'
foreach ($RequiredFile in @($BuiltInf.FullName, $BuiltSys, $BuiltCat)) {
    if (!(Test-Path $RequiredFile -PathType Leaf)) { throw "Driver package file is missing: $RequiredFile" }
}

$UnsignedDirectory = Join-Path $OutputDirectory 'package-unsigned'
New-Item -ItemType Directory -Force $UnsignedDirectory | Out-Null
Copy-Item $BuiltInf.FullName $UnsignedDirectory -Force
Copy-Item $BuiltSys $UnsignedDirectory -Force
Copy-Item $BuiltCat $UnsignedDirectory -Force
Copy-Item (Join-Path $ProjectRoot 'driver/MS-PL.txt') $UnsignedDirectory -Force

Write-Host "Unsigned driver package: $UnsignedDirectory"
Write-Warning 'This package is intentionally not copied to dist/driver. It must pass Microsoft attestation/WHQL signing and scripts/import-signed-driver.ps1 first.'
Get-ChildItem $UnsignedDirectory -File | Get-FileHash -Algorithm SHA256
