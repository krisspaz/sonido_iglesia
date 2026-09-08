param(
    [string]$Output = "dist/app"
)
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot
$BuildRoot = Join-Path $ProjectRoot "out/church-sound-analyst-package"
python -m venv "$BuildRoot/venv"
& "$BuildRoot/venv/Scripts/python.exe" -m pip install --upgrade pip
& "$BuildRoot/venv/Scripts/python.exe" -m pip install ".[server,analysis,ai]"
& "$BuildRoot/venv/Scripts/python.exe" -m pip install pyinstaller
& "$BuildRoot/venv/Scripts/pyinstaller.exe" --clean --onedir --name ChurchSoundAnalyst `
    --distpath $Output --workpath "$BuildRoot/work" --specpath "$BuildRoot/spec" --paths python `
    --collect-all whisper --collect-all demucs `
    --add-data "python/church_analyzer;church_analyzer" `
    python/church_analyzer/service_runner.py
if (!(Test-Path "$Output/ChurchSoundAnalyst/ChurchSoundAnalyst.exe")) { throw "No se creó ChurchSoundAnalyst.exe" }
Write-Host "Servicio empaquetado en $Output/ChurchSoundAnalyst"
