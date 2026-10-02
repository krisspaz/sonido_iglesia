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

Sólo ejecuta copias de buffer, meters O(n), DSP preasignado y comunicación lock-free con análisis/UI. No hace disco, logs, red, JSON, OBS, ventanas, carga de archivos ni reserva deliberada de memoria. Los diagnósticos de Smart Masking se publican mediante un snapshot atómico; las pistas de nombres X32 también cruzan hacia el router como atomics. `ScopedNoDenormals` evita penalizaciones de CPU al decaer filtros/envolventes.

Cadena real:

```text
DC blocker 5 Hz → rumble HP Butterworth 4º orden → EQ adaptativa IIR (warmth
   shelf, low, mud, clarity, harsh, de-esser estático, air shelf)
→ EQ dinámica (mud 240 Hz, harsh 3.8 kHz) + de-esser dinámico 7 kHz
→ leveller K-ponderado, etapa de entrada (programa → nivel de trabajo -20 LUFS)
→ crossover Linkwitz-Riley 4 bandas → compresión coordinada con soft knee,
   detector RMS y makeup → saturación oversampled 4x → loudness gain
→ tone match (EQ lenta hacia curva objetivo) → MonoSpread + width safety
→ leveller, etapa de salida (nivel de trabajo → objetivo, más la loudness
   que quitó la compresión) → true-peak 4x polifásico
→ limiter look-ahead con mínimo deslizante → A/B crossfade
```

Los cambios de usuario y Smart Engine entran mediante atomics. EQ/gain/width usan rampas; bypass/A-B usa crossfade con la ruta original retrasada a la misma latencia. No se reinicia el audio al mover controles.

El `Broadcast Programme Leveller` trabaja también cuando la X32 entrega sólo un estéreo L/R. Mide el programa **antes del compresor** (`ProgrammeLevelTracker`) y reparte su ganancia en dos etapas, como un procesador de broadcast: la de entrada lleva el programa a un **nivel de trabajo fijo de -20 LUFS** antes del crossover, y la de salida lo lleva del nivel de trabajo al objetivo del operador y devuelve la loudness que quitó la compresión. La recuperación total está limitada a +24 dB y el corte a -10 dB. El true-peak limiter sigue siendo la última protección. No pretende separar voz y música dentro de un estéreo ya mezclado: las iguala porque mide loudness K-ponderada por sección.

Antes medía después de la compresión. Eso dejaba al compresor juzgando el nivel crudo de la consola contra umbrales absolutos, y los servicios grabados llegan entre -30 y -45 dBFS RMS: en dos servicios completos el compresor promedió 0.002 dB de reducción. Con la etapa de entrada delante, los umbrales son un punto de operación real. Si la ganancia total choca con el límite, el faltante se descuenta en la etapa de salida y no en la de entrada; descontarlo antes dejaba una prédica muy baja bajo todos los umbrales, justo el programa que más necesitaba compresión.

La compensación de la compresión es la relación entre la potencia K-ponderada que entra al crossover y la que sale de la ruta wet, promediadas 3 s y sólo con el gate abierto, limitada a ±4 dB. Es una relación entre dos promedios del mismo tramo, así que sigue lo que hizo el compresor y no cómo se movió el programa: no es un segundo leveller que pueda pelear con el primero.

Medido sobre dos servicios reales grabados de YouTube (render offline con `ChurchStreamProcessorRender`), la prédica quedaba 15.7 y 9.7 dB bajo la alabanza en el original y 4.1 y 8.6 dB con el leveller anterior; con el actual, 0.7 dB en el servicio cuyo nivel cabe en el rango de recuperación. El otro llega tan bajo (-38 LUFS de prédica) que choca con el límite y queda en 3.2 dB; con 10 dB más de nivel de entrada la brecha es 0.3 dB.

El detector es **K-ponderado** según BS.1770 —shelf a 1681.97 Hz y paso alto RLB a 38.14 Hz, bilinealizados a la frecuencia de muestreo real en vez de usar los coeficientes tabulados sólo para 48 kHz— y el nivel se reporta como un LUFS aproximado. No es una medición conforme: el detector es un polo simple y no bloques de 400 ms. La ponderación importa porque un RMS plano lo domina el bombo y el bajo, así que la misma voz terminaba nivelada distinto según lo que tocara la banda debajo.

