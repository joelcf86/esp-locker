#include "BluetoothSerial.h"
#include "Preferences.h"
#include "HardwareSerial.h"

#define SCAN 0
#define LOCKER_COUNT 5
#define NEXTION_BAUD 9600
#define NEXTION_RX 21
#define NEXTION_TX 22
#define BUZZER_PIN 32
#define RELEASE_ALL_BUTTON 33

HardwareSerial nextionSerial(2);

#define HOST_SHIELD_SCK 14
#define HOST_SHIELD_MISO 25
#define HOST_SHIELD_MOSI 26
#define HOST_SHIELD_CS 27

enum LockerState { FREE = 0, OCCUPIED = 1 };

struct LockerSession {
  uint8_t locker;
  uint32_t operationId;
  uint64_t token;
  bool active;
};

LockerSession currentSession;

struct LockerButton {
  uint8_t pin;
  uint8_t locker;
};

const LockerButton lockerButtons[LOCKER_COUNT] = {
  {23, 1},
  {4,  4},
  {13, 2},
  {18, 3},
  {19, 5}
};

Preferences prefs;
uint8_t lockerState[LOCKER_COUNT];
uint32_t operationCounter = 0;

BluetoothSerial SerialBT;

uint8_t printerMAC[] = {
  0x66, 0x32, 0x9F,
  0xBC, 0xFC, 0x73
};

int lastReading[LOCKER_COUNT];
unsigned long lastDebounce[LOCKER_COUNT];
bool buttonLatched[LOCKER_COUNT];

char nextionLine[32];
uint8_t nextionLen = 0;

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
  // Buzzer activo: S al GPIO32, - a GND y + a 3V3.
  digitalWrite(BUZZER_PIN, HIGH);
  delay(180);

  digitalWrite(BUZZER_PIN, LOW);
  delay(60);

  digitalWrite(BUZZER_PIN, HIGH);
  delay(250);

  digitalWrite(BUZZER_PIN, LOW);
}

void printQrCode(const String &data){
  SerialBT.write(0x1D);
  SerialBT.write(0x28);
  SerialBT.write(0x6B);
  SerialBT.write(0x04);
  SerialBT.write(0x00);
  SerialBT.write(0x31);
  SerialBT.write(0x41);
  SerialBT.write(0x32);
  SerialBT.write(0x00);

  SerialBT.write(0x1D);
  SerialBT.write(0x28);
  SerialBT.write(0x6B);
  SerialBT.write(0x03);
  SerialBT.write(0x00);
  SerialBT.write(0x31);
  SerialBT.write(0x43);
  SerialBT.write(0x06);

  SerialBT.write(0x1D);
  SerialBT.write(0x28);
  SerialBT.write(0x6B);
  SerialBT.write(0x03);
  SerialBT.write(0x00);
  SerialBT.write(0x31);
  SerialBT.write(0x45);
  SerialBT.write(0x31);

  uint16_t payloadLen = data.length() + 3;

  SerialBT.write(0x1D);
  SerialBT.write(0x28);
  SerialBT.write(0x6B);
  SerialBT.write(payloadLen & 0xFF);
  SerialBT.write(payloadLen >> 8);
  SerialBT.write(0x31);
  SerialBT.write(0x50);
  SerialBT.write(0x30);
  SerialBT.print(data);
  SerialBT.flush();
  delay(200);

  SerialBT.write(0x1D);
  SerialBT.write(0x28);
  SerialBT.write(0x6B);
  SerialBT.write(0x03);
  SerialBT.write(0x00);
  SerialBT.write(0x31);
  SerialBT.write(0x51);
  SerialBT.write(0x30);
  SerialBT.flush();
  delay(1000);
}

