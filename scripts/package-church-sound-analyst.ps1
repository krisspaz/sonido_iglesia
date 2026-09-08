param(
    [string]$Output = "dist/app"
)
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot
$BuildRoot = Join-Path $ProjectRoot "out/church-sound-analyst-package"
if (Test-Path "$BuildRoot/venv") { Remove-Item -Recurse -Force "$BuildRoot/venv" }
python -m venv "$BuildRoot/venv"
& "$BuildRoot/venv/Scripts/python.exe" -m pip install --upgrade pip
& "$BuildRoot/venv/Scripts/python.exe" -m pip install ".[server,analysis,ai]"
& "$BuildRoot/venv/Scripts/python.exe" -m pip install pyinstaller
& "$BuildRoot/venv/Scripts/pyinstaller.exe" --clean --noconfirm --onedir --name ChurchSoundAnalyst `
    --distpath $Output --workpath "$BuildRoot/work" --specpath "$BuildRoot/spec" --paths python `
    --collect-all whisper --collect-all demucs --collect-all torch `
    --hidden-import=uvicorn.logging --hidden-import=uvicorn.loops.auto --hidden-import=uvicorn.protocols.http.auto `
    --add-data "python/church_analyzer;church_analyzer" `
    python/church_analyzer/service_runner.py
if (!(Test-Path "$Output/ChurchSoundAnalyst/ChurchSoundAnalyst.exe")) { throw "No se creó ChurchSoundAnalyst.exe" }
Copy-Item "pyproject.toml" "$Output/ChurchSoundAnalyst/DEPENDENCIES.txt"
Write-Host "Servicio empaquetado en $Output/ChurchSoundAnalyst"