Hay **un solo lazo a cargo del nivel del programa**. El leveller medía después del `outputGain` del Smart Engine, así que cuando ambos corrían se cancelaban: el leveller apuntaba a un -19 dBFS RMS fijo mientras el Smart Engine llevaba la ganancia hacia el objetivo LUFS del operador, y como el leveller ve esa ganancia se la quitaba. Los dos venían encendidos por defecto, de modo que el stream quedaba varios dB por debajo del objetivo con las dos ganancias vagando una contra otra. Con el leveller activo el `loudnessGainDb` del Smart Engine se retira.

El nivel no se suaviza con constantes fijas sino con un **filtro de Kalman 1-D en dB**. Mientras el programa es estable la covarianza colapsa y el estimado casi no se mueve, que es lo que evita el bombeo entre frases; cuando la innovación se mantiene alta (fin de alabanza, inicio de oración) el ruido de proceso sube y el mismo filtro se vuelve rápido. La aceleración es asimétrica: hacia arriba siempre —el programa subió y la ganancia debe bajar ya— y hacia abajo sólo después de que el gate acepte una sección nueva, porque una caída y una pausa son indistinguibles hasta entonces y acelerar sobre la duda es exactamente cómo se termina amplificando el aire acondicionado. Ninguna constante fija pasa las dos pruebas a la vez: lenta pierde el cambio de sección, rápida cabalga las sílabas.

El Kalman sigue la loudness **momentary** de BS.1770 (potencia integrada 400 ms antes del logaritmo), mientras el gate, la variación del habla y el silencio siguen juzgándose con el detector rápido de 50 ms. Promediar en dB un detector de 50 ms subestimaba la loudness en una cantidad que crece con cuánto se mueve el nivel: medido en servicios reales, 4-5 dB en la prédica y 2 dB en la alabanza. La voz se leía más baja de lo que era, así que el leveller o la ponía por encima de la música o se quedaba sin rango de recuperación intentándolo.

El **gating** tiene la forma de BS.1770 —piso absoluto más gate relativo bajo el nivel corriente del programa— aplicado al detector RMS del leveller. No es una medición conforme: no hay ponderación K ni bloques de 400 ms. La referencia es una media acumulativa que degrada a ventana de 30 s, porque una media exponencial pura tarda minutos en ser significativa y hasta entonces el gate relativo queda demasiado bajo para excluir nada. La decisión se toma sobre nivel integrado a 400 ms y con 2 dB de histéresis: las sílabas oscilan unos 12 dB pico a valle, más que el gate relativo de 10 dB, y sin ambas cosas el gate parpadea. Una pausa congela el leveller; el silencio real bajo el gate absoluto lo devuelve a unidad deslizando el estimado hacia el objetivo, no forzando la ganancia, para que no haya salto al volver el programa. Distinguir pausa de sección más suave se hace por duración (2.5 s) **y** por variación: el habla mueve su propio nivel varios dB, una sala vacía no. La ventana de variación arranca al cerrar el gate, porque el escalón que lo cerró haría que toda pausa pareciera habla.

El detector de pico del limitador sobremuestrea a 4x con un interpolador sinc-Blackman polifásico (12 taps por fase, 6 muestras de latencia absorbidas por el look-ahead de 1 ms). La estimación cúbica anterior no era limitada en banda y subestimaba el pico inter-muestra.

La ganancia del limitador no se aplica de forma instantánea. La muestra que sale de la línea de retardo tiene un look-ahead de antigüedad, así que la ganancia que la protege está en algún punto de la ventana de ganancias calculadas desde entonces: se toma el **mínimo deslizante** de esa ventana y luego se suaviza con una constante de un quinto del look-ahead. El mínimo es lo que hace seguro suavizar, porque sostiene la reducción durante todo el tiempo que el pico puede estar en vuelo. Antes se aplicaba la ganancia instantánea a la muestra retrasada, lo que era un escalón de una muestra, y un escalón de ganancia es energía de banda ancha: con el detector 4x disparando constantemente sobre programa denso se oye como aspereza en los graves.

La **EQ dinámica** extrae cada banda con un paso banda de ganancia de pico unitaria y la reinyecta escalada, `salida = entrada + (g - 1) · pasoBanda(entrada)`, de modo que `g` puede moverse por muestra sin recalcular coeficientes. Las dos bandas sostenidas (mud a 240 Hz, harsh a 3.8 kHz, máximo 3 dB cada una) se juzgan contra el programa de banda ancha; el **de-esser** a 7 kHz se juzga contra el promedio de su propia banda, con ataque de 0.8 ms y release de 50 ms. Esa asimetría no es un detalle: un detector que se adapta a su propia banda trataría el exceso de graves medios como normal al cabo de un segundo y dejaría de corregirlo, mientras que un de-esser referido a la banda ancha deja de funcionar en cuanto una canción con bajos sube la referencia. Lo que había antes en 7.4 kHz no podía ser un de-esser: era un peak estático cuya ganancia movía el Smart Engine a 2-10 Hz con rampa de 0.75 s, y una sibilante dura entre 60 y 150 ms.

