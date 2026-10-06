#include "BluetoothSerial.h"
#include "Preferences.h"

#define SCAN 0
#define LOCKER_COUNT 5
#define NEXTION_TX 17
#define NEXTION_RX 16
#define NEXTION_BAUD 115200

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
  Serial1.print(msg);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  Serial1.flush();
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

void handleNextionCommand(const char *cmd){
  Serial.printf("[NEXTION] %s\n", cmd);

  if (strcmp(cmd, "ready") == 0){
    char stateMsg[20];
    snprintf(stateMsg, sizeof(stateMsg), "LOCKER_STATE=%d", stateMask());
    sendToNextion(stateMsg);
    return;
  }

  if (strncmp(cmd, "LOCKER=", 7) == 0){
    openLocker((uint8_t)atoi(cmd + 7), "NEXTION");
    return;
  }

  if (strncmp(cmd, "FREE=", 5) == 0){
    freeLocker((uint8_t)atoi(cmd + 5));
    return;
  }
}

void readNextion(){
  while (Serial1.available()){
    char c = Serial1.read();

    if (c == 0xFF){
      if (nextionLen > 0){
        nextionLine[nextionLen] = '\0';
        handleNextionCommand(nextionLine);
      }
      nextionLen = 0;
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

  Serial1.begin(NEXTION_BAUD, SERIAL_8N1, NEXTION_RX, NEXTION_TX);

  prefs.begin("locker", false);
  loadState();

  for (int i = 0; i < LOCKER_COUNT; i++){
    pinMode(lockerButtons[i].pin, INPUT_PULLUP);
    lastReading[i] = HIGH;
    lastDebounce[i] = millis();
    buttonLatched[i] = false;
  }

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
  Serial.println("Presiona el boton para imprimir");
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

  if (!SerialBT.connected()){
    Serial.println("Bluetooth Desconectado, reintentando...");
    SerialBT.connect(printerMAC, 0, ESP_SPP_SEC_NONE);
    delay(3000);
    return;
  }

  for (int i = 0; i < LOCKER_COUNT; i++){
    int reading = digitalRead(lockerButtons[i].pin);

    if (reading != lastReading[i]){
      lastDebounce[i] = millis();
      lastReading[i] = reading;
    }

    if ((millis() - lastDebounce[i]) > 50){
      if (reading == LOW && !buttonLatched[i]){
        buttonLatched[i] = true;
        openLocker(lockerButtons[i].locker, "BOTON");
        Serial.println("Ticket Enviado...");
      }
      else if (reading == HIGH){
        buttonLatched[i] = false;
      }
    }
  }

  delay(50);
}