void printTicket(){

  String token = tokenToHex(currentSession.token);

  SerialBT.write(0x1B);
  SerialBT.write(0x40);
  delay(200);
  SerialBT.write(0x1B);
  SerialBT.write(0x61);
  SerialBT.write(0x01);
  SerialBT.println("============================");
  SerialBT.println("ACCESS LOCKERS");
  SerialBT.println("============================");
  SerialBT.print("LOCKER #");
  SerialBT.println(currentSession.locker);
  SerialBT.println();
  SerialBT.println("Escanea este codigo QR");
  SerialBT.println("para identificar tu locker:");
  SerialBT.println();

  printQrCode(buildQrPayload());

  SerialBT.print("TOKEN: ");
  SerialBT.println(token);
  SerialBT.println();
  SerialBT.println("Conserva este ticket");
  SerialBT.println("para recoger tus pertenencias");
  SerialBT.println();
  SerialBT.println("ACCESS");
  SerialBT.println("Tu espacio, mas simple");
  SerialBT.println();
  SerialBT.flush();
  delay(500);

  SerialBT.write(0x1B);
  SerialBT.write(0x64);
  SerialBT.write(0x06);
  SerialBT.flush();
  delay(1500);

  SerialBT.write(0x1D);
  SerialBT.write(0x56);
  SerialBT.write(0x00);
  SerialBT.flush();
  delay(1000);

  Serial.println("Ticket impreso");
}

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
  lockerState[idx] = FREE;
  char key[8];
  lockerKey(key, idx);
  prefs.putBool(key, false);

  Serial.printf("Locker %d liberado\n", locker);

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

  // Sincronizar siempre la Nextion, aunque el ESP32 ya tuviera
  // todos los lockers libres. La pantalla puede conservar estados
  // bloqueados en su propia EEPROM.
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

  // Reiniciar la selección y recargar la página para que su evento
  // Initialize pinte todos los lockers como libres.
  sendToNextion("va0.val=0");
  sendToNextion("va2.val=0");
  sendToNextion("tm0.en=0");
  sendToNextion("page page1");

  Serial.println(changed ?
                 "Todos los lockers fueron liberados" :
                 "Nextion sincronizada: lockers libres");
  sendToNextion("LOCKER_STATE=0");
}

void handleNextionCommand(const char *cmd){
  Serial.printf("[NEXTION] %s\n", cmd);

  // Desde cualquier boton Nextion: prints "BEEP",4 + terminador.
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

  if (strncmp(cmd, "LOCKER:", 7) == 0){
    uint8_t lockerNum = 0;
    const char *p = cmd + 7;
    if (*p >= '1' && *p <= '9'){
      lockerNum = (uint8_t)atoi(p);
    }else if ((uint8_t)(*p) >= 1 && (uint8_t)(*p) <= LOCKER_COUNT){
      lockerNum = (uint8_t)(*p);
    }else{
      lockerNum = (uint8_t)atoi(p);
    }
    openLocker(lockerNum, "NEXTION");
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
        if (nextionLen > 0){
          handleNextionCommand(nextionLine);
        }
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

void setup(){
  Serial.begin(115200);
  delay(1000);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(RELEASE_ALL_BUTTON, INPUT_PULLUP);

  nextionSerial.begin(NEXTION_BAUD, SERIAL_8N1, NEXTION_RX, NEXTION_TX);

  prefs.begin("locker", false);
  loadState();

  Serial.println();
  Serial.println("===========================");
  Serial.println("        ESP LOCKER         ");
  Serial.println("     Bluetooth Printer     ");
  Serial.println("===========================");
  Serial.printf("Estado de lockers: %d\n", stateMask());

  if (!SerialBT.begin("ESP_LOCKER", true)){
    Serial.println("Error: no se pudo iniciar Bluetooth");
    while (1) delay(1000);
  }

  if (SCAN){
    Serial.println("Escaneando 10 segundos...");
    BTScanResults *r = SerialBT.discover(10000);
    for (int i = 0; i < r->getCount(); i++){
      BTAdvertisedDevice *d = r->getDevice(i);
      if (d == nullptr) continue;
      Serial.printf("MAC: %s | NOMBRE: %s\n",
                    d->getAddress().toString().c_str(),
                    d->getName().c_str());
    }
    Serial.println("Escaneo terminado");
    while (1) delay(1000);
  }

  Serial.println("Bluetooth Iniciado");
  Serial.println("Conectado por MAC address");
  delay(2000);

  if (SerialBT.connect(printerMAC, 0, ESP_SPP_SEC_NONE)){
    Serial.println("************************");
    Serial.println("*IMPRESORA CONECTADA!!!*");
    Serial.println("************************");
  }
  else{
    Serial.println("ERROR: No se pudo conectar");
  }

}

void loop(){
  readSerialCommands();
  readNextion();

  static bool releaseButtonWasPressed = false;
  bool releaseButtonPressed = digitalRead(RELEASE_ALL_BUTTON) == LOW;

  if (releaseButtonPressed && !releaseButtonWasPressed){
    delay(30);
    if (digitalRead(RELEASE_ALL_BUTTON) == LOW){
      freeAllLockers();
    }
  }

  releaseButtonWasPressed = releaseButtonPressed;

  /*
  static unsigned long lastBtRetry = 0;

  if (!SerialBT.connected()){
    if (millis() - lastBtRetry > 8000){
      lastBtRetry = millis();
      Serial.println("Bluetooth Desconectado, reintentando...");
      SerialBT.connect(printerMAC, 0, ESP_SPP_SEC_NONE);
    }
  }
  */
}
