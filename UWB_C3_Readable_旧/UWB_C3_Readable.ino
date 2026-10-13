#include <Arduino.h>
#include <HardwareSerial.h>
#include "RangeFrameParser.h"

constexpr int UWB_RX_PIN = 1; // UWB TX -> C3 GPIO1
constexpr int UWB_TX_PIN = 0; // UWB RX -> C3 GPIO0
constexpr uint32_t UWB_BAUD = 921600;
// Keep 1 for the supplied 13-byte frames. For a BASE station batching N tags,
// this must match its AT+OTOTAL setting. There is no count field in the frame.
constexpr uint8_t RECORDS_PER_FRAME = 1;
static_assert(RECORDS_PER_FRAME >= 1 && RECORDS_PER_FRAME <= 10, "Count must be 1..10");

HardwareSerial UWB(1);
RangeFrameParser parser(RECORDS_PER_FRAME);
char command[128];
size_t commandLength = 0;
bool overflow = false;
uint32_t totalFrames = 0;

void printRange() {
  const uint8_t *f = parser.frame;
  const char *role = f[4] == 0 ? "标签" : "基站";
  for (uint8_t i = 0; i < RECORDS_PER_FRAME; ++i) {
    const size_t offset = 5 + 6 * i;
    const uint32_t id = (uint32_t(f[offset]) << 24)
                      | (uint32_t(f[offset + 1]) << 16)
                      | (uint32_t(f[offset + 2]) << 8)
                      | uint32_t(f[offset + 3]);
    const uint16_t cm = (uint16_t(f[offset + 4]) << 8) | f[offset + 5];
    Serial.printf("距离%s%lu号，相距%u cm。\n", role,
                  (unsigned long)id, (unsigned)cm);
  }
  ++totalFrames;
}

// Pass readable AT responses through while the parser searches for a header.
void printTextByte(uint8_t b) {
  if ((b >= 32 && b <= 126) || b == '\r' || b == '\n') Serial.write(b);
}

void readCommands() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (overflow) Serial.println("指令过长，已丢弃。");
      else if (commandLength) {
        command[commandLength] = '\0';
        UWB.write((const uint8_t *)command, commandLength);
        UWB.print("\r\n");
        Serial.print("发送指令：");
        Serial.println(command);
      }
      commandLength = 0;
      overflow = false;
    } else if (!overflow) {
      if (commandLength < sizeof(command) - 1) command[commandLength++] = c;
      else overflow = true;
    }
  }
}

void setup() {
  Serial.begin(921600); // 检查这个波特率！！！
  const uint32_t startMs = millis();
  while (!Serial && millis() - startMs < 2000) delay(10);
  UWB.setRxBufferSize(2048);
  UWB.begin(UWB_BAUD, SERIAL_8N1, UWB_RX_PIN, UWB_TX_PIN);
  Serial.println("UWB可读测距程序已启动。UWB波特率：921600。");
  Serial.println("输出编号为对端模块ID，使用十进制显示；ID配置工具使用十六进制。");
  Serial.println("当前运行版本: V1.0-C3");
}

void loop() {
  while (UWB.available()) {
    if (parser.push((uint8_t)UWB.read(), printTextByte)) printRange();
  }
  readCommands();
  // Continuous status avoids missing the one-time startup message.
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= 5000) {
    lastStatus = millis();
    Serial.printf("[运行状态] 已收到%lu个有效测距帧。\n", (unsigned long)totalFrames);
  }
}
