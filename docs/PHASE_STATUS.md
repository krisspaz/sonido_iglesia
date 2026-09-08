# Estado fase por fase

Fecha de esta matriz: 26 de agosto de 2026.

| Fase | Implementación en código | Validación realizada aquí | Gate pendiente de producción |
|---|---|---|---|
| 1 Audio I/O | JUCE device manager, estéreo, X32 naming, selección/persistencia, meters, reconexión | Compiló Release; callback real CoreAudio 48 kHz; tests de ruteo/mono/meters | Ejecutar con X32 USB en Windows |
| 2 DSP | EQ, 4-band compressor, punch, saturation, limiter, A/B/bypass suavizados | Tests de limiter, recombinación, live changes, 44.1/48/96 kHz y 64–1024 samples | Escucha/medición con material de la iglesia |
| 3 Análisis | FFT, bandas, LUFS M/S/I/LRA, peak/RMS/true peak 4x en entrada y salida, crest/estéreo | Seno real 1 kHz, libebur128 y detector inter-muestra automatizados | Comparar contra medidor de referencia en Windows |
| 4 DSP adaptativo | targets dinámicos, transientes, stereo safety y límites | Pruebas finitas/multibanda/ceiling | Ajuste auditivo conservador con repertorio real |
| 5 Smart Engine | persistencia, confianza, prioridades, acciones, contexto, de-esser con closed loop propio | Test de no reacción instantánea, acción persistente, límites, enganche y liberación del de-esser | Sesión completa supervisada |
| 6 Auto Tune | 25 s, baseline, ocho perfiles, deriva lenta, persistencia en `church-profile.json` | Test de silencio, ventana de 25 s, baseline y perfil medido; simulación offline de 40 s que aprende baseline sobre una copia del perfil | Confirmar clasificación con varias mezclas reales |
| 7 OBS | WebSocket v5 local, auth, eventos, escena, source create/update/recreate | Vector SHA-256/Base64 automatizado y protocolo contrastado | OBS real en Windows, escenas/colecciones distintas |
| 8 Audio virtual | patch SysVAD fijado, ring PCM 48 kHz estéreo, sólo render/capture, build WDK reproducible; app/OBS resuelven endpoint; instalador usa PnPUtil | hash/manifiesto CTest y `git apply --check` contra commit fijado aprobados | Compilar WDK en Windows, firma Microsoft, Driver Verifier/HLK y prueba de cable real |
| 9 Frontend | UI JUCE nativa, meters/spectrum reales, What I’m Doing | Ejecución local real; no WebView/Electron | Revisión DPI 100/125/150% Windows |
| 10 Instalación | Inno Setup, accesos, autostart/minimized, tray, cleanup | Orden de firma interior/exterior y errores nativos endurecidos | Compilar/firmar instalador en Windows |
| 11 Estabilidad | tests, benchmark acelerado, sanitizers, logs CPU/xruns/drops, degradación UI | Soak de 4 h simuladas: 135.35×; ASan/UBSan y benchmark instrumentado aprobados | Soak de 4–8 h de reloj con X32+OBS+encoder |

## Módulos añadidos sin hardware disponible

| Módulo | Implementación | Validación realizada aquí | Gate pendiente |
|---|---|---|---|
| Grupos X32 + Smart Masking | `GroupMixer` suma los tres stems con fallback estéreo; masking consultivo por defecto, aplicado sólo con activación explícita | Test de rechazo con rutas inválidas, enganche con música cubriendo la presencia de la voz, límite de 2.5 dB y bandas extremas intactas | Seis canales reales desde la tarjeta X32 |
| Enlace X32 sólo lectura | OSC 1.0 sobre UDP 10023, `/xremote` renovado, nombres/faders/buses por tandas | Vectores OSC verificados a mano, rechazo de paquetes truncados, rechazo de toda escritura, taper de fader en sus cuatro tramos. Además una consola falsa en localhost ejercita el bucle de socket completo: enganche, `/xremote`, parseo de nombres/faders/buses, supervivencia a datagramas basura, cero escrituras observadas en el cable y caída del enlace a los 5 s de silencio | Consola física: direcciones y firmware reales, latencia sobre la red de la iglesia |
| Calibración de sala | Tercios de octava, RT60 por Schroeder T20, detección de modos, recomendaciones para matrices | Sala sintética con RT60 de 1.2 s y modo de 125 Hz: estimación dentro de 0.25 s y modo detectado; medición silenciosa rechazada | Micrófono de medición calibrado contra sistema de referencia |

## Resultado local medido

- tests: 100% aprobados;
- benchmark Release más reciente, estéreo 48 kHz/256, cadena completa y cambios en vivo: `129.96×` tiempo real durante 10 min simulados (`0.769468%` de un núcleo);
- soak Release de 4 h simuladas: `125.85×`, equivalente a `0.794577%` de un núcleo;
- las cifras anteriores (`144.10×` y `135.35×`) son de la cadena sin detección true-peak sobremuestreada; el detector 4x, el de-esser y el integrador de A/B igualado cuestan ~10% del tiempo de DSP;
- ASan+UBSan: tests aprobados; 10 min simulados a `41.68×` incluso instrumentado, sin hallazgos;
- ejecución de audio real local visible (medición anterior al detector true-peak 4x y al de-esser, pendiente de repetir): app normalizada ~`1.02%`, Audio CPU interno `1.71%`, DSP `181.7 µs/callback`, RSS `113.7 MB`, `xruns=0`, `analysisDrops=0`;
- no hay X32, Windows ni OBS disponibles en esta máquina, por lo que esos tres puntos no están marcados como aprobados.

El dato benchmark aísla DSP y no representa el consumo total de GUI/device driver/OBS. Las cifras finales sólo son válidas tras el protocolo Windows.
