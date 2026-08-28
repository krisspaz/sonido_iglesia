# Aceptación Windows 10/11 + X32 + OBS

Esta prueba convierte el build en un candidato de producción. Ejecutarla en Windows 10 22H2 y Windows 11 soportado, x64 Release, con el hardware real de transmisión.

## Preparación

- driver USB X32 oficial instalado y consola a 48 kHz;
- Card/USB 1–2 contiene la mezcla estéreo destinada al stream;
- paquete `ChurchStreamVirtual` firmado e instalado sin Test Mode;
- OBS Studio v28+ con WebSocket local habilitado;
- monitoreo de la salida por una ruta que no realimente la X32.

## Prueba funcional

1. Arrancar Windows e instalar con el Inno Setup generado.
2. Confirmar el render `Church Stream Processor Input` y el capture `Church Stream Processor Output`, ambos estéreo a 48 kHz.
3. Abrir la app: debe mostrar X32 CONNECTED, AUDIO PROCESSING y 48.0 kHz.
4. Enviar tono -18 dBFS primero a L y luego R desde la X32. Confirmar canal correcto en meters y OBS.
5. Abrir OBS desde el botón. Conectar WebSocket y confirmar escena y fuente `Church Stream Processor Audio`.
6. Iniciar grabación local y stream de prueba. Confirmar STREAM LIVE sin detener audio.
7. Mover Clean/Punch/Clarity/Dynamics/Warmth de 0 a 100, A/B y Bypass. Escuchar con audífonos y revisar waveform: sin click, pop, silencio ni salto duro.
8. Ejecutar Auto Tune durante 25 s; confirmar progreso, perfil y targets graduales.
9. Desconectar/reconectar USB X32. Debe mostrar DISCONNECTED y recuperar automáticamente cada 5 s sin reiniciar la app.
10. Cerrar/reabrir OBS. Debe reconectar aproximadamente cada 2.5 s y restaurar estado/fuente.
11. Suspender/reanudar Windows y repetir señal estéreo.

## Matriz de formatos

Probar 48 kHz con 64/128/256/512/1024 cuando el driver lo permita. La entrega se aprueba para producción con el buffer más bajo que mantenga cero xruns durante 8 h en la PC objetivo; no se fuerza 64 si perjudica a OBS.

## Fallos que bloquean entrega

- inversión/intercambio L/R, clipping > -1.0 dBTP con limiter activo o audio no finito;
- cualquier operación de disco/red/lock en callback encontrada por profiler;
- pops al mover controles o usar bypass/A-B;
- pérdida de fuente OBS al cambiar escena activa;
- crecimiento sostenido de RAM, xruns en estado estable o analysis drops;
- driver no firmado, requiere Test Mode o queda instalado después de desinstalar;
- CPU total de la app fuera del objetivo en la PC acordada después de minimizar.

Guardar como evidencia: instalador SHA-256, versión Windows/OBS/driver X32, PC/CPU, buffer, logs, grabación de prueba y tabla RAM/CPU por hora.
