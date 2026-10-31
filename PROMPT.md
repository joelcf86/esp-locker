# ESP Locker — Contexto para Continuar el Proyecto

## Qué es

Locker automático para equipo (maletas/bolsas). El usuario elige un casillero
libre en una pantalla Nextion, el ESP32 emite un ticket impreso con QR, y al
escanear ese QR se libera el casillero.

El QR **no es decorative**: es el mecanismo de retiro. Al escanearlo, el ESP32
valida el token y marca el casillero como libre.

## Hardware

| Componente | Detalle |
|---|---|
| MCU | ESP32-D0WD rev 1.0 (WROOM-32, DevKit V1 clónico) |
| Pantalla | Nextion NX8048P070-011R, UART TTL 3.3V |
| Impresora | POS-8360, Bluetooth SPP, MAC `66:32:9F:BC:FC:73` |
| Escáner | JOVI omnidireccional 2D/QR, USB HID |
| USB Host | Shield con MAX3421E (controlador externo por SPI) |

### Pinout

```
Nextion UART   TX GPIO 17   RX GPIO 16        Serial1 @ 115200
Botones        GPIO 23, 4, 13, 18, 19        INPUT_PULLUP, botón a GND
USB Host SPI   SCK 14  MISO 25  MOSI 26  CS 27
```

Decisiones de pinout y sus motivos:

- **Pines de strapping evitados.** El ESP32 lee GPIO 0/2/5/12/15 en el reset.
  GPIO 12 además configura el flash a 1.8V si queda HIGH, lo que impide el boot.
  Por eso el SPI usa 25/26/27 (los DAC, libres y no strapping) en vez de los
  pines habituales del HSPI que caen en 12/15.
- **GPIO 34-39 no sirven para botones.** No tienen pull-up interno.
- **El SPI por defecto del ESP32 es un conflicto.** Usa 18/19/23/5, que serían
  tres botones. Hay que pasar los pines explícitamente.
- Botones → lockers: GPIO 23→1, 4→4, 13→2, 18→3, 19→5.

## Firmware

`esp_locker_bt/esp_locker_bt.ino` — single-file, compila con
`--fqbn esp32:esp32:esp32`. Core `esp32:esp32` 3.3.12.

### Estructura

- `lockerState[5]` — `FREE` / `OCCUPIED`. **Es la fuente de verdad.**
  La pantalla Nextion solo refleja; no decide.
- `openLocker(n, origen)` — camino único de apertura. Lo usan tanto los botones
  como la pantalla, así el estado nunca se desincroniza entre las dos interfaces.
- `freeLocker(n)` — libera y persiste.
- `generateToken(n)` — genera token de 64 bits (`2 × esp_random()`) e incrementa
  `operationCounter`.

### Orden de las operaciones en `openLocker`

El casillero se marca `OCCUPIED` **antes** de imprimir, no después.

Es deliberado. Marcando después, si la impresión falla o el ESP32 se cuelga a
mitad del ticket, el casillero queda libre y el usuario puede pedir un segundo
ticket del mismo casillero. Marcando primero eso es imposible. El costo es el
inverso: si la impresión falla, el casillero queda bloqueado y hay que liberarlo
a mano.

### Persistencia

`Preferences` con namespace `locker`:
- `lk0`..`lk4` → estado de cada casillero
- `opc` → contador de operaciones (para que los IDs no se repitan tras reinicio)

### Comandos de diagnóstico (monitor USB @ 115200)

```
free <n>    libera el casillero n
state       imprime el bitmap actual
boot wipe   borra todo el estado persistido
```

## Protocolo Nextion

El ESP32 no conoce nombres de componentes: el protocolo son cadenas de texto,
y el lado Nextion las parsea con un script en *Run tC*.

```
Nextion → ESP32        ESP32 → Nextion
  LOCKER=<n>              LOCKER_OK=<n>
  FREE=<n>                LOCKER_BUSY
  ready                   LOCKER_STATE=<bitmap>
```

`LOCKER_STATE` lleva un bit por casillero: bit 0 = locker 1 … bit 4 = locker 5.
Se envía tras cada apertura y cuando la Nextion manda `ready`, así la pantalla
se sincroniza sola al arrancar.

## Formato del QR

```
L=<locker>&I=<operationId>&T=<token>
```

Ejemplo: `L=3&I=1&T=8A72F41C9B31D6E5`

El QR contiene **solo** los datos, sin etiqueta, para que el lector reciba el
valor limpio. El texto legible va impreso aparte en el ticket.

Delimitador de parseo: `&` y `=`. El charset alfanumérico de la spec QR no
incluye `=` ni `&`, así que la impresora cae a modo byte y el QR sale algo más
grande de lo mínimo. Funciona igual.

## Scanner — hallazgos verificados en hardware

