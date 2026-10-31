#include <SPI.h>
#include <UsbShield.h>

#define PIN_MISO 25
#define PIN_MOSI 26
#define PIN_SS   27

USB Usb;
uint8_t maxReadReg(uint8_t reg) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_SS, LOW);
  SPI.transfer(reg);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(PIN_SS, HIGH);
  SPI.endTransaction();
  return v;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Prueba MAX3421E");

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);
  delay(10);
  for (int i = 0; i < 3; i++) {
    uint8_t rev = maxReadReg(0x90);
    Serial.print("rREVISION = 0x");
    Serial.println(rev, HEX);
    delay(200);
  }

  if (Usb.Init() == -1) {
    Serial.println("Fallo la inicializacion del MAX3421E (revisa cableado y alimentacion)");
    return;
  }
  Serial.println("MAX3421E inicializado");
  pinMode(17, INPUT_PULLDOWN);
}

void loop() {
  Usb.Task();
}
