# Driver virtual Church Stream Processor

El repositorio ya contiene el código reproducible del cable virtual, no un `.sys` ficticio. Se construye como una modificación pequeña y auditable de SysVAD oficial de Microsoft:

- upstream fijado: `microsoft/Windows-driver-samples` commit `717778a20ba4dd2440fe609f69153a1f8a64f597`;
- licencia upstream: MS-PL, incluida en `driver/MS-PL.txt`;
- modificación: `driver/patches/0001-church-stream-virtual-cable.patch`;
- render: `Church Stream Processor Input`, donde escribe la aplicación;
- capture: `Church Stream Processor Output`, que recibe OBS;
- formato de cable: PCM estéreo, 48 kHz, 16 bit;
- transporte: ring buffer no paginado de 192000 bytes, preasignado y protegido por spin lock;
- endpoints instalados: un render y un capture; no hay red, servicio de usuario ni audio guardado.

La revisión y el SHA-256 de la modificación están fijados en `driver/upstream.lock.json`. CTest valida ese manifiesto en cada build.

## Construir el paquete WDK

Requisitos en Windows 10/11 x64:

- Visual Studio 2022 con Desktop C++;
- Windows SDK y Windows Driver Kit;
- Git;
- PowerShell 5.1 o posterior.

Desde la raíz:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-virtual-driver.ps1 -Configuration Release -Platform x64
```

El script descarga exclusivamente la revisión fijada, comprueba el hash, aplica el patch, elimina sideband Bluetooth/USB, deja sólo el par estéreo, genera nombres/ID propios y ejecuta MSBuild con `SignMode=Off`. El resultado queda en:

```text
out\virtual-driver\package-unsigned\
├── ChurchStreamVirtual.inf
├── ChurchStreamVirtual.sys
├── ChurchStreamVirtual.cat
└── MS-PL.txt
```

`package-unsigned` es deliberadamente un artefacto de desarrollo. Nunca se copia automáticamente a `dist/driver` ni puede pasar `validate-driver.ps1` como producción.

Para preparar el árbol sin compilar:

```powershell
.\scripts\build-virtual-driver.ps1 -PrepareOnly
```

## Firma de producción

Windows 10/11 x64 exige una cadena de firma kernel aceptada. El paso externo es enviar el paquete a Microsoft Hardware Dev Center para attestation signing o completar HLK/WHQL. Requiere la identidad/certificados de la organización y no puede fabricarse dentro del repositorio.

Para crear el CAB de envío y firmarlo con el certificado de la organización:

```powershell
.\scripts\create-driver-submission.ps1 -CertificateThumbprint 'THUMBPRINT_EV'
```

El CAB se guarda en `out\virtual-driver\submission\`. Su carga y aprobación se realizan en Hardware Dev Center.

Después de descargar el paquete firmado por Microsoft:

```powershell
.\scripts\import-signed-driver.ps1 -SignedPackageDirectory 'C:\ruta\paquete-firmado'
```

Ese comando exige `signtool verify /kp`, comprueba que INF y SYS estén cubiertos por el catálogo y sólo entonces copia los archivos a `dist/driver`. A continuación:

```powershell
.\scripts\build-windows.ps1 -Installer
```

El instalador usa `pnputil /add-driver ... /install`; la desinstalación detiene la aplicación y elimina el paquete PnP.

## Validación obligatoria

Antes de declarar producción:

1. compilar Release x64 con WDK sin warnings ni errores;
2. pasar Static Driver Verifier, Driver Verifier e InfVerif;
3. instalar el catálogo Microsoft-signed sin Test Mode en Windows 10 y 11 limpios;
4. confirmar render `Church Stream Processor Input` y capture `Church Stream Processor Output` a 48 kHz estéreo;
5. enviar audio identificable app → render → capture → OBS y verificar L/R, continuidad y silencio cuando no hay render;
6. probar suspensión, actualización, desconexión, reinstalación y desinstalación;
7. ejecutar 8 h con X32 + OBS + encoder y registrar xruns, CPU, RAM y latencia.

La aplicación, el patch y el proceso de empaquetado están preparados; la firma Microsoft y la prueba física siguen siendo gates externos verificables, no resultados simulados.