```
VID:PID     0218:0210
Producto    USBScn Chip USBScn Module   (chip OEM genérico)
Interfaz    class 0x03 HID, subclass 0x01 boot, protocol 0x01 keyboard
Driver      usbhid  →  /dev/hidraw0
Velocidad   12 Mb/s  = full-speed
Terminador  TAB (0x09)
```

**No es CDC.** No aparece ningún `/dev/ttyACM`. El QR llega como caracteres
tecleados, no como puerto serie. Por eso el parser HID tiene que filtrar los
modificadores: si el escáner manda `&` o `=` en modo shift, el reporte HID trae
el bit de modificador seteado y el keycode de la tecla sin shift.

El TAB es un buen delimitador para este caso: el payload solo contiene A-Z,
dígitos, `=` y `&`, así que el TAB nunca puede aparecer adentro del string.

**El "USB 3.0" del producto es marketing.** El enlace real es full-speed 12
Mbps, igual al techo del MAX3421E. El MAX3421E es USB 1.1 y no puede hacer
high-speed; su límite es hardware, ningún hub lo cambia. Los controladores
SuperSpeed reales (Terminus CSS3720, Microchip USB5740) son chips PCIe, no
attachables por SPI como el MAX3421E. Para códigos de barras la diferencia es
irrelevante: 26 bytes de payload contra 1.5 Mbps.

El MAX3421E sí soporta hubs, hasta 7 puertos. Eso resuelve la corriente del
escáner sin necesidad de USB 3.0.

## Impresora — secuencia ESC/POS del QR

Comandos nativos `GS ( k` (fn = 65/67/69/80/81). El QR se imprime **sin
librerías**, codificado por el hardware de la impresora.

```
1D 28 6B 04 00 31 41 32 00              modelo 2
1D 28 6B 03 00 31 43 06                 módulo = 6 dots
1D 28 6B 03 00 31 45 31                 corrección M (48=L 49=M 50=Q 51=H)
1D 28 6B <k+3> 00 31 50 30 <datos>      guardar
1D 28 6B 03 00 31 51 30                 imprimir
```

Dos trampas que costaron tiempo:

- **`fn = 80` (0x50) solo guarda los datos, no imprime.** Sin el comando
  `fn = 81` posterior no sale nada.
- **`pL = k + 3`** (longitud del payload + 3). Confirmado con el manual de
  Rongta: `1d 28 6b 06 00 31 50 30 41 42 43` con datos "ABC" → `pL = 6 = 3 + 3`.
- El byte `0x31` es `cn` y va **después** de `pL`/`pH`, según el formato
  `GS ( k pL pH cn fn`. Mandarlo antes rompe el parseo.

## Bluetooth SPP de la impresora

`BluetoothSerial`, maestro (`begin("ESP_LOCKER", true)`).

- **`begin()` devuelve `true` al éxito.** Escribir `if (begin(...))` como error
  es el bug clásico.
- **La MAC va en el orden nativo de la ESP-IDF**, no invertido. Esta impresora
  reporta `66:32:9F:BC:FC:73`, idéntico a la etiqueta. Confirmado con un modo
  scan que imprime lo que ve el ESP32.
- `sec_mask` default es `ENCRYPT | AUTHENTICATE`. Con `ESP_SPP_SEC_NONE`
  conecta sin emparejamiento.
- Si la impresora queda con un enlace SPP huérfano tras un flash, **reiniciarla
  a mano** antes de recargar.

## Limitaciones conocidas

- **`printTicket()` bloquea ~3.5 s.** Durante la impresión no se lee el UART ni
  los botones. Sin riesgo de ticket duplicado (por el orden de marcado), pero sí
  latencia perceptible en la pantalla.
- **No hay flujo de retiro implementado.** `freeLocker()` existe pero solo se
  dispara por consola o por `FREE=` desde la Nextion. Falta la lectura del
  escáner.
- `BluetoothSerial` está marcada `[[deprecated]]` en el core 3.3.12 y va a
  desaparecer por defecto en 4.0.0.

## Decisión abierta

**Validación del retiro: local o backend.**

Pendiente de definir. La alternativa de producción es que el backend verifique
el token, porque los tokens de un solo uso se pueden reutilizar si alguien
reimprime el ticket. Si es local, basta guardar el token emitido en `Preferences`
y comparar al escanear.
## Actualización de hardware: scanner e impresora USB

### Scanner X7-1000A

Configurado de USB-HID a USB-COM. Windows lo reconoce como COM8 y a 9600 8N1 entrega correctamente `L=5&I=3&T=981897898D90E92C`. El scanner sigue siendo USB y necesita USB Host; la prueba futura debe usar USB CDC/COM, no HID.

### Impresora Kinwodon POS-8360L

Interfaces USB + Bluetooth. Por USB Windows la identifica como `STMicroelectronics POS80 printer USB`, VID `0416`, PID `5011`. Es una impresora POS/ESC-POS USB, normalmente USB Printer unidireccional. Se conservarán los bytes ESC/POS y se cambiará solamente el transporte Bluetooth por USB.

