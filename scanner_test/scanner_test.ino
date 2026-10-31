/*
 * Prueba 1: escaner X7-1000A en modo USB-COM (CDC) conectado directo
 * al puerto USB-OTG nativo del ESP32-S3, sin hub.
 *
 * Requiere placa ESP32-S3 y "USB Mode = USB-OTG (TinyUSB)".
 * Con host USB activo el puerto nativo queda ocupado por el escaner,
 * asi que el monitor serie debe ir por el puerto UART (el otro USB-C).
 */

#include <USBHost.h>
#include <USBHostSerial.h>

#define SCANNER_BAUD 9600

USBHostSerialClass Scanner;

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== TEST ESCANER USB-COM (CDC) ===");

  if (!USBHost.begin()) {
    Serial.println("USBHost.begin() FALLO: no se pudo iniciar el host");
    return;
  }

  Scanner.begin(SCANNER_BAUD);
  Serial.println("Host USB iniciado. Conecta el escaner al puerto USB-OTG.");
}

void loop() {
  USBHost.task();

  static bool wasMounted = false;
  static unsigned long lastBeat = 0;
  bool mounted = Scanner.mounted();

  if (mounted != wasMounted) {
    wasMounted = mounted;
    if (mounted) {
      Serial.printf("ESCANER DETECTADO (baud=%lu) — escanea un QR\n",
                    (unsigned long)Scanner.baudRate());
    } else {
      Serial.println("ESCANER DESCONECTADO");
    }
  }

  if (!mounted) {
    if (millis() - lastBeat > 5000) {
      lastBeat = millis();
      Serial.printf("[esperando] host=%s\n", USBHost.started() ? "ok" : "FALLO");
    }
    return;
  }

  int c;
  while ((c = Scanner.read()) >= 0) {
    if (c >= 32 && c < 127) {
      Serial.write((char)c);
      if (c == '\n' || c == '\r') {
        Serial.println();
      }
    } else {
      Serial.printf("<%02X>", c);
    }
  }
}