El **tone match** (`ToneMatch.h`) lleva el balance tonal de largo plazo hacia una curva objetivo fija, medida en seis bandas relativas a 1 kHz (100, 280, 3.2 k, 7 k y 13 kHz; cada una dos pasa-banda en cascada, porque con 6 dB/oct la banda de aire leía sobre todo energía de 7 kHz). La curva sale de los videos producidos que abren y cierran los streams de la iglesia (tres tramos, dentro de 1.5 dB entre sí). Hace falta porque la baseline del Smart Engine se aprende de la misma iglesia: un servicio opaco todas las semanas enseña que opaco es normal y nunca se abre. Medidos contra la curva, dos servicios en vivo estaban 4-10 dB cortos en la banda de aire y, en la prédica, 4 dB pesados en 280 Hz.

Qué corregir se decide en lazo abierto: se mide la entrada de sus filtros, con ventana de 15 s y sólo con el gate del leveller abierto. Cuánto lleva corregido se mide también, con las mismas bandas a su salida, y las ganancias se integran contra ese efecto medido (relación salida/entrada en 3 s) y no contra la respuesta del filtro en el centro de la banda: con el espectro real, que cae fuerte y que el códec corta sobre 16 kHz, casi toda la energía de la banda de aire está en su parte baja, donde un shelf de 12 kHz hace mucho menos. Resolver sobre respuestas puntuales dejaba el shelf en su límite y la banda 3 dB corta. Velocidad máxima 0.5 dB/s. Límites asimétricos: grave -3/+2, cuerpo -3/+1, presencia -3/+4, brillo -3/+5, aire -4/+6. Los agudos se quedan cortos a propósito respecto de lo que pediría una prédica: la curva es de música y el tone match va después del de-esser. Los cortes también son suaves, porque parte del exceso de 280 Hz en la prédica es la voz misma (el habla tiene más energía en 400-500 Hz que la música).

Va después de la dinámica, como la EQ de mastering: delante de un compresor multibanda, un realce mete su banda en más reducción y los umbrales tiran del balance hacia el suyo. El Smart Engine juzga el programa **sin** el tone match (`SmartEngine::withoutToneMatch` le quita su respuesta bin a bin y aplica la relación resultante a las bandas del analizador); si no, cada realce del tone match le parecería un exceso que cortar y los dos tirarían en sentidos opuestos. Se apaga en MANUAL y desde ADVANCED.

El **MonoSpread** (`MonoSpread.h`) da ancho a una alimentación mono sin tocar su suma mono. Los dos servicios grabados llegaron mono todo el tiempo (Side 40-60 dB bajo Mid) salvo los videos. Sintetiza un Side a ~-13 dB desde el Mid sobre 350 Hz con cuatro all-pass de Schroeder cortos (4.7-16.9 ms, primos entre sí) y lo suma a un canal y lo resta del otro, así que L + R es exactamente 2·Mid. Sólo actúa cuando la entrada es mono (Side bajo -35 dB) y se desvanece hacia -25 dB, con rampa de 2 s; una fuente estéreo real conserva su imagen. Entra antes del bass-mono y de los controles de ancho, que conservan la última palabra. Correlación resultante ≈ 0.94-0.95, muy lejos del umbral de la protección de coherencia (0.30).

El compresor usa **detector RMS**, **soft knee de 8 dB** y **makeup**. Sus umbrales son relativos al nivel de trabajo del leveller (de -22 a -32 dBFS por banda según DYNAMICS, ratio 1.4:1 a 3.8:1; 2.6:1 en el valor por defecto), no a full scale. Una variante más fuerte se probó contra los servicios grabados: duplicaba el tiempo con más de 8-10 dB de reducción —lo que el `SafetyController` trata como procesamiento excesivo y revierte— sin mejorar ni el rango ni el crest de la prédica. El makeup devuelve el 80% de la reducción promediada a 500 ms: la parte sostenida se recupera, así que un pasaje fuerte se densifica en vez de simplemente bajar de volumen, y el movimiento rápido que el compresor existe para controlar se deja intacto. El detector corre sobre la banda sin comprimir, así que nada de esto realimenta. Sin makeup cada dB de compresión era un dB menos de salida.

