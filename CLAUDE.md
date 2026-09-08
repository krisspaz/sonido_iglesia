# Church Stream Processor — notas para Claude

## No lances la app para verificar cambios de DSP/análisis/Smart Engine

Esta es una app JUCE con dispositivos de audio reales; abrirla cada vez que
se toca `ProcessingEngine`, `AnalysisEngine` o `SmartEngine` es lento y no
hace falta. Usá la suite de tests, que corre señales sintéticas sin ningún
dispositivo de audio:

```bash
# Sólo la primera vez, o si cambia CMakeLists.txt (agrega/quita archivos):
cmake -S . -B build-dev -DBUILD_TESTING=ON \
  -DFETCHCONTENT_SOURCE_DIR_JUCE="$PWD/build-release/_deps/juce-src" \
  -DFETCHCONTENT_SOURCE_DIR_LIBEBUR128="$PWD/build-release/_deps/libebur128-src"

cmake --build build-dev -j8 --target ChurchStreamProcessorTests
./build-dev/ChurchStreamProcessorTests_artefacts/Debug/ChurchStreamProcessorTests
```

Los flags `FETCHCONTENT_SOURCE_DIR_*` reutilizan las fuentes de JUCE/libebur128
ya clonadas en `build-release/_deps` en vez de volver a clonarlas — sin eso,
un `build-dev` nuevo tarda varios minutos sólo en `git clone`.

Para medir CPU/latencia sin la GUI:

```bash
cmake --build build-dev -j8 --target ChurchStreamProcessorBenchmark
./build-dev/ChurchStreamProcessorBenchmark_artefacts/Debug/ChurchStreamProcessorBenchmark
```

Para probar la cadena de audio contra un WAV real (sin dispositivo, sin GUI),
usá `OfflineProcessor` — es el mismo camino que usa el botón "Offline Test"
de la app, pero es invocable como test.

## Cuándo sí hace falta lanzar la app

Sólo para: layout/UI (`MainComponent.cpp` `resized()`/`paint()`), selección
real de dispositivos de audio, integración con OBS/X32 reales, o para ver
algo con tus propios oídos en vivo. Para lo demás, los tests son la señal
confiable — si compilan y pasan, el cambio de DSP es válido sin abrir nada.

## Antes de compilar el target de la app completa

```bash
cmake --build build-dev -j8
```

usa el cache existente de `build-dev`; no hace falta reconfigurar salvo que
cambie `CMakeLists.txt`.

## Sesiones concurrentes

Este proyecto se trabaja a veces desde más de una sesión de Claude Code al
mismo tiempo sobre el mismo working tree. Si `git status` muestra un merge
en curso o archivos que cambian entre una lectura y la siguiente sin que vos
los hayas tocado, es la otra sesión escribiendo en paralelo — no asumas que
es corrupción. Verificá con el usuario antes de resolver conflictos o hacer
`git commit`/`push`, y evitá reescribir archivos que otra sesión esté
modificando activamente.
