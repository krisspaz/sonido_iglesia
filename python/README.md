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

Para FLAC/AIFF y LUFS EBU R128 instala `.[analysis]`. Whisper y Demucs siguen siendo módulos opcionales para transcripción/separación; no se ejecutan en la ruta de audio en vivo.
