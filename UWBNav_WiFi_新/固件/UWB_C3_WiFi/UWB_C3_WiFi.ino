// 执行程序：Wi-Fi、UDP回传、UWB串口读取与解析。
// 所有需要填写的信息在同目录 UserConfig.h 中；请先打开该文件。
#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include "RangeFrameParser.h"

#include "UserConfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "此工程用于ESP32-C3，请选择对应C3板型；S3请打开UWB_S3_WiFi工程。"
#endif

HardwareSerial UWB(1);
RangeFrameParser localParser(RECORDS_PER_FRAME);
WiFiUDP udp;
enum LinkState { CONNECTING, CONNECTED, TIMED_OUT };
LinkState state = CONNECTING;
uint32_t stateSince = 0, sequence = 0, sessionId = 0;
uint32_t lastHeartbeat = 0, lastByte = 0, lastStatus = 0;
uint32_t droppedBytes = 0, sendErrors = 0;
uint8_t raw[128];
size_t rawLength = 0;
String deviceId;
char command[128];
size_t commandLength = 0;
bool commandOverflow = false;

void leds(bool red, bool yellow, bool green) {
  digitalWrite(LED_RED, red == LED_ACTIVE_HIGH ? HIGH : LOW);
  digitalWrite(LED_YELLOW, yellow == LED_ACTIVE_HIGH ? HIGH : LOW);
  digitalWrite(LED_GREEN, green == LED_ACTIVE_HIGH ? HIGH : LOW);
}

void startConnect() {
  state = CONNECTING;
  stateSince = millis();
  leds(false, true, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("正在连接 Wi-Fi：%s\n", WIFI_SSID); // 不打印密码
}

void updateWiFi() {
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (state != CONNECTED) {
      state = CONNECTED;
      leds(false, false, true);
      udp.begin(0); // 使用本地临时 UDP 端口
      Serial.printf("Wi-Fi连接成功，本机 IP：%s，上位机：%s:%u\n",
                    WiFi.localIP().toString().c_str(), PC_IP, PC_UDP_PORT);
    }
    return;
  }
  if (state == CONNECTED) {
    udp.stop();
    WiFi.disconnect();
    startConnect();
  } else if (state == CONNECTING && now - stateSince >= CONNECT_TIMEOUT_MS) {
    state = TIMED_OUT;
    stateSince = now;
    WiFi.disconnect();
    leds(true, false, false);
    Serial.println("Wi-Fi连接超时：红灯点亮，5秒后重试。");
  } else if (state == TIMED_OUT && now - stateSince >= RETRY_WAIT_MS) {
    startConnect();
  }
}

// 原始串口字节不改写。十六进制只用于 JSON 传输表示；后端恢复字节解析。
// 添加本机身份、启动会话和序号，避免标签混淆及 UDP 丢包后拼错帧。
void sendRaw(const uint8_t *data, size_t length) {
  if (state != CONNECTED) { droppedBytes += length; return; }
  char hex[257];
  const char *digits = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    hex[2 * i] = digits[data[i] >> 4];
    hex[2 * i + 1] = digits[data[i] & 15];
  }
  hex[2 * length] = 0;
  char packet[640];
  const int n = snprintf(packet, sizeof(packet),
    "{\"v\":1,\"device\":\"%s\",\"session\":%lu,\"seq\":%lu,"
    "\"role\":\"%s\",\"self_id\":\"%s\",\"records\":%u,"
    "\"uptime_ms\":%lu,\"hex\":\"%s\"}", deviceId.c_str(),
    (unsigned long)sessionId, (unsigned long)sequence++, LOCAL_ROLE,
    LOCAL_ID_HEX, RECORDS_PER_FRAME, (unsigned long)millis(), hex);
  if (n <= 0 || n >= (int)sizeof(packet)) { ++sendErrors; return; }
  if (!udp.beginPacket(PC_IP, PC_UDP_PORT)) { ++sendErrors; return; }
  udp.write((const uint8_t *)packet, n);
  if (!udp.endPacket()) ++sendErrors;
}

