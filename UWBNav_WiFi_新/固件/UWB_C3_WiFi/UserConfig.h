#pragma once
#include <Arduino.h>

// ================================================================
// 信息输入区：只修改本文件；主程序 .ino 是执行区。
// 下面配置在重新编译/上传后生效，不能直接更改已烧录设备的参数。
// ================================================================

// 【1. 必填：Wi-Fi 名称与密码】只支持普通2.4GHz密码Wi-Fi。
// 电脑和ESP32必须处于可互通的同一局域网；不在代码中打印密码。
static constexpr char WIFI_SSID[] = "VisionStudio";      // ★ SSID
static constexpr char WIFI_PASSWORD[] = "VISIONstudio";  // ★ 密码

// 【2. 必填：上位机地址】填写电脑Wi-Fi网卡IPv4，不是ESP32的IP。
static constexpr char PC_IP[] = "10.11.11.166";
static constexpr uint16_t PC_UDP_PORT = 7788;

// 【3. 必填：本机UWB身份】与本机UWB模块的实际角色、ID保持一致。
// 移动标签用 tag；固定基站回传用 anchor。这些值不会自动设置UWB模块。
static constexpr char LOCAL_ROLE[] = "tag";
static constexpr char LOCAL_ID_HEX[] = "2"; // ★ 十六进制，不带0x；同角色设备ID不能重复
// 当前13字节帧填1；基站批量帧需与AT+OTOTAL一致，不是基站数量。
static constexpr uint8_t RECORDS_PER_FRAME = 1;

// 【4. 串口参数】UWB TX -> 本机RX；UWB RX -> 本机TX；两板必须共地。
static constexpr int UWB_RX = 1;
static constexpr int UWB_TX = 0;
static constexpr uint32_t UWB_BAUD = 921600;
static constexpr uint32_t CONSOLE_BAUD = 115200;
// 本地仍打印可读距离。true额外打印HEX；网络始终回传原始字节。
static constexpr bool PRINT_RAW_HEX = false;

// 【5. Wi-Fi 超时设置】黄色连接中；绿色联网；红色超时等待重试。
// 绿色仅代表Wi-Fi联网，不代表上位机已经收到UDP。
static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t RETRY_WAIT_MS = 5000;

// 【6. C3外接状态灯】GPIO -> 限流电阻 -> LED正极，负极接GND。
static constexpr int LED_RED = 2;
static constexpr int LED_YELLOW = 3;
static constexpr int LED_GREEN = 4;
static constexpr bool LED_ACTIVE_HIGH = true; // 低电平点亮时改false
// GPIO2是启动绑带引脚，LED电路不能强制改变复位时所需电平。

static_assert(RECORDS_PER_FRAME >= 1 && RECORDS_PER_FRAME <= 10, "帧记录数量必须为1~10");
static_assert(PC_UDP_PORT > 0, "UDP端口不能为0");
static_assert(UWB_RX != UWB_TX, "UART收发引脚不能相同");
static_assert(CONNECT_TIMEOUT_MS > 0 && RETRY_WAIT_MS > 0, "超时和重试等待必须大于0");
