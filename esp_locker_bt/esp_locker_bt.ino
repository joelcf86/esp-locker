/*
 * ESP Locker - firmware master
 *
 * Flujo:
 *   - La HMI Nextion envia "LOCKER:<n>" al elegir un casillero.
 *   - El firmware marca el casillero OCCUPIED, imprime el ticket con QR
 *     (ESC/POS por USB) y avisa a la Nextion.
 *   - El escaner USB (CDC-ACM) lee el QR del ticket ("L=<n>&I=..&T=..")
 *     o un QR "LOCKER:<n>", y libera ese casillero.
 *
 * Arquitectura USB:
 *   - Un unico stack USB Host del ESP-IDF maneja hub + impresora + escaner.
 *     (TinyUSB no tiene clase printer y no puede coexistir con este stack.)
 *   - Impresora: clase 0x07 Printer, bulk OUT.
 *   - Escaner: clase 0x02/0x0A CDC-ACM, hecho a mano (SET_LINE_CODING + bulk IN).
 *
 * Compilar con:
 *   FQBN esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=default,PSRAM=opi,
 *        FlashSize=16M,PartitionScheme=default,UploadSpeed=115200
 *   - USBMode=hwcdc desactiva TinyUSB y deja libre el periferico OTG.
 *   - PSRAM=opi usa la memoria del modulo; GPIO33-37 quedan reservados.
 *   - El monitor/upload va por el CH343 del puerto UART.
 *
 * Pines:
 *   Nextion TX/RX 17/18 @9600, buzzer 14, liberar todo 47.
 *   Actuadores de locker 1..5: GPIO1,2,4,5,6.
 *   GPIO19/20 quedan para USB D-/D+ (host OTG): no usar como GPIO.
 */

#include "Preferences.h"
#include "HardwareSerial.h"
#include <stdarg.h>

#include "soc/soc_caps.h"
#if !SOC_USB_OTG_SUPPORTED
#error "Este firmware necesita un target con USB OTG (ESP32-S3/P4)."
#endif

#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

extern "C" void USBHostBoardInit(void);

#define LOCKER_COUNT 5
#define NEXTION_BAUD 9600
#define NEXTION_RX 18
#define NEXTION_TX 17
#define BUZZER_PIN 14
#define RELEASE_ALL_BUTTON 47

#define MAX_OPEN_DEVICES 8
#define TRANSFER_TIMEOUT 5000
#define SCAN_DEDUP_MS     2000
#define ACTUATOR_PULSE_MS 2000

HardwareSerial nextionSerial(2);

enum LockerState { FREE = 0, OCCUPIED = 1 };
enum PrintLanguage { LANGUAGE_ES = 0, LANGUAGE_EN = 1 };

struct LockerSession {
  uint8_t locker;
  uint32_t operationId;
  uint64_t token;
  bool active;
};

LockerSession currentSession;

const uint8_t actuatorPins[LOCKER_COUNT] = {
  1, 2, 4, 5, 6
};

Preferences prefs;
uint8_t lockerState[LOCKER_COUNT];
uint32_t operationCounter = 0;
PrintLanguage printLanguage = LANGUAGE_ES;

int lastReading[LOCKER_COUNT];
unsigned long lastDebounce[LOCKER_COUNT];
bool buttonLatched[LOCKER_COUNT];

char nextionLine[32];
uint8_t nextionLen = 0;

/* ------------------------------------------------------------------ */
/* USB host                                                           */
/* ------------------------------------------------------------------ */

struct PrintJob {
  usb_device_handle_t dev;
  uint8_t  iface;
  uint8_t  alt;
  uint8_t  out_ep;
  uint16_t mps;
  bool     valid;
};

struct CdcJob {
  usb_device_handle_t dev;
  uint8_t  comm_iface;
  uint8_t  data_iface;
  uint8_t  alt;
  uint8_t  in_ep;
  uint16_t mps;
  bool     valid;
};

static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t s_devices[MAX_OPEN_DEVICES];
static QueueHandle_t s_cdcQueue = NULL;
static QueueHandle_t s_scanQueue = NULL;
static SemaphoreHandle_t s_printerSem = NULL;
static SemaphoreHandle_t s_scannerSem = NULL;
static PrintJob s_printer = {};

