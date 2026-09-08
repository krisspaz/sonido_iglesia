# Smart Engine 2.0

## Implementado

El motor conserva la arquitectura local y separada del callback. La versión 2 añade:

- `STREAM QUALITY` 0–100 con balance tonal, dinámica, loudness, true peak, claridad, estéreo, ruido, compresión y estabilidad;
- comprobaciones visibles de loudness, true peak, low-mid, harshness, estéreo y dinámica;
- ciclo cerrado por banda: detectar → corregir → esperar 3.5 s → volver a medir → conservar o rollback;
- eficacia aprendida por iglesia para las cinco zonas tonales, limitada entre 0 y 1;
- cooldown tras cada evaluación para impedir oscilaciones y aprendizaje excesivamente rápido;
- perfil persistente `church-profile.json` con baseline, fecha, sample rate y eficacia aprendida;
- Smart Scenes: Auto, Preaching, Worship, Full Band, Announcements, Prayer y Ambience;
- modos distintos: SAFE mantiene los límites conservadores; AUTO dispone de un presupuesto tonal mayor;
- Safety Controller independiente para X32, silencio, clipping, CPU, xruns/drops, OBS, fase y procesamiento excesivo;
- reportes JSON sin audio en `Session Reports`, actualizados cada 30 s;
- análisis adaptativo: después de 10 s con score ≥92 y sin acciones/problemas, reduce su frecuencia a la mitad;
- contrato y controlador probado de `Smart Masking` para grupos Voice/Music/Ambience;
- de-esser dinámico independiente en 5.5-9 kHz con su propio closed loop;
- detección de pico verdadero sobremuestreada 4x en el limitador y en la entrada;
- A/B con volumen perceptualmente igualado;
- prueba offline que ejecuta el Smart Engine completo y emite un informe.

## De-esser

La banda de harshness (2.5-8 kHz) no distingue una `s` agresiva de un plato brillante. Se añadió un detector propio sobre la fracción de energía en 5.5-9 kHz comparada con la baseline aprendida de la iglesia, y una campana estrecha (Q 3.2) en 7.4 kHz.

Queda deliberadamente fuera del presupuesto tonal compartido: es una corrección quirúrgica y consumir ahí el presupuesto de banda ancha dejaría sin margen a las correcciones de low-mid y harshness. Sus límites son -2.5 dB en SAFE y -4.0 dB en AUTO, y tiene su propia transacción de closed loop y su propia eficacia aprendida.

## Pico verdadero

El detector del limitador usa un interpolador polifásico sinc-Blackman a 4x (12 taps por fase, 6 muestras de latencia absorbidas por el look-ahead de 1 ms), como recomienda ITU-R BS.1770-4. La estimación cúbica anterior no era limitada en banda y subestimaba el pico inter-muestra que protege el techo de -1 dBTP. El mismo detector da pico verdadero real al análisis de entrada, que antes caía a pico de muestra; con eso la detección de clipping de entrada del Safety Controller ve también el clipping inter-muestra.

Costo medido: alrededor de un 10% del tiempo de DSP (`144.10×` → `129.96×` en el benchmark de 600 s; `0.6939%` → `0.769468%` de un núcleo).

## A/B con volumen igualado

Dos integradores de 1.5 s miden la ruta seca y la procesada dentro del propio DSP. Al escuchar A se aplica a la ruta seca la ganancia que iguala ambos niveles, con límite de ±12 dB y rampa de 250 ms, de modo que la comparación no la gane el lado que suena más fuerte.

El bypass nunca se iguala. Debe seguir siendo una ruta de seguridad literal.

## Prueba offline inteligente

`Offline Test` ya no desactiva el Smart Engine. La simulación:

- ejecuta el DSP real, el Smart Engine real y el Safety Controller real sobre la grabación;
- se sincroniza por conteo de muestras y no por reloj, así que el mismo archivo produce siempre las mismas decisiones;
- dispara Auto Tune al inicio, igual que haría el operador antes de un servicio;
- trabaja sobre una copia del perfil de la iglesia (`offline-profile.json`), nunca sobre el real;
- renderiza `*-original.wav` y `*-processed.wav` con volumen igualado a la más baja de las dos, así ninguna puede recortar;
- escribe `*-report.txt` con niveles, score medio y mínimo, correcciones evaluadas, retenidas y revertidas, tiempo marcado por cada problema y un registro de eventos con hora, frecuencia, dB, confianza y resultado.

El informe no contiene audio.

## Closed loop

Cada una de las cinco correcciones tonales mantiene su propia transacción. Al activarse guarda severidad y score inicial. Después del tiempo de asentamiento compara de nuevo:

- si la severidad baja al menos 8%, el resultado es `IMPROVED` y sube ligeramente la eficacia aprendida;
- si queda estable, conserva la corrección con aprendizaje mínimo;
- si la severidad crece más de 18% o el score cae más de 5 puntos, vuelve suavemente a cero, marca `ROLLBACK` y bloquea esa corrección durante 12 s;
- el Safety Controller puede solicitar un rollback global de objetivos adaptativos durante 15 s.

No se guarda audio. El perfil contiene sólo números agregados.

## Smart Masking y grupos X32

El algoritmo de masking produce reducciones selectivas de hasta 2.5 dB en las bandas de presencia del grupo Music cuando detecta voz válida y solapamiento persistente. No toca graves/agudos no relacionados ni el grupo Voice.

Ya está conectado a la ruta de audio a través de `GroupMixer`, pero llega al sonido sólo cuando se cumplen dos condiciones: que el router resuelva tres stems estables y que el operador lo active en ADVANCED. Mientras tanto corre en modo consultivo y publica lo que haría, de modo que se puede observar un servicio completo antes de encenderlo. Cualquier problema con las rutas cae al passthrough estéreo.

Sigue necesitando seis entradas reales y estables desde la X32:

```text
USB 1–2  Voice
USB 3–4  Music
USB 5–6  Ambience
```

Antes de conectar esos targets al DSP de producción hay que verificar el driver X32 en Windows, nombres/ruteo de buses, latencia común, desconexión parcial y fallback a la mezcla estéreo. Activarlo sin esa prueba podría silenciar o duplicar grupos durante una transmisión.

## Pendiente con hardware

- recepción multicanal X32 y aplicación real de Smart Masking;
- cliente OSC/X32 de sólo lectura y, más adelante, escritura con lista permitida y confirmación;
- Room Calibration con micrófono de medición y generación de recomendaciones, separado del streaming;
- validación auditiva del score y umbrales con servicios reales;
- soak de reloj de 5–8 h con X32, OBS y encoder;
- driver virtual Windows firmado por Microsoft.

Estas funciones requieren hardware y mediciones reales; no se presentan como habilitadas hasta pasar esos gates.