// 保留电脑串口发送 AT 指令的能力，自动补上回车换行。
// AT 配置前仍需按手册将 UWB 自身 GPIO17 拉低；测距时恢复高/悬空。
void readCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (commandOverflow) Serial.println("指令过长，已丢弃。");
      else if (commandLength) {
        command[commandLength] = 0;
        UWB.write((const uint8_t *)command, commandLength);
        UWB.print("\r\n");
        Serial.printf("发送指令：%s\n", command);
      }
      commandLength = 0;
      commandOverflow = false;
    } else if (!commandOverflow) {
      if (commandLength < sizeof(command) - 1) command[commandLength++] = c;
      else commandOverflow = true;
    }
  }
}

// 本地保留先前程序的可读距离；网络发送的仍是原始字节。
void printLocalRange() {
  const uint8_t *f = localParser.frame;
  for (uint8_t i = 0; i < RECORDS_PER_FRAME; ++i) {
    const size_t k = 5 + 6 * i;
    const uint32_t id = (uint32_t(f[k]) << 24) | (uint32_t(f[k+1]) << 16)
                      | (uint32_t(f[k+2]) << 8) | f[k+3];
    const uint16_t cm = (uint16_t(f[k+4]) << 8) | f[k+5];
    Serial.printf("距离%s%lu号，相距%u cm。\n", f[4] ? "基站" : "标签",
                  (unsigned long)id, (unsigned)cm);
  }
}

// 让AT指令的可读回复继续出现在串口监视器。
void printTextByte(uint8_t b) {
  if ((b >= 32 && b <= 126) || b == '\r' || b == '\n') Serial.write(b);
}

void flushRaw() {
  if (!rawLength) return;
  sendRaw(raw, rawLength);
  if (PRINT_RAW_HEX) {
    Serial.print("串口原始数据：");
    for (size_t i = 0; i < rawLength; ++i) Serial.printf("%02X ", raw[i]);
    Serial.println();
  }
  rawLength = 0;
}

void setup() {
  Serial.begin(CONSOLE_BAUD); // 电脑调试串口，与 UWB 921600 独立
  pinMode(LED_RED, OUTPUT); pinMode(LED_YELLOW, OUTPUT); pinMode(LED_GREEN, OUTPUT);
  leds(false, true, false);
  UWB.setRxBufferSize(4096);
  UWB.begin(UWB_BAUD, SERIAL_8N1, UWB_RX, UWB_TX);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // 关闭 Wi-Fi 省电，可能避开某些接入点与省电机制的兼容问题。
  bool powerOK = WiFi.setTxPower(WIFI_POWER_8_5dBm); // 排查用：降低发射功率，观察是否与供电或射频状态有关
  Serial.printf("设置发射功率：%s\n", powerOK ? "成功" : "失败"); // 排查用：降低发射功率，观察是否与供电或射频状态有关
  WiFi.setAutoReconnect(false); // 使用本程序的非阻塞超时/重试状态机
  deviceId = WiFi.macAddress();
  sessionId = esp_random();
  startConnect();
}

void loop() {
  updateWiFi(); // 不使用等待联网的 while，断网时仍持续读取 UWB
  // 每轮最多处理512字节，防止串口持续输入时饿死 Wi-Fi 状态机。
  for (int i = 0; i < 512 && UWB.available(); ++i) {
    const uint8_t b = (uint8_t)UWB.read();
    raw[rawLength++] = b;
    if (localParser.push(b, printTextByte)) printLocalRange();
    lastByte = millis();
    if (rawLength == sizeof(raw)) flushRaw();
  }
  if (rawLength && millis() - lastByte >= 3) flushRaw();
  readCommands();
  if (millis() - lastHeartbeat >= 1000) {
    lastHeartbeat = millis();
    sendRaw(nullptr, 0); // 空字节心跳，电脑可区分“联网但无测距”
  }
  if (millis() - lastStatus >= 5000) {
    lastStatus = millis();
    Serial.printf("状态：%s；离线丢弃字节：%lu；UDP发送失败：%lu\n",
      state == CONNECTED ? "Wi-Fi已连接" : state == CONNECTING ? "连接中" : "连接超时",
      (unsigned long)droppedBytes, (unsigned long)sendErrors);
  }
}