static char s_scanLine[64];
static uint8_t s_scanLen = 0;
static char lastScanPayload[64] = "";
static uint32_t lastScanMs = 0;

static uint8_t g_ticket[1024];
static size_t g_ticketLen = 0;

#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
static bool enumFilter(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  (void)dev_desc;
  *bConfigurationValue = 1;
  return true;
}
#endif

static void printerCb(usb_transfer_t *t) { (void)t; xSemaphoreGive(s_printerSem); }
static void scannerCb(usb_transfer_t *t) { (void)t; xSemaphoreGive(s_scannerSem); }

/* ------------------------------------------------------------------ */
/* Helpers de estado / Nextion                                        */
/* ------------------------------------------------------------------ */

String tokenToHex(uint64_t token){
  char buffer[17];
  sprintf(buffer, "%016llX", token);
  return String(buffer);
}

void lockerKey(char *out, uint8_t idx){
  snprintf(out, 8, "lk%d", idx);
}

uint8_t stateMask(){
  uint8_t mask = 0;
  for (uint8_t i = 0; i < LOCKER_COUNT; i++){
    if (lockerState[i] == OCCUPIED) mask |= (1 << i);
  }
  return mask;
}

void sendToNextion(const char *msg){
  nextionSerial.print(msg);
  nextionSerial.write(0xFF);
  nextionSerial.write(0xFF);
  nextionSerial.write(0xFF);
  nextionSerial.flush();
}

void loadState(){
  char key[8];
  for (uint8_t i = 0; i < LOCKER_COUNT; i++){
    lockerKey(key, i);
    lockerState[i] = prefs.getBool(key, false) ? OCCUPIED : FREE;
  }
  operationCounter = prefs.getULong("opc", 0);
  printLanguage = prefs.getUChar("lang", LANGUAGE_ES) == LANGUAGE_EN
                    ? LANGUAGE_EN : LANGUAGE_ES;
}

void setPrintLanguage(PrintLanguage language){
  printLanguage = language;
  prefs.putUChar("lang", (uint8_t)language);
  Serial.printf("Idioma del ticket: %s\n",
                language == LANGUAGE_EN ? "ENGLISH" : "ESPANOL");
}

void generateToken(uint8_t locker){
  currentSession.locker = locker;
  currentSession.operationId = ++operationCounter;
  prefs.putULong("opc", operationCounter);

  uint32_t high = esp_random();
  uint32_t low = esp_random();

  currentSession.token = ((uint64_t)high << 32 | low);
  currentSession.active = true;

  Serial.println();
  Serial.println("==== NUEVA SESION ====");
  Serial.print("Locker: ");
  Serial.println(currentSession.locker);
  Serial.print("Operacion: ");
  Serial.println(currentSession.operationId);
  Serial.print("Token: ");
  Serial.println(tokenToHex(currentSession.token));
  Serial.println("=======================");
}

String buildQrPayload(){
  String token = tokenToHex(currentSession.token);
  return "L=" + String(currentSession.locker)
       + "&I=" + String(currentSession.operationId)
       + "&T=" + token;
}

void beep(){
  digitalWrite(BUZZER_PIN, HIGH);
  delay(180);
  digitalWrite(BUZZER_PIN, LOW);
  delay(60);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(250);
  digitalWrite(BUZZER_PIN, LOW);
}

void pulseActuator(uint8_t locker){
  if (locker < 1 || locker > LOCKER_COUNT) return;

  uint8_t pin = actuatorPins[locker - 1];
  Serial.printf("Actuador locker %d ON (%u ms)\n", locker, ACTUATOR_PULSE_MS);
  digitalWrite(pin, HIGH);
  delay(ACTUATOR_PULSE_MS);
  digitalWrite(pin, LOW);
  Serial.printf("Actuador locker %d OFF\n", locker);
}

/* ------------------------------------------------------------------ */
/* Buffer ESC/POS + QR                                                */
/* ------------------------------------------------------------------ */

static void tkByte(uint8_t b){
  if (g_ticketLen < sizeof(g_ticket)) g_ticket[g_ticketLen++] = b;
}

