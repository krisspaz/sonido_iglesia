# Arquitectura

## Flujo

```text
X32 USB (2 canales) → callback de audio → DSP → virtual render "CSP Input"
                               │              ↓
                               └─ SPSC FIFO → análisis/Smart thread
                                              ↓
                                 virtual capture "CSP Output" → OBS
```

El callback recibe los primeros dos canales activos del dispositivo seleccionado, duplica mono sólo cuando la entrada realmente es mono, procesa por bloques y escribe al dispositivo de salida. La latencia reportada suma entrada, salida y 1 ms de look-ahead del limiter.

El driver virtual se deriva de una revisión fijada de Microsoft SysVAD. El render y el capture intercambian PCM 48 kHz/16-bit/estéreo mediante 1 s de almacenamiento circular no paginado. El callback del driver no reserva memoria, no accede a disco/red y entrega silencio ante underrun; si el render adelanta más de la capacidad, descarta primero los bytes más antiguos. El catálogo de producción debe estar firmado por Microsoft.

## Thread de audio

Sólo ejecuta copias de buffer, meters O(n), DSP preasignado y pushes lock-free. No hace disco, logs, red, JSON, OBS, ventanas, carga de archivos ni reserva deliberada de memoria. `ScopedNoDenormals` evita penalizaciones de CPU al decaer filtros/envolventes.

Cadena real:

```text
DC blocker → rumble HP IIR → EQ adaptativa IIR (low, mud, clarity, harsh,
   de-esser 7.4 kHz, air) → crossover Linkwitz-Riley 4 bandas
→ compresión coordinada/punch → saturación ligera → loudness gain
→ width safety → true-peak 4x polifásico/limiter look-ahead → A/B crossfade
```

Los cambios de usuario y Smart Engine entran mediante atomics. EQ/gain/width usan rampas; bypass/A-B usa crossfade con la ruta original retrasada a la misma latencia. No se reinicia el audio al mover controles.

El detector de pico del limitador sobremuestrea a 4x con un interpolador sinc-Blackman polifásico (12 taps por fase, 6 muestras de latencia absorbidas por el look-ahead de 1 ms). La estimación cúbica anterior no era limitada en banda y subestimaba el pico inter-muestra.

El A/B compara con volumen perceptualmente igualado: dos integradores de 1.5 s miden la ruta seca y la procesada, y al escuchar A se aplica a la ruta seca la ganancia que iguala ambos niveles (límite ±12 dB, rampa de 250 ms). El bypass nunca se iguala: debe seguir siendo una ruta de seguridad literal.

## Análisis y Smart Engine

Dos FIFOs SPSC preasignadas trasladan audio input/output. Un thread de prioridad baja calcula FFT 2048 Hann, cinco bandas, RMS/peak/crest/transientes/estéreo y libebur128. Drena todos los chunks pendientes por tick para evitar backlog.

El Smart Engine opera a 2–10 Hz. Mantiene sólo estadísticas y persistencia; nunca conserva audio histórico. Una acción necesita severidad persistente y confianza mínima. Hay presupuesto tonal global, exclusión de clarity cuando harshness es confiable y límites por módulo. La seguridad estéreo usa correlación y desequilibrio L/R persistente; limita width y balance (máximo ±0.75 dB) con rampa de 5 s. Auto Tune acumula 25 s de estadísticas, crea baseline/perfil y después la baseline sólo deriva cuando no se está corrigiendo.

## Grupos X32 y Smart Masking

Cuando `AutoGroupRouter` resuelve tres stems estables entre los primeros ocho canales de la tarjeta, `GroupMixer` suma voz + música + 0.35 x ambiente hacia la salida estéreo. Cualquier fallo de resolución, canal ausente o ruta inválida cae de inmediato al passthrough estéreo: un servicio no puede quedarse en silencio porque la detección de grupos dudara.

`SmartMaskingController` corre siempre en modo consultivo y publica lo que haría, pero sólo llega al audio si el operador activa Smart Masking en ADVANCED. `MaskingDecision::active` es la decisión; `applied` es lo que efectivamente se aplicó. Así se puede observar un servicio completo antes de encenderlo.

Cuando está activo sólo mueve dos campanas sobre el stem de música, 1.1 kHz y 4.5 kHz, con máximo 2.5 dB y rampa de 250 ms. No toca la voz, ni graves ni agudos de la música. Los coeficientes se recalculan una vez por bloque, no por muestra.

## Enlace X32 de sólo lectura

