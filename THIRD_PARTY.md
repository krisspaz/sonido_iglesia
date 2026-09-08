# Componentes de terceros

- JUCE 8.0.15 — fijado mediante CMake FetchContent. Revisar y cumplir la licencia JUCE aplicable antes de distribución comercial/cerrada.
- libebur128 1.2.6 — implementación local EBU R128, fijada mediante CMake FetchContent.
- OBS Studio WebSocket protocol v5 — comunicación local compatible; OBS no se redistribuye aquí.
- Microsoft Windows Driver Samples / SysVAD — revisión `717778a20ba4dd2440fe609f69153a1f8a64f597`, base del código reproducible del endpoint virtual, bajo MS-PL incluida en `driver/MS-PL.txt`.

El repositorio no redistribuye el driver Behringer, OBS, certificados de firma ni un binario kernel firmado por terceros. El binario de producción se construye desde la revisión fijada y debe recibir firma Microsoft antes de distribuirse.