La **saturación** corre sólo sobre las bandas bajo 4 kHz. Un tanh a 48 kHz pliega los armónicos de todo lo que está sobre 8 kHz de vuelta al rango audible como aliasing inarmónico, que es exactamente el tipo de falla que hace sonar granuloso un stream en platillos y sibilantes sin poder señalarla. Saturar los grupos grave y medio por separado además evita que el bajo intermodule con las voces a través de la misma no linealidad.

El **paso alto de rumble** es Butterworth de 4º orden (dos secciones, Q 0.5412 y 1.3066) con corte por defecto entre 30 y 90 Hz. Lo que ensucia un stream de iglesia vive entre 40 y 90 Hz: manejo de micrófono, aire acondicionado y golpes de tarima. El 2º orden a 20 Hz que había antes no quitaba nada de eso. El bloqueador de DC se fijó en 5 Hz derivado de la frecuencia de muestreo; el coeficiente 0.995 anterior era un paso alto de primer orden en unos 38 Hz, es decir modelado tonal real escondido dentro de algo con nombre de medida de seguridad, y se movía con la frecuencia de muestreo.

La **compatibilidad mono** colapsa el canal Side bajo 120 Hz con dos Biquad paso-alto en cascada. Restar una copia paso-bajo era el camino obvio y no funciona: `1 - LP(z)` de un Butterworth de 2º orden no es un paso-alto porque la copia filtrada llega desfasada, y a media frecuencia de corte las dos rutas se cancelan sólo -4 dB en vez de los -15 dB que sugiere la magnitud. Encima de eso, la **coherencia de fase** mide la correlación de Pearson inter-canal (integrada 400 ms, resuelta una vez por bloque) y estrecha progresivamente el Side entre r=0.30 y r=-0.20, hasta un ancho mínimo de 0.55, con rampa de 3 s. Nunca ensancha: sólo puede recortar lo que pidió el Smart Engine, así que los dos controles no pelean.

El watchdog del DSP vigila cada muestra. La entrada no finita se reemplaza por silencio y se cuenta antes de tocar cualquier estado recursivo, y se limita a ±4.0 (+12 dBFS) para que una muestra absurda no quede atrapada en los IIR ni llegue a la ruta seca. Si la salida procesada resulta no finita o supera +18 dBFS —sólo posible si un filtro diverge— el motor hace crossfade de 10 ms a la ruta seca retrasada, la sostiene 500 ms tras el último fallo, y limpia los estados envenenados sólo cuando la ruta procesada está muda para que el reset no se oiga. Las líneas de retardo seco y la posición de escritura se conservan: son el audio que el failsafe está reproduciendo. `forceFailsafe` expone el mismo crossfade como botón de pánico, sin contar fallo ni descartar estado. `DspMetrics` publica `failsafeActive`, `failsafeEngagements` y `nonFiniteInputSamples`; el `SafetyController` los convierte en eventos críticos visibles durante 30 s y pide rollback de las correcciones adaptativas.

El A/B compara con volumen perceptualmente igualado: dos integradores de 1.5 s miden la ruta seca y la procesada, y al escuchar A se aplica a la ruta seca la ganancia que iguala ambos niveles (límite ±12 dB, rampa de 250 ms). El bypass nunca se iguala: debe seguir siendo una ruta de seguridad literal.

## Análisis y Smart Engine

Dos FIFOs SPSC preasignadas trasladan audio input/output. Un thread de prioridad baja calcula FFT 2048 Hann, cinco bandas, RMS/peak/crest/transientes/estéreo y libebur128. Drena todos los chunks pendientes por tick para evitar backlog.

El espectro se promedia con **Welch**: una transformada cada medio solape (1024 muestras) acumulada en potencia y normalizada al publicar. Antes se tomaba una sola ventana de 2048 por tick, es decir un periodograma único —cuya varianza es igual a su propia media— sobre el 43 ms más reciente, descartando unas dos terceras partes del audio del chunk. Todas las decisiones tonales se toman sobre esas bandas, así que ese ruido entraba directo a las correcciones.

Se publican las cinco bandas **dos veces**: como proporción del total y como nivel absoluto en dB. La proporción sola no alcanza porque está normalizada por el total: un bombo fuerte sube la proporción de la banda 0 y baja las otras cuatro en el mismo instante, así que el motor leía un solo evento como exceso de graves *y* como déficit de presencia, y aplicaba las dos correcciones. El nivel absoluto confirma cuál de las dos ocurrió de verdad.

