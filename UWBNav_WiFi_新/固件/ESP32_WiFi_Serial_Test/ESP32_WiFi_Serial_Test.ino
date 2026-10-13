#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ================= 参数输入区 =================
// 独立Wi-Fi测试程序：兼容ESP32-C3/S3，不读取UWB、不依赖外接LED。
// 无需将SSID/密码写入代码，全部通过电脑串口监视器输入。
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t JOIN_TIMEOUT_MS = 25000;
constexpr uint32_t SCAN_TIMEOUT_MS = 30000;
// ================= 程序执行区 =================

bool scanning = false, joining = false, waitingPassword = false;
uint32_t scanStarted = 0, joinStarted = 0, lastProgress = 0, lastGreeting = 0;
String targetSSID;
bool passwordRequired = false, allowShortPassword = true;
char inputLine[160];
size_t inputLength = 0;
bool lineOverflow = false, seenCommand = false, ignoreNextLF = false;
volatile uint16_t lastDisconnectReason = 0;

void printHelp() {
  Serial.println("\n=== ESP32独立Wi-Fi串口测试 ===");
  Serial.println("scan               搜索附近2.4GHz Wi-Fi");
  Serial.println("join <Wi-Fi名称>   加入网络；名称含空格也可，例如 join My Home WiFi");
  Serial.println("                   加密网络会提示输入密码，下一行直接发送密码");
  Serial.println("                   开放网络无需密码；未扫描到的网络会询问密码");
  Serial.println("status             显示连接状态、IP、网关与信号强度");
  Serial.println("leave              断开Wi-Fi");
  Serial.println("cancel             取消扫描、连接或密码输入");
  Serial.println("help               显示本说明");
  Serial.println("串口115200，行结束选择换行或回车和换行。密码不会被程序打印。");
}

const char *authName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "开放/无需密码";
    case WIFI_AUTH_WEP: return "WEP（本测试不支持）";
    case WIFI_AUTH_WPA_PSK: return "WPA-Personal";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-Personal";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-Personal";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "Enterprise（需账号认证）";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-Personal";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-Personal";
    default: return "其他认证方式";
  }
}

void printStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("当前未连接。状态码：%d；最近断开原因码：%u\n",
                  (int)WiFi.status(), (unsigned)lastDisconnectReason);
    return;
  }
  Serial.printf("已连接：%s\n", WiFi.SSID().c_str());
  Serial.printf("本机IP：%s\n", WiFi.localIP().toString().c_str());
  Serial.printf("网关：%s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("子网掩码：%s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("信号：%ld dBm；信道：%ld\n", (long)WiFi.RSSI(), (long)WiFi.channel());
  Serial.println("已加入局域网；这不等于已验证互联网或上位机连通性。");
}

void cancelTask() {
  if (scanning) { esp_wifi_scan_stop(); WiFi.scanDelete(); }
  if (joining) WiFi.disconnect(false, false);
  scanning = joining = waitingPassword = false;
  targetSSID = "";
  Serial.println("已取消当前操作。");
}

void beginJoin(const String &password) {
  WiFi.disconnect(false, false);
  lastDisconnectReason = 0;
  WiFi.setAutoReconnect(false);
  joinStarted = lastProgress = millis();
  joining = true;
  waitingPassword = false;
  Serial.printf("正在连接：%s（最多等待%lu秒）\n", targetSSID.c_str(),
                (unsigned long)(JOIN_TIMEOUT_MS / 1000));
  // 显式开放最低认证门槛，仅此测试器允许连接用户选定的开放/WPA网络。
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  WiFi.begin(targetSSID.c_str(), password.length() ? password.c_str() : nullptr);
}

void requestJoin(String ssid) {
  ssid.trim();
  // 可选外层双引号，用于明确包含首尾空格的SSID。
  if (ssid.length() >= 2 && ssid.startsWith("\"") && ssid.endsWith("\""))
    ssid = ssid.substring(1, ssid.length() - 1);
  if (!ssid.length() || ssid.length() > 32) {
    Serial.println("Wi-Fi名称不能为空，且UTF-8编码长度不能超过32字节。");
    return;
  }
  targetSSID = ssid;
  passwordRequired = false; allowShortPassword = true;
  // 同名热点可能出现多条记录，使用扫描结果中信号最强者判断认证方式。
  const int count = WiFi.scanComplete();
  int best = -1;
  for (int i = 0; i < count; ++i)
    if (WiFi.SSID(i) == ssid && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
  if (best >= 0) {
    const auto auth = WiFi.encryptionType(best);
    if (auth == WIFI_AUTH_WPA2_ENTERPRISE || auth == WIFI_AUTH_WEP) {
      Serial.printf("该网络使用%s，不能只靠普通Wi-Fi密码加入。\n", authName(auth));
      targetSSID = "";
      return;
    }
    if (auth == WIFI_AUTH_OPEN) { beginJoin(""); return; }
    passwordRequired = true;
    allowShortPassword = auth == WIFI_AUTH_WPA3_PSK;
  }
  waitingPassword = true;
  Serial.printf("准备加入：%s\n", ssid.c_str());
  Serial.println("请在下一行直接输入密码并发送，程序不回显密码。");
  if (best < 0) Serial.println("未在扫描结果中找到此名称；如果它是开放网络，发送一个空行。");
  Serial.println("输入 cancel 可取消。密码可以包含空格，不要加引号。");
}

void handleLine(const String &line) {
  seenCommand = true;
  if (waitingPassword) {
    if (line == "cancel") { cancelTask(); return; }
    // 保留密码中的所有空格，不执行trim，也不打印输入内容。
    if (!line.length() && passwordRequired) {
      Serial.println("这是加密网络，密码不能为空，请重新输入。"); return;
    }
    if (line.length() > 64 || (!allowShortPassword && line.length() > 0 && line.length() < 8)) {
      Serial.println("普通WPA/WPA2密码需8~63字节，或64个十六进制字符；请重新输入。");
      return;
    }
    if (line.length() == 64) {
      for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)line[i])) {
          Serial.println("64字节密码必须全部为十六进制字符，请重新输入。"); return;
        }
      }
    }
    beginJoin(line);
    return;
  }
  String command = line;
  command.trim();
  if (!command.length()) return;
  if (command == "help") { printHelp(); return; }
  if (command == "status") { printStatus(); return; }
  if (command == "cancel") { cancelTask(); return; }
  if (command == "leave") {
    cancelTask(); WiFi.disconnect(false, false); Serial.println("已断开Wi-Fi。"); return;
  }
  if (scanning || joining) {
    Serial.println("当前操作尚未完成；可发送status或cancel。"); return;
  }
  if (command == "scan") {
    WiFi.scanDelete();
    const int result = WiFi.scanNetworks(true, true); // 异步扫描，显示隐藏网络记录
    if (result == WIFI_SCAN_FAILED) { Serial.println("扫描启动失败，请重试。"); return; }
    scanning = true; scanStarted = millis();
    Serial.println("正在扫描，请稍候；串口仍可接受status或cancel。"); return;
  }
  if (command.startsWith("join ")) { requestJoin(command.substring(5)); return; }
  Serial.println("未知命令。发送help查看用法。"); // 不回显可能误输入的密码
}