### Plataforma ESP32-S3 prevista

Se migrará a ESP32-S3 N16R8/DevKitC-1 de 44 pines con dos USB-C: COM para USB-UART/programación y USB para USB OTG nativo. El S3 no tiene Bluetooth Classic, por lo que `BluetoothSerial` no se usará; scanner e impresora irán por USB.

### Hub Atolla y puente USB-OTG

Topología prevista:

```text
ESP32-S3 USB OTG -> adaptador USB-C OTG/A -> upstream hub Atolla
                                             ├── scanner X7-1000A
                                             └── impresora POS-8360L
```

El hub usará su adaptador externo. La placa tiene dos pads `USB-OTG` inferiores; unirlos con soldadura, alimentar la placa por COM y verificar aproximadamente 5 V en VBUS del puerto USB. No conectar fuentes independientes a ambos puertos con el puente cerrado. El puente solo distribuye VBUS y no implementa USB Host.

GPIO19 y GPIO20 son USB D- y D+; el botón que usaba GPIO19 deberá moverse a otro GPIO cuando USB OTG esté activo.

### Orden de implementación pendiente

1. Sketch independiente ESP32-S3: detectar scanner USB-COM y mostrar el payload.
2. Probar scanner mediante el hub Atolla.
3. Detectar impresora VID 0416/PID 5011 y enviar prueba ESC/POS.
4. Probar scanner e impresora simultáneamente mediante el hub.
5. Integrar ambos drivers al firmware master.
6. Implementar validación del token y liberación del locker.

### Actuadores de liberación

Cada locker tendrá una salida independiente hacia su propia etapa de potencia.
El ESP32-S3 no alimentará directamente los actuadores; únicamente entregará la
señal lógica de control a cada etapa. La etapa deberá incluir la protección
correspondiente al tipo de actuador, por ejemplo diodo flyback para solenoides.

Asignación GPIO provisional, pendiente de confirmar con el pinout físico de la placa:

| Locker | GPIO de control |
|---|---:|
| 1 | GPIO1 |
| 2 | GPIO2 |
| 3 | GPIO4 |
| 4 | GPIO5 |
| 5 | GPIO6 |

GPIO19 y GPIO20 quedan reservados para USB D- y D+. El botón que actualmente usa
GPIO19 deberá reasignarse antes de activar USB OTG.

Secuencia prevista al retirar un locker:

```text
validar QR → seleccionar salida 1..5 → activar etapa de potencia
→ mantener el pulso configurado → desactivar salida
→ marcar locker libre → guardar estado → actualizar Nextion
```

El tiempo de pulso y la polaridad activa (`HIGH`/`LOW`) todavía deben confirmarse
con el actuador y su etapa de potencia reales.

## Estado actual de la HMI Nextion

Proyecto revisado:

```text
C:\Users\Joel Calderon\Documents\Lockers\BagPackPicker\Locker_BagPack.HMI
```

La lógica comprobada hasta esta etapa es la siguiente:

- `lk1` a `lk5` representan el estado visual de los cinco lockers.
- `sel` y `va0` almacenan la selección actual.
- Al seleccionar un locker se muestra el botón de confirmación.
- Al confirmar, el locker seleccionado cambia a la imagen roja (`pic=11`) y
  se desactiva su toque mediante `tsw pX,0`.
- El temporizador `tm0` controla la animación de generación del QR.
- En el ciclo 7 se transmite el locker seleccionado al ESP32.
- En el ciclo 10 se detiene el temporizador, se reactiva el toque y se vuelve a
  `page1`.
- La HMI envía `BEEP` terminado con `0A 0D` para activar el buzzer del ESP32.

### Modo actual de presentación

La inicialización contiene intencionalmente:

```nextion
lk1=0
lk2=0
lk3=0
lk4=0
lk5=0
```

Esto es correcto para la demostración actual, porque permite reiniciar la pantalla
con todos los lockers libres mientras todavía no existe scanner para liberarlos.
Por ese motivo, actualmente no se usan `repo` ni `wepo`.

### Pendientes para producción

Cuando se integre el scanner USB-COM y la liberación física:

1. Restaurar `repo lkX,<dirección>` en la inicialización.
2. Añadir `wepo lkX,<dirección>` al confirmar la ocupación.
3. Recibir del ESP32 la liberación después de validar el QR.
4. Cambiar el locker liberado a verde y reactivar su toque.
5. Sincronizar los cinco estados después de reiniciar o reconectar la HMI.

La HMI actualmente transmite el comando de selección con dos puntos:

```text
LOCKER:<n>
```

El firmware clásico actual utiliza ese formato. Durante la migración al ESP32-S3
debe conservarse o normalizarse explícitamente a `LOCKER=<n>`; ambos lados deben
usar exactamente el mismo protocolo.