La **densidad de transientes** es una tasa sobre ventana móvil de 5 s. Antes eran dos contadores que corrían durante toda la vida del stream: a los diez minutos de servicio un chunk nuevo movía el promedio en centésimas de porcentaje y la lectura quedaba congelada. `classifyContext` la lee en cinco de sus siete ramas, así que el clasificador de contexto entero dejaba de responder a mitad de cada servicio.

El Smart Engine opera a 2–10 Hz. Mantiene sólo estadísticas y persistencia; nunca conserva audio histórico. Una acción necesita severidad persistente y confianza mínima. Hay presupuesto tonal global, exclusión de clarity cuando harshness es confiable y límites por módulo. Cada severidad de banda se multiplica además por una **confirmación absoluta**: cuánto se movió de verdad esa banda en dB respecto de la baseline, saturando a 1.5 dB. Sólo puede retener una corrección, nunca inventarla, y queda neutra hasta que Auto Tune haya medido una baseline absoluta.

Los rangos tonales dejaron de ser sólo sustractivos. Con WARMTH limitado a 0.8 dB y todas las demás bandas limitadas a cortar, el motor únicamente podía quitar, y el resultado salía limpio y sin vida. WARMTH ahora es un shelf grave de hasta 2.5 dB, CLARITY llega a 3 dB, y el aire pasó de un peak en 12 kHz con Q 0.70 —que abarcaba de 7 a 20 kHz y se sentaba encima del de-esser— a un shelf en 9 kHz que puede subir o bajar. La seguridad estéreo usa correlación y desequilibrio L/R persistente; limita width y balance (máximo ±0.75 dB) con rampa de 5 s. Auto Tune acumula 25 s de estadísticas, crea baseline/perfil y después la baseline sólo deriva cuando no se está corrigiendo.

## Psicoacústica

`Analysis/Psychoacoustics.h` contiene sólo funciones puras: escala Bark de Zwicker, función de dispersión de Schroeder, umbral de enmascaramiento global y SII. Están separadas de todo el cableado de audio precisamente porque son la parte contrastable contra números publicados, y sus pruebas lo hacen (1 kHz = 8.51 Bark, dispersión ≈ 0 dB sobre el propio enmascarador y asimétrica hacia arriba, SII = 1.0 / 0.5 / 0.0 en sus extremos).

Las bandas son las 21 críticas del ANSI S3.5-1997 con su función de importancia del habla, que suma 1. No son pesos ajustados por nosotros.

## Grupos X32 y Smart Masking

Cuando `AutoGroupRouter` resuelve tres stems estables entre los primeros ocho canales de la tarjeta, `GroupMixer` suma voz + música + 0.35 x ambiente hacia la salida estéreo. Cualquier fallo de resolución, canal ausente o ruta inválida cae de inmediato al passthrough estéreo: un servicio no puede quedarse en silencio porque la detección de grupos dudara.

`SmartMaskingController` corre siempre en modo consultivo y publica lo que haría, pero sólo llega al audio si el operador activa Smart Masking en ADVANCED. `MaskingDecision::active` es la decisión; `applied` es lo que efectivamente se aplicó. Así se puede observar un servicio completo antes de encenderlo.

La decisión se toma sobre las 21 bandas críticas y se aplica sobre cuatro zonas (750, 1500, 3000 y 5000 Hz), con máximo 4 dB y rampa de 250 ms. Esa asimetría es deliberada: la dispersión de Schroeder está definida a resolución de banda crítica, pero atenuar a 1 Bark modula la música lo bastante rápido como para oírse como artefacto, y la precisión extra no compra nada cuando la reducción es de un par de dB.

El disparo es el SII, no el solape de bandas. Se calcula el umbral que la música proyecta sobre la voz vía dispersión de Schroeder; si el SII cae bajo 0.75 se reduce, repartiendo el déficit entre zonas según cuánta culpa tiene cada una. La atribución importa y es lo que distingue esto de un ducker multibanda: el enmascaramiento se extiende hacia arriba, así que música a 1.6 kHz entierra una voz a 2.5 kHz, y bajar la música a 2.5 kHz quitaría música que no está ahí dejando intacto al culpable. La reducción se carga a la banda que enmascara, no a la enmascarada. No toca la voz, ni graves ni agudos de la música. Los coeficientes se recalculan una vez por bloque, no por muestra.

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

El Offline Test copia del motor en vivo también el leveller, la EQ dinámica, el de-esser, el tone match, la compatibilidad mono y la coherencia de fase. Antes no lo hacía, y como `DspParameters` trae el leveller apagado mientras la aplicación lo enciende por defecto, toda prueba offline corría sin él.

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
