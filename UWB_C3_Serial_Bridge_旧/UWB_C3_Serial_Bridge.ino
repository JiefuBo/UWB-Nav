#include <Arduino.h>
#include <HardwareSerial.h>

// UWB TX -> ESP32-C3 GPIO1 (RX)
// UWB RX -> ESP32-C3 GPIO0 (TX)
// Connect both boards' GND. Use compatible 3.3 V UART logic.
constexpr int UWB_RX_PIN = 1;
constexpr int UWB_TX_PIN = 0;
// Match the UWB module's current setting. Factory default in V1.2: 921600.
constexpr uint32_t UWB_BAUD = 921600;

HardwareSerial UWB(1);
uint8_t received[64];
size_t receivedLength = 0;
uint32_t lastByteMs = 0;
char command[128];
size_t commandLength = 0;
bool commandOverflow = false;

// These are receive chunks, not necessarily complete UWB protocol frames.
void showReceived() {
  if (receivedLength == 0) return;
  Serial.printf("RX [%u bytes] HEX: ", (unsigned)receivedLength);
  for (size_t i = 0; i < receivedLength; ++i) {
    Serial.printf("%02X ", received[i]);
  }
  Serial.print(" | TEXT: ");
  for (size_t i = 0; i < receivedLength; ++i) {
    const uint8_t c = received[i];
    if (c == '\r') Serial.print("\\r");
    else if (c == '\n') Serial.print("\\n");
    else if (c >= 32 && c <= 126) Serial.write(c);
    else Serial.print('.');
  }
  Serial.println();
  receivedLength = 0;
}

void readConsole() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (commandOverflow) {
        Serial.println("Command too long; discarded.");
      } else if (commandLength > 0) {
        showReceived();
        command[commandLength] = '\0';
        UWB.write((const uint8_t *)command, commandLength);
        UWB.print("\r\n");  // Always append the module's required CRLF.
        Serial.print("TX: ");
        Serial.println(command);
      }
      commandLength = 0;
      commandOverflow = false;
    } else if (!commandOverflow) {
      if (commandLength < sizeof(command) - 1) {
        command[commandLength++] = c;
      } else {
        commandOverflow = true;
      }
    }
  }
}

void setup() {
  Serial.begin(921600);
  const uint32_t startMs = millis();
  while (!Serial && millis() - startMs < 2000) delay(10);

  UWB.setRxBufferSize(2048);  // Set before begin().
  UWB.begin(UWB_BAUD, SERIAL_8N1, UWB_RX_PIN, UWB_TX_PIN);
  Serial.println("ESP32-C3 UWB serial bridge ready.");
  Serial.printf("UWB: RX GPIO%d, TX GPIO%d, %lu baud, 8N1\n",
                UWB_RX_PIN, UWB_TX_PIN, (unsigned long)UWB_BAUD);
  Serial.println("Type an AT command and press Send with a line ending enabled.");
  Serial.println("For AT configuration, pull UWB GPIO17 LOW as specified in its manual.");
  Serial.println("For ranging, restore UWB GPIO17 HIGH or floating.");
}

void loop() {
  // Read raw bytes: ranging data is binary and must not depend on newlines.
  while (UWB.available()) {
    received[receivedLength++] = (uint8_t)UWB.read();
    lastByteMs = millis();
    if (receivedLength == sizeof(received)) showReceived();
  }
  if (receivedLength > 0 && millis() - lastByteMs >= 3) showReceived();
  readConsole();
}
