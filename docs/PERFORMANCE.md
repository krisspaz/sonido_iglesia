# Rendimiento y estabilidad

## Qué medir

En DEVELOPMENT se muestran CPU normalizado de la app, CPU del sistema, CPU del dispositivo de audio, tiempo del callback medido 1 de cada 32 bloques, RAM, xruns y frecuencia Smart. Cada 30 s se registra además `analysisDrops`; el valor aceptable en una transmisión estable es cero.

Objetivos de aceptación, no promesas de hardware:

- DSP/audio normal ideal 1–3%, techo objetivo 5%;
- RAM objetivo <150 MB, máximo deseado <250 MB;
- cero crecimiento sostenido de memoria;
- cero xruns/drops después del calentamiento;
- spectrum/UI apagados al minimizar;
- OBS/encoder siempre tiene prioridad sobre análisis secundario.

## Benchmark reproducible

`ChurchStreamProcessorBenchmark` genera una mezcla estéreo determinista con tonos y transientes, cambia Clean/Punch/Clarity/Dynamics/Warmth y bypass, usa el DSP de producción y rechaza NaN/Inf, pérdida de tiempo real y muestras fuera de rango para su señal de prueba.

```powershell
.\scripts\run-soak.ps1 -SimulatedSeconds 14400
```

Esto acelera cuatro horas de contenido; sirve para estabilidad numérica y throughput, no sustituye cuatro horas de reloj con driver real. Para la prueba física:

Resultado local largo de referencia: 14 400 s procesados en 114.419 s (`125.85×`, equivalente a `0.794577%` de un núcleo), salida finita y dentro del rango de la señal de prueba. La repetición Release más reciente de 600 s dio `129.96×` y `0.769468%` de un núcleo.

La medición anterior (`135.35×` y `144.10×`) es de la cadena sin detección true-peak sobremuestreada. El detector 4x polifásico del limitador, el de-esser dinámico y el integrador de A/B igualado cuestan alrededor de un 10% del tiempo de DSP; se aceptó ese costo porque la interpolación cúbica anterior subestimaba el pico inter-muestra que protege el techo de -1 dBTP.

La suite y 600 s adicionales también pasaron bajo AddressSanitizer + UndefinedBehaviorSanitizer. El benchmark instrumentado alcanzó `41.68×` (`2.399%` de un núcleo), sin errores. LeakSanitizer no está disponible en la versión de macOS usada, por eso el crecimiento de memoria se comprueba con RSS durante el soak de reloj.

Referencia de la aplicación completa con CoreAudio real a 48 kHz, ventana visible, ECO, en Apple Silicon de 11 núcleos: CPU de proceso normalizado ~`1.02%`, Audio CPU `1.71%`, DSP `181.7 µs/callback`, RSS `113.7 MB`, cero xruns y cero samples de análisis descartados. Es una referencia local, no una extrapolación a Windows. Esa medición es anterior al detector true-peak 4x y al de-esser; queda pendiente repetirla con audio real después del cambio.

1. iniciar app minimizada, X32, OBS y encoder real;
2. anotar RAM privada a 0, 1, 2, 4 y 8 h;
3. exportar el log y confirmar `xruns=0` y `analysisDrops=0` durante el periodo estable;
4. provocar reconnect X32/OBS una vez;
5. mover todos los controles y A/B durante STREAM LIVE;
6. rechazar si el stream se interrumpe, hay pops o RAM tiene tendencia monotónica.

Los xruns que ocurran al conectar/desconectar físicamente un dispositivo se separan de los xruns durante estado estable.