void readConsole() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    // 将CRLF视作一个行结束，避免它的LF被误当成下一行的空密码。
    if (ignoreNextLF && c == '\n') { ignoreNextLF = false; continue; }
    ignoreNextLF = false;
    if (c == '\r' || c == '\n') {
      if (c == '\r') ignoreNextLF = true;
      if (lineOverflow) Serial.println("输入过长，整行已丢弃；请重新输入。");
      else {
        inputLine[inputLength] = 0;
        handleLine(String(inputLine));
      }
      // 清除接收缓冲区中的密码副本，不长期保存用户输入。
      memset(inputLine, 0, sizeof(inputLine));
      inputLength = 0; lineOverflow = false;
    } else if (!lineOverflow) {
      if (inputLength < sizeof(inputLine) - 1) inputLine[inputLength++] = c;
      else lineOverflow = true;
    }
  }
}

void updateTasks() {
  const uint32_t now = millis();
  if (scanning) {
    const int count = WiFi.scanComplete();
    if (count >= 0) {
      scanning = false;
      Serial.printf("\n扫描完成，共%u个热点：\n", (unsigned)count);
      Serial.println("序号 | Wi-Fi名称 | RSSI(dBm) | 信道 | 认证 | BSSID");
      for (int i = 0; i < count; ++i) {
        const String ssid = WiFi.SSID(i);
        Serial.printf("%d | %s | %ld | %ld | %s | %s\n", i+1,
          ssid.length() ? ssid.c_str() : "[隐藏SSID，需手动填写真实名称]",
          (long)WiFi.RSSI(i), (long)WiFi.channel(), authName(WiFi.encryptionType(i)),
          WiFi.BSSIDstr(i).c_str());
      }
      Serial.println("发送 join <Wi-Fi名称> 加入；本列表只包括可扫描到的2.4GHz网络。");
      // 保留扫描结果，便于join判断是否需要密码；下一次scan会释放旧结果。
    } else if (count == WIFI_SCAN_FAILED || now - scanStarted >= SCAN_TIMEOUT_MS) {
      esp_wifi_scan_stop(); WiFi.scanDelete(); scanning = false;
      Serial.println("扫描失败或超时，请再次发送scan。");
    }
  }
  if (joining) {
    if (WiFi.status() == WL_CONNECTED) {
      joining = false; targetSSID = "";
      Serial.println("连接成功！"); printStatus();
    } else if (now - joinStarted >= JOIN_TIMEOUT_MS) {
      joining = false; targetSSID = ""; WiFi.disconnect(false, false);
      Serial.printf("连接超时。最近断开原因码：%u\n", (unsigned)lastDisconnectReason);
      Serial.println("检查名称、密码、信号和2.4GHz网络；可重新发送join。");
    } else if (now - lastProgress >= 2000) {
      lastProgress = now; Serial.println("仍在连接中...");
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  WiFi.persistent(false); // 不将本次测试的Wi-Fi密码写入Flash
  WiFi.mode(WIFI_STA);
  // 添加测试内容
  // 排查用：关闭 Wi-Fi 省电
  WiFi.setSleep(false);

  // 排查用：降低发射功率，观察是否与供电或射频状态有关
  bool powerOK = WiFi.setTxPower(WIFI_POWER_8_5dBm);
  // bool powerOK = WiFi.setTxPower(WIFI_POWER_11dBm);
  Serial.printf("设置发射功率：%s\n", powerOK ? "成功" : "失败");
  // 结束测试内容
  WiFi.setAutoReconnect(false);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  const uint32_t started = millis();
  while (!Serial && millis() - started < 2000) delay(10);
  printHelp(); lastGreeting = millis();
}

void loop() {
  readConsole(); updateTasks();
  // 用户尚未发送命令前，定期给提示，避免监视器打开太晚错过开机文本。
  if (!seenCommand && millis() - lastGreeting >= 5000) {
    lastGreeting = millis(); Serial.println("Wi-Fi测试程序就绪。发送scan搜索，发送help查看说明。");
  }
  delay(1);
}