`X32Client` habla OSC 1.0 por UDP al puerto 10023. Renueva `/xremote` cada 6 s y consulta por tandas nombres de canal, faders, estado on/off y nombres de bus, para no provocar pérdida de paquetes en la consola.

La garantía de sólo lectura se aplica en el único punto de transmisión: un mensaje con argumentos es una escritura y se rechaza; además la dirección debe estar en una lista permitida. `X32Client::isReadOnlyQuery` es pública precisamente para que esa garantía sea comprobable por test.

Los nombres de canal alimentan `AutoGroupRouter::setCandidateName` como pista blanda; el router sigue necesitando que el audio esté de acuerdo antes de cambiar nada. Supone el ruteo de tarjeta por defecto del X32, Card Out 1-8 = canales 1-8.

Control de la consola es una decisión posterior y separada: necesitaría su propia lista permitida de direcciones escribibles y confirmación del operador.

El bucle de red se prueba contra una consola falsa en localhost que responde OSC real: verifica el enganche, la renovación de `/xremote`, el parseo de nombres, faders y buses, que el cliente sobreviva a datagramas malformados intercalados, que la consola no reciba **ningún** mensaje con argumentos, y que el enlace caiga solo a los 5 s de silencio. Esa prueba destapó que los temporizadores del hilo descontaban un tick fijo de 100 ms en vez del tiempo transcurrido real, con lo que todos corrían a mitad de velocidad: el timeout de enlace tardaba 10 s y la renovación de `/xremote` caía a los 12 s, por encima de los 10 s en que el X32 la expira. Ahora se mide el tiempo real por iteración.

## Calibración de sala

`RoomCalibration` analiza una grabación de micrófono de medición, completamente fuera de la ruta de streaming. Distingue respuesta al impulso de ruido estacionario por factor de cresta; con impulso calcula RT60 por integración inversa de Schroeder (T20 extrapolado), con ruido sólo respuesta en frecuencia. Publica tercios de octava relativos al promedio 200 Hz - 4 kHz, inclinación de graves y agudos, y hasta seis recomendaciones para la EQ de matrices del X32.

Sólo recomienda cortes: realzar un nulo de sala gasta headroom y suele empeorar la realimentación. No aplica nada y no devuelve la salida de streaming al PA, porque esa ruta añadiría latencia y puede crear feedback.

## UI y protección de OBS

ECO es el perfil predeterminado. Visible: meters a 15 FPS; el spectrum sólo se repinta cuando llega una FFT nueva y el texto/estado va a 5 Hz en ECO (hasta 30 FPS en otros perfiles). Oculta o minimizada: spectrum/meters/animaciones visuales no se actualizan; análisis INPUT se suspende y análisis OUTPUT/Smart/LUFS/protección continúan a 2 Hz. Con carga de sistema ≥80% se aplica la misma degradación secundaria. Nunca se desactiva abruptamente el DSP principal.

## OBS

Un cliente WebSocket v5 pequeño conecta sólo a `127.0.0.1:4455`, usa eventos y no hace polling de alta frecuencia. El password local se protege con Windows DPAPI. La fuente `Church Stream Processor Audio` se crea o actualiza con el endpoint capture ID obtenido por MMDevice COM. Todo esto vive fuera del callback.

Los opcodes, autenticación, subscripciones y campos se contrastaron con el [protocolo oficial OBS WebSocket 5.x](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md). El test automatizado aplica el algoritmo documentado a un vector password/salt/challenge cuyo resultado SHA-256/Base64 se verificó de forma independiente.

## Datos locales

```text
%APPDATA%\ChurchStreamProcessor\
├── ChurchStreamProcessor.settings
├── logs\
├── presets\*.cspreset
├── Offline Tests\                     (sólo por acción explícita)
│   ├── *-original.wav                 (referencia con volumen igualado)
│   ├── *-processed.wav                (resultado con volumen igualado)
│   ├── *-report.txt                   (problemas, correcciones y rollbacks)
│   └── offline-profile.json           (copia del perfil, no toca el real)
└── Room Calibration\*-room-*.txt      (sólo por acción explícita)
```

La desinstalación elimina proceso, driver, configuración, logs y presets de acuerdo con el requisito de desinstalación completa.

## Arranque de recuperación

`--no-audio` evita abrir APIs de dispositivo cuando un driver defectuoso bloquea la llamada del sistema. La interfaz arranca con estado real `AUDIO STOPPED`, sin meters ni métricas simuladas; Offline Test y OBS permanecen accesibles. `AUTO CONFIGURE` inicia el motor bajo petición. El acceso directo y autostart normales nunca añaden este argumento.