static void tkBytes(const uint8_t *p, size_t n){
  for (size_t i = 0; i < n; i++) tkByte(p[i]);
}

static void tkText(const char *s){
  while (*s) tkByte((uint8_t)*s++);
}

static void tkPrintf(const char *fmt, ...){
  char tmp[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  tkText(tmp);
}

static void tkQr(const char *data){
  static const uint8_t model[]   = {0x1D,0x28,0x6B,0x04,0x00,0x31,0x41,0x32,0x00};
  static const uint8_t modsize[] = {0x1D,0x28,0x6B,0x03,0x00,0x31,0x43,0x06};
  static const uint8_t ecc[]     = {0x1D,0x28,0x6B,0x03,0x00,0x31,0x45,0x31};
  static const uint8_t printCmd[]= {0x1D,0x28,0x6B,0x03,0x00,0x31,0x51,0x30};

  uint16_t payloadLen = (uint16_t)(strlen(data) + 3);
  uint8_t store[8] = {0x1D,0x28,0x6B,(uint8_t)(payloadLen & 0xFF),
                      (uint8_t)(payloadLen >> 8),0x31,0x50,0x30};

  tkBytes(model, sizeof(model));
  tkBytes(modsize, sizeof(modsize));
  tkBytes(ecc, sizeof(ecc));
  tkBytes(store, sizeof(store));
  tkText(data);
  tkBytes(printCmd, sizeof(printCmd));
}

static bool usbPrint(const uint8_t *data, size_t len){
  if (len == 0) return false;

  if (!s_printer.valid){
    Serial.println("[print] impresora no disponible");
    return false;
  }

  usb_transfer_t *xfer = NULL;
  if (usb_host_transfer_alloc(len, 0, &xfer) != ESP_OK || xfer == NULL){
    Serial.println("[print] alloc fallo");
    return false;
  }

  memcpy(xfer->data_buffer, data, len);
  xfer->device_handle    = s_printer.dev;
  xfer->bEndpointAddress = s_printer.out_ep;
  xfer->num_bytes        = len;
  xfer->timeout_ms       = TRANSFER_TIMEOUT;
  xfer->callback         = printerCb;
  xfer->context          = NULL;

  esp_err_t err = usb_host_transfer_submit(xfer);
  bool ok = false;

  if (err != ESP_OK){
    Serial.printf("[print] submit fallo: %s\n", esp_err_to_name(err));
  } else if (xSemaphoreTake(s_printerSem, pdMS_TO_TICKS(TRANSFER_TIMEOUT)) == pdTRUE){
    ok = (xfer->status == USB_TRANSFER_STATUS_COMPLETED);
    Serial.printf("[print] status=%d actual=%d/%u\n", (int)xfer->status,
                  xfer->actual_num_bytes, (unsigned)len);
  } else {
    Serial.println("[print] timeout");
    usb_host_endpoint_flush(s_printer.dev, s_printer.out_ep);
    xSemaphoreTake(s_printerSem, pdMS_TO_TICKS(1000));
  }

  usb_host_transfer_free(xfer);
  return ok;
}

void printTicket(){
  g_ticketLen = 0;

  tkByte(0x1B); tkByte(0x40);                       // ESC @ init
  tkByte(0x1B); tkByte(0x61); tkByte(0x01);         // centrar
  tkText("============================\n");
  tkText("ACCESS LOCKERS\n");
  tkText("============================\n");
  tkPrintf("LOCKER #%d\n", currentSession.locker);
  tkText("\n");
  if (printLanguage == LANGUAGE_EN){
    tkText("Scan this QR code\n");
    tkText("to identify your locker:\n");
  } else {
    tkText("Escanea este codigo QR\n");
    tkText("para identificar tu locker:\n");
  }
  tkText("\n");

  tkQr(buildQrPayload().c_str());

  tkPrintf("TOKEN: %s\n", tokenToHex(currentSession.token).c_str());
  tkText("\n");
  if (printLanguage == LANGUAGE_EN){
    tkText("Keep this ticket\n");
    tkText("to collect your belongings\n");
  } else {
    tkText("Conserva este ticket\n");
    tkText("para recoger tus pertenencias\n");
  }
  tkText("\n");
  tkText("ACCESS\n");
  tkText(printLanguage == LANGUAGE_EN ? "Your space, made simple\n"
                                     : "Tu espacio, mas simple\n");
  tkText("\n");

  tkByte(0x1B); tkByte(0x64); tkByte(0x06);         // feed 6
  tkByte(0x1D); tkByte(0x56); tkByte(0x00);         // corte

  Serial.printf("[print] ticket locker %d, %u bytes\n",
                currentSession.locker, (unsigned)g_ticketLen);

  if (usbPrint(g_ticket, g_ticketLen)){
    Serial.println("Ticket impreso");
  } else {
    Serial.println("Ticket NO impreso (impresora no disponible)");
  }
  delay(600);
}

/* ------------------------------------------------------------------ */
/* Logica de lockers                                                  */
/* ------------------------------------------------------------------ */

bool openLocker(uint8_t locker, const char *origin){
  if (locker < 1 || locker > LOCKER_COUNT){
    Serial.printf("Locker %d inexistente (%s)\n", locker, origin);
    sendToNextion("LOCKER_BUSY");
    return false;
  }

  uint8_t idx = locker - 1;

  if (lockerState[idx] == OCCUPIED){
    Serial.printf("Locker %d ya ocupado, no se imprime (%s)\n", locker, origin);
    sendToNextion("LOCKER_BUSY");
    return false;
  }

  lockerState[idx] = OCCUPIED;
  char key[8];
  lockerKey(key, idx);
  prefs.putBool(key, true);

  Serial.printf("Locker %d abierto (%s)\n", locker, origin);

  generateToken(locker);
  pulseActuator(locker);
  printTicket();

  char msg[16];
  snprintf(msg, sizeof(msg), "LOCKER_OK=%d", locker);
  sendToNextion(msg);

  char stateMsg[20];
  snprintf(stateMsg, sizeof(stateMsg), "LOCKER_STATE=%d", stateMask());
  sendToNextion(stateMsg);

  return true;
}

void freeLocker(uint8_t locker){
  if (locker < 1 || locker > LOCKER_COUNT) return;

  uint8_t idx = locker - 1;
  if (lockerState[idx] == FREE){
    Serial.printf("Locker %d ya estaba libre\n", locker);
    return;
  }

  pulseActuator(locker);
  lockerState[idx] = FREE;
  char key[8];
  lockerKey(key, idx);
  prefs.putBool(key, false);

  Serial.printf("Locker %d liberado\n", locker);

  char nextionLocker[16];
  snprintf(nextionLocker, sizeof(nextionLocker), "lk%d=0", locker);
  sendToNextion(nextionLocker);

  char nextionMemory[20];
  snprintf(nextionMemory, sizeof(nextionMemory), "wepo lk%d,%d", locker, (locker - 1) * 4);
  sendToNextion(nextionMemory);

  char stateMsg[20];
  snprintf(stateMsg, sizeof(stateMsg), "LOCKER_STATE=%d", stateMask());
  sendToNextion(stateMsg);
}

void freeAllLockers(){
  bool changed = false;

  for (uint8_t i = 0; i < LOCKER_COUNT; i++){
    if (lockerState[i] == OCCUPIED){
      lockerState[i] = FREE;
      char key[8];
      lockerKey(key, i);
      prefs.putBool(key, false);
      changed = true;
    }
  }

  sendToNextion("lk1=0");
  sendToNextion("wepo lk1,0");
  sendToNextion("lk2=0");
  sendToNextion("wepo lk2,4");
  sendToNextion("lk3=0");
  sendToNextion("wepo lk3,8");
  sendToNextion("lk4=0");
  sendToNextion("wepo lk4,12");
  sendToNextion("lk5=0");
  sendToNextion("wepo lk5,16");

  sendToNextion("va0.val=0");
  sendToNextion("va2.val=0");
  sendToNextion("tm0.en=0");
  sendToNextion("page page1");

  Serial.println(changed ?
                 "Todos los lockers fueron liberados" :
                 "Nextion sincronizada: lockers libres");
  sendToNextion("LOCKER_STATE=0");
}

/* ------------------------------------------------------------------ */
/* Escaneo -> liberar locker                                          */
/* ------------------------------------------------------------------ */

static bool parseLockerPayload(const char *payload, int *num){
  const char *p = NULL;

  if (strncmp(payload, "LOCKER:", 7) == 0){
    p = payload + 7;
  } else if (strncmp(payload, "L=", 2) == 0){
    p = payload + 2;
  } else {
    return false;
  }

  if (*p < '0' || *p > '9') return false;

  int n = 0;
  while (*p >= '0' && *p <= '9'){
    n = n * 10 + (*p - '0');
    p++;
  }
  if (*p != '\0' && *p != '&') return false;
  if (n < 1 || n > LOCKER_COUNT) return false;

  *num = n;
  return true;
}

void handleScanPayload(const char *payload){
  uint32_t now = millis();
  if (strcmp(payload, lastScanPayload) == 0 && (now - lastScanMs) < SCAN_DEDUP_MS){
    return;
  }
  strncpy(lastScanPayload, payload, sizeof(lastScanPayload) - 1);
  lastScanPayload[sizeof(lastScanPayload) - 1] = '\0';
  lastScanMs = now;

  int locker = 0;
  if (!parseLockerPayload(payload, &locker)){
    Serial.printf("[SCAN] %s (formato no valido)\n", payload);
    return;
  }

  uint8_t idx = locker - 1;
  Serial.printf("[SCAN] %s -> locker %d\n", payload, locker);

  if (lockerState[idx] == FREE){
    Serial.printf("Locker %d ya estaba libre\n", locker);
    return;
  }

  beep();
  freeLocker((uint8_t)locker);
}

void drainScanQueue(){
  char item[64];
  while (xQueueReceive(s_scanQueue, item, 0) == pdTRUE){
    handleScanPayload(item);
  }
}

static void feedScanByte(uint8_t c){
  if (c == 0x09 || c == '\r' || c == '\n'){
    if (s_scanLen > 0){
      s_scanLine[s_scanLen] = '\0';
      char item[64];
      strncpy(item, s_scanLine, sizeof(item) - 1);
      item[sizeof(item) - 1] = '\0';
      xQueueSend(s_scanQueue, item, 0);
      s_scanLen = 0;
    }
    return;
  }
  if (c >= 0x20 && c < 0x7F){
    if (s_scanLen < sizeof(s_scanLine) - 1){
      s_scanLine[s_scanLen++] = (char)c;
    } else {
      s_scanLen = 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Nextion / comandos serie                                           */
/* ------------------------------------------------------------------ */

void handleNextionCommand(const char *cmd){
  Serial.printf("[NEXTION] %s\n", cmd);

  if (strcmp(cmd, "BEEP") == 0){
    beep();
    return;
  }

  if (strcmp(cmd, "ready") == 0){
    char stateMsg[20];
    snprintf(stateMsg, sizeof(stateMsg), "LOCKER_STATE=%d", stateMask());
    sendToNextion(stateMsg);
    return;
  }

  if (strncmp(cmd, "LANG:", 5) == 0 || strncmp(cmd, "LANG=", 5) == 0){
    const char *language = cmd + 5;
    if (strcmp(language, "EN") == 0){
      setPrintLanguage(LANGUAGE_EN);
      sendToNextion("LANG_OK=EN");
    } else if (strcmp(language, "ES") == 0){
      setPrintLanguage(LANGUAGE_ES);
      sendToNextion("LANG_OK=ES");
    } else {
      Serial.printf("Idioma no reconocido: %s\n", language);
    }
    return;
  }

  if (strncmp(cmd, "LOCKER:", 7) == 0){
    openLocker((uint8_t)atoi(cmd + 7), "NEXTION");
    return;
  }

  if (strncmp(cmd, "FREE=", 5) == 0){
    freeLocker((uint8_t)atoi(cmd + 5));
    return;
  }
}

void readNextion(){
  while (nextionSerial.available()){
    char c = nextionSerial.read();

    if (c == 0xFF){
      if (nextionLen > 0){
        while (nextionLen > 0 && (nextionLine[nextionLen - 1] == '\r' || nextionLine[nextionLen - 1] == '\n')){
          nextionLen--;
        }
        nextionLine[nextionLen] = '\0';
        if (nextionLen > 0){
          handleNextionCommand(nextionLine);
        }
      }
      nextionLen = 0;
    }
    else if (c == '\r' || c == '\n'){
      if (nextionLen > 0){
        nextionLine[nextionLen] = '\0';
        handleNextionCommand(nextionLine);
        nextionLen = 0;
      }
    }
    else if (nextionLen < sizeof(nextionLine) - 1){
      nextionLine[nextionLen++] = c;
    }
  }
}

void readSerialCommands(){
  static char cmd[16];
  static uint8_t len = 0;

  while (Serial.available()){
    char c = Serial.read();

    if (c == '\n' || c == '\r'){
      if (len > 0){
        cmd[len] = '\0';
        if (strncmp(cmd, "free ", 5) == 0){
          freeLocker((uint8_t)atoi(cmd + 5));
        }
        else if (strncmp(cmd, "open ", 5) == 0){
          openLocker((uint8_t)atoi(cmd + 5), "SERIAL");
        }
        else if (strncmp(cmd, "state", 5) == 0){
          Serial.printf("estado: %d\n", stateMask());
        }
        else if (strncmp(cmd, "boot ", 5) == 0){
          prefs.clear();
          loadState();
          Serial.println("estado borrado");
        }
        len = 0;
      }
    }
    else if (len < sizeof(cmd) - 1){
      cmd[len++] = c;
    }
  }
}

/* ------------------------------------------------------------------ */
/* USB: descriptores, tareas                                          */
/* ------------------------------------------------------------------ */

static esp_err_t cdcControl(usb_device_handle_t dev, uint8_t reqType, uint8_t req,
                            uint16_t value, uint16_t index, const uint8_t *data, uint16_t dataLen){
  size_t bufSize = 8 + dataLen;
  usb_transfer_t *xfer = NULL;
  esp_err_t err = usb_host_transfer_alloc(bufSize, 0, &xfer);
  if (err != ESP_OK || xfer == NULL){
    return err;
  }

  xfer->data_buffer[0] = reqType;
  xfer->data_buffer[1] = req;
  xfer->data_buffer[2] = value & 0xFF;
  xfer->data_buffer[3] = value >> 8;
  xfer->data_buffer[4] = index & 0xFF;
  xfer->data_buffer[5] = index >> 8;
  xfer->data_buffer[6] = dataLen & 0xFF;
  xfer->data_buffer[7] = dataLen >> 8;
  if (dataLen && data && (reqType & 0x80) == 0){
    memcpy(xfer->data_buffer + 8, data, dataLen);
  }

  xfer->device_handle    = dev;
  xfer->bEndpointAddress = 0;
  xfer->num_bytes        = bufSize;
  xfer->timeout_ms       = 1000;
  xfer->callback         = scannerCb;
  xfer->context          = NULL;

  err = usb_host_transfer_submit_control(s_client, xfer);
  if (err == ESP_OK){
    if (xSemaphoreTake(s_scannerSem, pdMS_TO_TICKS(2000)) == pdTRUE){
      err = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
    } else {
      err = ESP_ERR_TIMEOUT;
    }
  }

  usb_host_transfer_free(xfer);
  return err;
}

static void scannerRun(const CdcJob &job){
  esp_err_t err = usb_host_interface_claim(s_client, job.dev, job.data_iface, job.alt);
  if (err != ESP_OK){
    Serial.printf("[scan] claim data iface fallo: %s\n", esp_err_to_name(err));
    return;
  }

  const uint8_t lineCoding[7] = {0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08}; /* 9600 8N1 */
  err = cdcControl(job.dev, 0x21, 0x20, 0, job.comm_iface, lineCoding, 7);
  Serial.printf("[scan] SET_LINE_CODING: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));

  err = cdcControl(job.dev, 0x21, 0x22, 0x0003, job.comm_iface, NULL, 0);
  Serial.printf("[scan] SET_CONTROL_LINE_STATE(DTR|RTS): %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));

  usb_transfer_t *xfer = NULL;
  if (usb_host_transfer_alloc(job.mps, 0, &xfer) != ESP_OK || xfer == NULL){
    Serial.println("[scan] alloc fallo");
    usb_host_interface_release(s_client, job.dev, job.data_iface);
    return;
  }

  xfer->device_handle    = job.dev;
  xfer->bEndpointAddress = job.in_ep;
  xfer->callback         = scannerCb;
  xfer->context          = NULL;

  Serial.printf("[scan] leyendo ep 0x%02X mps=%u\n", job.in_ep, job.mps);

  while (true){
    xfer->num_bytes = job.mps;
    err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK){
      Serial.printf("[scan] submit fallo: %s\n", esp_err_to_name(err));
      break;
    }
    if (xSemaphoreTake(s_scannerSem, portMAX_DELAY) != pdTRUE){
      continue;
    }
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED){
      for (int i = 0; i < xfer->actual_num_bytes; i++){
        feedScanByte(xfer->data_buffer[i]);
      }
    } else if (xfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
               xfer->status == USB_TRANSFER_STATUS_CANCELED){
      Serial.println("[scan] dispositivo retirado");
      break;
    } else {
      Serial.printf("[scan] status=%d, reintentando\n", (int)xfer->status);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  usb_host_transfer_free(xfer);
  usb_host_interface_release(s_client, job.dev, job.data_iface);
}

static void scannerTask(void *){
  CdcJob job;
  while (true){
    if (xQueueReceive(s_cdcQueue, &job, portMAX_DELAY) == pdTRUE){
      scannerRun(job);
    }
  }
}

static void deviceAdded(uint8_t address){
  usb_device_handle_t dev = NULL;
  if (usb_host_device_open(s_client, address, &dev) != ESP_OK){
    Serial.printf("[usb] no se pudo abrir %u\n", address);
    return;
  }

  const usb_device_desc_t *desc = NULL;
  usb_host_get_device_descriptor(dev, &desc);
  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL){
    Serial.println("[usb] sin configuracion activa");
    usb_host_device_close(s_client, dev);
    return;
  }

  PrintJob pj = {};
  CdcJob   cj = {};
  pj.dev = dev;
  cj.dev = dev;
  cj.comm_iface = 0xFF;

  int offset = 0;
  const usb_intf_desc_t *curItf = NULL;
  const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;

  while ((cur = usb_parse_next_descriptor(cur, cfg->wTotalLength, &offset)) != NULL){
    if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE){
      curItf = (const usb_intf_desc_t *)cur;
      if (curItf->bInterfaceClass == 0x02){
        cj.comm_iface = curItf->bInterfaceNumber;
      }
      if (curItf->bInterfaceClass == 0x0A){
        cj.data_iface = curItf->bInterfaceNumber;
        cj.alt = curItf->bAlternateSetting;
      }
    }
    else if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && curItf != NULL){
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)cur;
      uint8_t type = ep->bmAttributes & 0x03;
      if (type != 0x02) continue;

      if (curItf->bInterfaceClass == 0x07 && !(ep->bEndpointAddress & 0x80) && pj.out_ep == 0){
        pj.iface  = curItf->bInterfaceNumber;
        pj.alt    = curItf->bAlternateSetting;
        pj.out_ep = ep->bEndpointAddress;
        pj.mps    = ep->wMaxPacketSize & 0x7FF;
      }
      if (curItf->bInterfaceClass == 0x0A && (ep->bEndpointAddress & 0x80) && cj.in_ep == 0){
        cj.in_ep = ep->bEndpointAddress;
        cj.mps   = ep->wMaxPacketSize & 0x7FF;
      }
    }
  }

  if (desc){
    Serial.printf("[usb] %04X:%04X class=0x%02X printer=%d cdc=%d\n",
                  desc->idVendor, desc->idProduct, desc->bDeviceClass,
                  pj.out_ep != 0, cj.in_ep != 0);
  }

  if (pj.out_ep != 0){
    esp_err_t err = usb_host_interface_claim(s_client, dev, pj.iface, pj.alt);
    if (err == ESP_OK){
      pj.valid = true;
      s_printer = pj;
      Serial.printf("[usb] impresora lista: iface %u ep 0x%02X\n", pj.iface, pj.out_ep);
    } else {
      Serial.printf("[usb] no se pudo reclamar impresora: %s\n", esp_err_to_name(err));
    }
  }

  if (cj.in_ep != 0 && cj.comm_iface != 0xFF){
    cj.valid = true;
    Serial.printf("[usb] escaner: comm %u data %u ep 0x%02X\n",
                  cj.comm_iface, cj.data_iface, cj.in_ep);
    xQueueSend(s_cdcQueue, &cj, 0);
  }

  for (int i = 0; i < MAX_OPEN_DEVICES; i++){
    if (s_devices[i] == NULL){
      s_devices[i] = dev;
      return;
    }
  }
  usb_host_device_close(s_client, dev);
}

static void deviceGone(usb_device_handle_t dev){
  for (int i = 0; i < MAX_OPEN_DEVICES; i++){
    if (s_devices[i] == dev){
      s_devices[i] = NULL;
      break;
    }
  }
  if (s_printer.dev == dev){
    s_printer.valid = false;
  }
  usb_host_device_close(s_client, dev);
  Serial.println("[usb] dispositivo retirado");
}

static void clientEvent(const usb_host_client_event_msg_t *msg, void *){
  switch (msg->event){
    case USB_HOST_CLIENT_EVENT_NEW_DEV:  deviceAdded(msg->new_dev.address); break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE: deviceGone(msg->dev_gone.dev_hdl); break;
    default: break;
  }
}

static void daemonTask(void *){
  while (true){
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS){
      usb_host_device_free_all();
    }
  }
}

static void clientTask(void *){
  while (true){
    usb_host_client_handle_events(s_client, portMAX_DELAY);
  }
}

/* ------------------------------------------------------------------ */
/* Setup / loop                                                       */
/* ------------------------------------------------------------------ */

void setup(){
  Serial.begin(115200);
  delay(1000);

  for (uint8_t i = 0; i < LOCKER_COUNT; i++){
    pinMode(actuatorPins[i], OUTPUT);
    digitalWrite(actuatorPins[i], LOW);
  }

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(RELEASE_ALL_BUTTON, INPUT_PULLUP);

  nextionSerial.begin(NEXTION_BAUD, SERIAL_8N1, NEXTION_RX, NEXTION_TX);

  prefs.begin("locker", false);
  loadState();

  Serial.println();
  Serial.println("===========================");
  Serial.println("        ESP LOCKER         ");
  Serial.println("   USB Scanner + Printer   ");
  Serial.println("===========================");
  Serial.printf("Estado de lockers: %d\n", stateMask());

  s_cdcQueue   = xQueueCreate(4, sizeof(CdcJob));
  s_scanQueue  = xQueueCreate(8, 64);
  s_printerSem = xSemaphoreCreateBinary();
  s_scannerSem = xSemaphoreCreateBinary();

  USBHostBoardInit();

  usb_host_config_t host_config = {};
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
  host_config.enum_filter_cb = enumFilter;
#endif

  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK){
    Serial.printf("[usb] install fallo: %s\n", esp_err_to_name(err));
    return;
  }

  usb_host_client_config_t client_config = {};
  client_config.is_synchronous = false;
  client_config.max_num_event_msg = 5;
  client_config.async.client_event_callback = clientEvent;
  client_config.async.callback_arg = NULL;

  err = usb_host_client_register(&client_config, &s_client);
  if (err != ESP_OK){
    Serial.printf("[usb] client_register fallo: %s\n", esp_err_to_name(err));
    return;
  }

  xTaskCreate(daemonTask,  "usbh_daemon", 4096, NULL, 2, NULL);
  xTaskCreate(clientTask,  "usbh_client", 4096, NULL, 3, NULL);
  xTaskCreate(scannerTask, "scanner",     4096, NULL, 4, NULL);

  Serial.println("USB host listo. Escanea un QR para liberar el locker.");
}

void loop(){
  readSerialCommands();
  readNextion();
  drainScanQueue();

  static bool releaseButtonWasPressed = false;
  bool releaseButtonPressed = digitalRead(RELEASE_ALL_BUTTON) == LOW;

  if (releaseButtonPressed && !releaseButtonWasPressed){
    delay(30);
    if (digitalRead(RELEASE_ALL_BUTTON) == LOW){
      freeAllLockers();
    }
  }

  releaseButtonWasPressed = releaseButtonPressed;

  delay(1);
}
