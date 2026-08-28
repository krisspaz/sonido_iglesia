# Church Stream Processor

Aplicación nativa C++20 + JUCE para procesar en tiempo real una mezcla estéreo de iglesia. Todo el audio, análisis, presets y control de OBS permanecen en la PC; no hay nube, cuentas, telemetría, Electron, WebView, Python en runtime ni modelos de IA.

## Estado de la entrega

El árbol contiene una aplicación funcional con:

- entrada/salida estéreo real, selección persistente y detección/reconexión X32;
- DSP real con DC/rumble, EQ adaptativa, de-esser dinámico, cuatro bandas, punch, saturación ligera, loudness gain y limiter con look-ahead y detección de pico verdadero 4x;
- bypass y A/B con rutas de latencia igualada, crossfade de 50 ms y volumen perceptualmente igualado;
- FFT 2048, RMS, Peak, True Peak, LUFS M/S/I/LRA, crest, bandas, transientes y estéreo;
- Smart Engine conservador, memoria temporal, confianza, prioridades, Auto Tune de 25 s y acciones explicables;
- Smart Engine 2.0 con score 0–100, evaluación closed-loop, aprendizaje local por iglesia y rollback;
- Smart Scenes, Safety Controller independiente y reportes de sesión JSON sin guardar audio;
- suma de grupos X32 con fallback estéreo y Smart Masking selectivo por banda, en modo consultivo hasta que se activa;
- enlace X32 de sólo lectura por OSC/UDP para leer nombres, faders y buses, con la escritura bloqueada por lista permitida;
- calibración de sala con micrófono de medición: tercios de octava, RT60 y recomendaciones para la EQ de matrices, sin aplicar nada;
- OBS WebSocket v5 local, estado stream/record, escena y creación/actualización de fuente WASAPI;
- Offline Test WAV/FLAC/AIFF que simula el Smart Engine completo y emite par A/B igualado más informe;
- presets locales, tray, autostart, ECO/Balanced/HQ y diagnósticos;
- tests automatizados, benchmark/soak, presets CMake de Windows y proyecto Inno Setup.

El código reproducible del driver virtual también está incluido como una modificación fijada de SysVAD: par render/capture estéreo a 48 kHz, ring buffer no paginado y build WDK. Hay dos gates externos que este equipo macOS no puede certificar: prueba física con X32+OBS en Windows y firma Microsoft del paquete kernel. No se incluye un `.sys` falso ni se acepta un paquete sin firma como producción. El build y la ruta de firma están en [driver/README.md](driver/README.md); la matriz honesta de validación está en [docs/PHASE_STATUS.md](docs/PHASE_STATUS.md).

## Compilar en Windows 10/11

Requisitos:

- Windows 10/11 x64;
- Visual Studio 2022 con “Desktop development with C++” y Windows SDK;
- Windows Driver Kit sólo para compilar el endpoint virtual;
- CMake 3.24+ y Git;
- Inno Setup 6 sólo para generar el instalador;
- acceso a internet durante la primera configuración para descargar JUCE 8.0.15 y libebur128 1.2.6 fijados por versión.

En PowerShell desde la raíz:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-windows.ps1 -AllowMissingSignedDriver
```

Ese comando configura x64 Release, compila la app/tests/benchmark y ejecuta CTest. Para producción, coloca primero el paquete Microsoft-signed en `dist/driver/` y ejecuta el build sin la excepción de desarrollo:

```powershell
.\scripts\build-windows.ps1
```

El `.exe` de desarrollo queda en:

```text
out/build/windows-x64/ChurchStreamProcessor_artefacts/Release/Church Stream Processor.exe
```

Después firma los binarios de aplicación, reconstruye el instalador con esos binarios firmados y firma el instalador, todo en el orden correcto, con:

```powershell
.\scripts\sign-release.ps1 -CertificateThumbprint 'THUMBPRINT_DEL_CERTIFICADO'
```

`validate-driver.ps1` rechaza paquetes incompletos, INF que no sea clase MEDIA, archivos fuera del catálogo o una firma que no pase la política kernel. `sign-release.ps1` vuelve a ejecutar esa validación antes de empaquetar.

Para construir el código del driver antes de enviarlo a Microsoft:

```powershell
.\scripts\build-virtual-driver.ps1 -Configuration Release -Platform x64
```

El paquete sin firma queda aislado en `out\virtual-driver\package-unsigned`; consulta el flujo de firma e importación en [driver/README.md](driver/README.md).

## Primera prueba con X32

1. En la X32 envía el bus o Main L/R deseado a USB/Card 1–2. La aplicación no cambia silenciosamente el ruteo interno de la consola.
2. Instala el driver USB oficial de Behringer y confirma 48 kHz.
3. Abre la aplicación, selecciona `X32` como INPUT y pulsa `AUTO CONFIGURE`.
4. Selecciona `Church Stream Processor Input` como OUTPUT de la aplicación. OBS recibirá el endpoint capture `Church Stream Processor Output`; ambos aparecen al instalar el paquete virtual firmado.
5. Abre OBS. Si WebSocket pide contraseña, escríbela una vez en `OBS LOCAL PASSWORD` y pulsa `CONNECT OBS`.
6. Verifica señal real en INPUT y OUTPUT; después inicia el stream y ejecuta la lista de [aceptación Windows/X32](docs/WINDOWS_X32_ACCEPTANCE.md).

La app guarda configuración, presets y logs técnicos en `%APPDATA%\ChurchStreamProcessor\`. No guarda audio, salvo los WAV que el usuario crea expresamente en Offline Test.

Si un driver de audio de terceros bloquea su API durante el arranque, existe un modo de recuperación que abre la UI sin intentar abrir dispositivos:

```powershell
& '.\Church Stream Processor.exe' --no-audio
```

En ese estado no se inventa señal: AUDIO muestra `STOPPED`, meters/métricas quedan vacíos y Offline Test sigue disponible. `AUTO CONFIGURE` vuelve a intentar iniciar el motor de audio de forma explícita.

## Arquitectura y pruebas

- [Arquitectura y threads](docs/ARCHITECTURE.md)
- [Smart Engine 2.0, closed loop y grupos X32](docs/SMART_ENGINE_2.md)
- [Estado fase por fase](docs/PHASE_STATUS.md)
- [Aceptación Windows, X32 y OBS](docs/WINDOWS_X32_ACCEPTANCE.md)
- [Rendimiento y soak](docs/PERFORMANCE.md)
- [Driver virtual firmado](driver/README.md)

Ejecutar un soak acelerado de cuatro horas de audio:

```powershell
.\scripts\run-soak.ps1 -SimulatedSeconds 14400
```

Los objetivos de CPU/RAM son objetivos de ingeniería, no garantías universales; deben aprobarse en el hardware Windows que transmitirá con OBS.
