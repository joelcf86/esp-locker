# ESP Locker - Contexto Actual del Proyecto

## Objetivo

Locker automatico para equipaje. El usuario elige un locker libre en la HMI
Nextion, el ESP32 marca el locker como ocupado e imprime un ticket con QR.
Al escanear el QR de regreso, el ESP32 identifica el locker y lo libera.

El ESP32 solo entrega senales de control a las etapas de potencia de los
actuadores. No alimenta directamente solenoides, cerraduras ni motores.

## Hardware Actual

| Componente | Detalle |
|---|---|
| MCU | ESP32-S3 N16R8 / DevKitC-1 |
| Pantalla | Nextion NX8048P070-011R, UART TTL, 3.3 V |
| Impresora | Kinwodon POS-8360L, USB Printer Class, VID `0416`, PID `5011` |
| Escaner | X7-1000A/JOVI, USB-COM CDC-ACM, VID `0218`, PID `0212` |
| Hub | Atolla USB, escaner e impresora conectados simultaneamente |
| Monitor/upload | Puente CH343 por el puerto UART de la placa |

El ESP32-S3 no tiene Bluetooth Classic. La impresora Bluetooth y
`BluetoothSerial` quedaron eliminados del firmware final.

## Pinout Definitivo

```text
Nextion TX/RX       GPIO17 / GPIO18       UART2 @ 9600
Buzzer activo       GPIO14                salida de control
Liberar todos       GPIO47                boton a GND, INPUT_PULLUP

Actuador locker 1   GPIO1                 salida a etapa de potencia
Actuador locker 2   GPIO2                 salida a etapa de potencia
Actuador locker 3   GPIO4                 salida a etapa de potencia
Actuador locker 4   GPIO5                 salida a etapa de potencia
Actuador locker 5   GPIO6                 salida a etapa de potencia

USB OTG D-          GPIO19
USB OTG D+          GPIO20
USB OTG VBUS        5 V
USB OTG GND         GND comun
```

Los cinco actuadores son **salidas**, no botones. Cada GPIO va solamente a la

Pines reservados y que no deben usarse para GPIO:

- GPIO19/20: USB D-/D+.
- GPIO26-32: Flash del modulo.
- GPIO33-37: PSRAM OPI del N16R8.
- GPIO0, 3, 45 y 46: strapping/arranque.
- GPIO43/44: UART0 por defecto.
- GPIO48: posible LED RGB de la placa.

El puente de pads USB-OTG de la placa debe estar soldado. El hub debe recibir
VBUS y tener GND comun. No conectar fuentes independientes a ambos puertos con
el puente cerrado.

## Firmware Master

Archivo principal:

```text
esp_locker_bt/esp_locker_bt.ino
```

El nombre del directorio conserva `bt` por compatibilidad con el repositorio,
pero el firmware actual ya no usa Bluetooth.

FQBN actual:

```text
esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=default,PSRAM=opi,FlashSize=16M,PartitionScheme=default
```

Subir siempre a `115200`; el CH343 puede fallar a `921600`.

El firmware usa un unico stack USB Host del ESP-IDF. TinyUSB y el host IDF no
se usan juntos: TinyUSB no incluye clase Printer y ambos stacks no pueden ser
duenos simultaneos del periferico OTG.

El CDC del escaner se implementa manualmente:

- `SET_LINE_CODING`: 9600, 8N1.
- `SET_CONTROL_LINE_STATE`: DTR + RTS.
- Lectura bulk IN del endpoint `0x81`.
- Terminadores aceptados: TAB, CR y LF.
- Deduplicacion de payload repetido: 2 segundos.

La impresora usa bulk OUT en el endpoint `0x01` y comandos ESC/POS. El QR se
construye con `GS ( k`, sin libreria adicional.

## Estado y Persistencia

`lockerState[5]` es la fuente de verdad. `FREE` y `OCCUPIED` se guardan en
`Preferences`, namespace `locker`:

```text
lk0..lk4    estado de cada locker
opc         contador de operaciones
lang        idioma del ticket: 0=espanol, 1=ingles
```

`openLocker()` marca `OCCUPIED` antes de imprimir para evitar tickets
duplicados. Si la impresion falla, el locker queda ocupado y debe liberarse
manualmente.

