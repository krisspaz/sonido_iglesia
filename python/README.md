# Church Stream Analyzer (Python)

Analizador offline para grabaciones de culto. Produce métricas y recomendaciones explicables sin modificar el audio ni controlar la consola. El WAV se procesa localmente.

```bash
python -m pip install -e .
church-analyze grabacion.wav -o reporte.json
```

Para activar la API local:

```bash
python -m pip install -e '.[server]'
CHURCH_ANALYZER_TOKEN_FILE='/ruta/al/token/creado-por-la-app' \
uvicorn church_analyzer.api:create_app --factory --host 127.0.0.1 --port 8765
```

Luego abre `http://127.0.0.1:8765/docs` en la PC. Esta primera versión escucha sólo en loopback. La API sólo analiza archivos subidos o rutas locales; no expone comandos de audio/OBS.

Además expone `/reference-match`, `/feedback`, `/learning` y `DELETE /learning`. El historial SQLite contiene únicamente métricas y decisiones del operador; nunca guarda audio.

Para FLAC/AIFF y LUFS EBU R128 instala `.[analysis]`. Sin esas dependencias, el informe marca explícitamente sus mediciones como aproximadas; no las presenta como EBU/ITU certificadas. Whisper y Demucs siguen siendo módulos opcionales para transcripción/separación; no se ejecutan en la ruta de audio en vivo.

## Operación segura

Las operaciones pesadas pueden solicitarse como trabajos con estado local (`POST /jobs/analyze`, `POST /jobs/ai-analysis`, `GET` o `DELETE /jobs/{id}`); Whisper y Demucs aceptan `"background": true`. El trabajo compuesto de IA transcribe localmente, refina las secciones de prédica mediante timestamps y puede separar stems sólo si se solicita. Los trabajos están limitados a un proceso por defecto, pueden cancelarse y no sobreviven a un reinicio del servicio. La app C++ conserva su comportamiento actual si el servicio se cae o no responde.

La API acepta WAV, FLAC y AIFF. Puede restringirse a carpetas concretas con `--allowed-root`; las salidas de Demucs se escriben siempre debajo del directorio de datos del servicio. El historial SQLite guarda métricas, decisiones y calificaciones, nunca audio.

## Paquete Windows

El build de Windows ejecuta `scripts/package-church-sound-analyst.ps1`, crea `dist/app/ChurchSoundAnalyst/ChurchSoundAnalyst.exe` y el instalador lo instala junto a la aplicación. El script crea un entorno aislado, empaqueta FastAPI, análisis y los módulos opcionales de IA, y falla si no se produce el ejecutable. Los pesos de Whisper/Demucs se descargan la primera vez que el operador selecciona esos módulos (pueden preinstalarse en la caché del equipo); no se incluyen silenciosamente en el instalador por tamaño y licencia. En Windows, Church Stream Processor lo inicia como proceso auxiliar local con el token de `%APPDATA%\\ChurchStreamProcessor\\church-sound-analyst.token`. Si el servicio no inicia, el DSP C++ continúa sin cambios.

El menú `ANALYZE PYTHON` permite analizar, comparar una referencia, solicitar Whisper/Demucs, revisar recomendaciones, aplicarlas con confirmación y calificar una sesión. La aplicación al DSP está acotada a ±3 dB para EQ dinámica y a -18…-10 LUFS para el objetivo de loudness.
