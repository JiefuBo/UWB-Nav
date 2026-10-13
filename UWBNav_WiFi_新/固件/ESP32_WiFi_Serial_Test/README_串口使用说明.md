# 独立ESP32 Wi-Fi串口测试

适用于ESP32-C3和ESP32-S3，使用Arduino内置WiFi库，不依赖UWB或LED电路。
这是独立固件：烧录它会替换开发板上的UWB程序。测试完成后可重新上传UWB固件。

## 上传

打开同文件夹内 `ESP32_WiFi_Serial_Test.ino`，选择实际开发板和USB端口。
原生USB接口启用 `USB CDC On Boot = Enabled`；S3原生USB选 `Hardware CDC and JTAG`。
若使用USB转UART接口，串口输出按对应板卡设置，通常关闭USB CDC On Boot。
N16R8常见WROOM-1配置：Flash16MB、OPI PSRAM；N8R2：Flash8MB、QSPI PSRAM。
打开串口监视器，波特率115200，行结束选择“换行”或“回车和换行”。

## 使用例子

发送：

```text
scan
```

程序返回序号、Wi-Fi名称、RSSI、信道、认证方式、BSSID。
列表是热点列表，同一个SSID可能对应多个热点；隐藏SSID无法从扫描确定真实名称。
C3/S3仅扫描2.4GHz网络。扫描过程中仍可发送status或cancel。

加入网络，例如名称为 `My Home WiFi`：

```text
join My Home WiFi
```

若扫描结果表明需要密码，程序提示后**在下一行直接输入实际密码**，不要加引号或 `password` 前缀。
例如先发送join，收到提示后再发送密码。密码可以包含空格，程序不回显密码。
串口监视器自身输入框仍可看见你键入的字符。

扫描到的开放网络会直接连接。
若SSID未出现在扫描列表，程序会询问密码，支持隐藏SSID的手动连接；开放网络可发送空行。
可选写法 `join "My Home WiFi"`；首尾有空格的SSID用外层双引号明确界定。
超过25秒未成功会报超时及最近的Wi-Fi断开原因码，不把超时简单归因于密码错误。

成功后输出IP、网关、子网掩码、RSSI和信道。
这只验证加入局域网，不代表互联网或UWB上位机已经连通。

## 其他命令

| 命令 | 用途 |
|---|---|
| `status` | 查询当前联网状态/IP |
| `leave` | 断开Wi-Fi |
| `cancel` | 取消当前扫描、连接或密码输入 |
| `help` | 显示操作说明 |

处于密码输入状态时，输入按密码处理，只有完整一行 `cancel` 是取消指令。
因此密码本身若恰好是 `cancel` 无法使用本交互流程；其他含空格、引号或命令字的密码按原样处理。
不回显未知命令内容，避免误将密码作为普通命令时输出它。
每行最多159字节，过长整行丢弃。CRLF作为一个行结束处理，不会误变成一个空密码。

## 范围与验证

只支持普通开放网络与Personal密码网络；不实现校园WPA2-Enterprise账号/EAP认证，也不实现WEP流程。
不将测试SSID/密码写入源码或Wi-Fi Flash配置，重启后需重新输入。
Arduino ESP32核心3.3.12：C3、S3 16MB/OPI、S3 8MB/QSPI编译检查。
串口CRLF/LF分行与超长输入行为已用原读串口逻辑进行桌面测试。
真实RF扫描与连接仍需用户设备现场验证。不会自动扫描用户周围网络或烧录设备。

API参考：https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