El firmware actual inicializa los cinco GPIO de actuadores en LOW, pero aun no
implementa el pulso fisico de apertura. La integracion pendiente es activar el
GPIO correcto al liberar y luego marcar el locker como libre.

## Flujo Actual

1. Nextion envia la seleccion del locker.
2. `openLocker()` valida que este libre.
3. Se genera `operationId` y token de 64 bits.
4. Se guarda el estado ocupado.
5. Se imprime el ticket por USB.
6. Nextion recibe el nuevo bitmap de estados.
7. El escaner lee el QR del ticket.
8. El firmware extrae el numero de locker y llama `freeLocker()`.
9. `freeLocker()` guarda estado libre y actualiza Nextion.

Actualmente el QR se parsea por formato, pero el token no se valida contra un
token persistido por locker. La validacion de token de un solo uso queda como
mejora de seguridad pendiente.

## Protocolo Nextion

Todos los comandos son cadenas terminadas por `FF FF FF`.

### Nextion hacia ESP32

```text
LOCKER:<n>     seleccionar y ocupar locker n
FREE=<n>       liberar locker n manualmente
LANG:ES        imprimir siguientes tickets en espanol
LANG:EN        imprimir siguientes tickets en ingles
BEEP           activar buzzer
ready          solicitar sincronizacion de estados
```

Tambien se aceptan `LANG=ES` y `LANG=EN`.

El idioma se debe enviar antes de `LOCKER:<n>`. Ejemplos:

```text
LANG:ES
LOCKER:5
```

```text
LANG:EN
LOCKER:5
```

El idioma se guarda en NVS y por defecto es espanol.

### ESP32 hacia Nextion

```text
LOCKER_OK=<n>
LOCKER_BUSY
LOCKER_STATE=<bitmap>
LANG_OK=ES
LANG_OK=EN
```

`LOCKER_STATE` usa un bit por locker: bit 0 = locker 1, hasta bit 4 = locker
5.

## Ticket e Idiomas

El texto del ticket se selecciona con `lang`:

Español:

```text
Escanea este codigo QR
para identificar tu locker:
Conserva este ticket
para recoger tus pertenencias
Tu espacio, mas simple
```

English:

```text
Scan this QR code
to identify your locker:
Keep this ticket
to collect your belongings
Your space, made simple
```

El encabezado, numero de locker, token y QR son iguales en ambos idiomas.

## Formatos QR Aceptados

El ticket generado usa:

```text
L=<locker>&I=<operationId>&T=<token>
```

Ejemplo:

```text
L=3&I=1&T=8A72F41C9B31D6E5
```

El parser del firmware tambien acepta:

```text
LOCKER:<n>
```

Para liberar, actualmente se valida el formato y el numero `1..5`; no se
valida aun la autenticidad del token.

## Pruebas USB Verificadas

Sketches del repositorio:

```text
scanner_test/scanner_test.ino       prueba individual CDC
printer_test/printer_test.ino       prueba individual Printer Class
hub_test/hub_test.ino               enumeracion y descriptores del hub
usb_combo_test/usb_combo_test.ino   escaner + impresora simultaneos
```

Resultados confirmados:

- Escaner CDC directo: payload correcto y terminador TAB.
- Impresora USB directa: transferencia ESC/POS completada.
- Hub Atolla: ambos dispositivos enumeran simultaneamente.
- Combo: lectura del escaner e impresion por el mismo USB Host IDF.
- Payload real capturado: `L=5&I=3&T=981897898D90E92C`.

## Diagnostico por Monitor

```text
free <n>    liberar locker n
open <n>    ocupar e imprimir locker n para prueba
state       mostrar bitmap actual
boot wipe   borrar estado persistido
```

El monitor normal va por el CH343 en `/dev/ttyACM0`. `arduino-cli monitor`
puede mantener DTR/RTS en reset; para leer se usa el script local
`/tmp/opencode/serial_read.py`.

## Tests Pendientes

- Conectar la HMI con los comandos `LANG:ES` y `LANG:EN`.
- Confirmar ticket completo en ambos idiomas.
- Implementar pulso de cada actuador mediante su etapa de potencia.
- Definir polaridad y tiempo de pulso de los actuadores.
- Validar token persistido por locker antes de liberar en produccion.
- Verificar sincronizacion visual de `lk1..lk5` tras ocupacion y liberacion.
