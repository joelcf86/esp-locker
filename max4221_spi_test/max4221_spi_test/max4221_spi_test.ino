#include <SPI.h>
#include <driver/gpio.h>

// --- Test de comunicacion SPI con el MAX3421E (v3) ---
// El puente MOSI-MISO ya demostro que el ESP32 y sus pines estan bien.
// Ahora comprobamos si el modulo esta manejando la linea MISO o la deja al aire.
//
//   rREVISION normal        -> lo que responde el chip
//   rREVISION con pull-up   -> 0xFF = MISO al aire (el chip no responde)
//                              0x00 = algo esta forzando MISO a LOW

#define PIN_SCK  14
#define PIN_MISO 25
#define PIN_MOSI 26
#define PIN_SS   27

uint8_t readReg(uint32_t hz, uint8_t reg) {
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_SS, LOW);
  delayMicroseconds(5);
  SPI.transfer(reg);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(PIN_SS, HIGH);
  SPI.endTransaction();
  return v;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("--- TEST SPI MAX3421E v3 ---");

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);
  delay(20);

  uint8_t rev = readReg(1000000, 0x90);
  Serial.printf("rREVISION (normal)  = 0x%02X\n", rev);

  // Pull-up interno en MISO para ver si la linea esta al aire
  gpio_set_pull_mode((gpio_num_t)PIN_MISO, GPIO_PULLUP_ONLY);
  delay(5);
  rev = readReg(1000000, 0x90);
  Serial.printf("rREVISION (pull-up) = 0x%02X\n", rev);
  gpio_set_pull_mode((gpio_num_t)PIN_MISO, GPIO_FLOATING);

  if (rev == 0xFF) {
    Serial.println("=> MISO al aire: el modulo NO responde");
    Serial.println("   Revisa: VCC del modulo, GND comun, RESET a 3V3 y que SS llegue al pin SS");
  } else if (rev == 0x00) {
    Serial.println("=> Algo fuerza MISO a LOW: corto a GND o cable en el pin equivocado");
  } else {
    Serial.println("=> El chip responde");
  }

  // Estado del pin SS y del pin RESET no se pueden leer desde aqui,
  // pero se imprime el nivel que deja el ESP32 en SS para referencia.
  Serial.printf("SS (GPIO%d) en HIGH\n", PIN_SS);
  Serial.println("--- fin ---");
}

void loop() {
  delay(1000);
}
