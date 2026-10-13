# C3 / S3 固件：信息输入与程序执行分离

两个Arduino工程分别位于 `UWB_C3_WiFi` 与 `UWB_S3_WiFi`。
每个工程都需要保留以下三个文件在同一个文件夹：

| 文件 | 用途 |
|---|---|
| `UserConfig.h` | 信息输入：SSID、密码、电脑IP、本机角色和ID、UWB串口引脚及状态灯设置 |
| 与文件夹同名的 `.ino` | 程序执行：Wi-Fi重连、原始串口UDP回传、本地可读距离、AT指令 |
| `RangeFrameParser.h` | TR3.9数据帧与CRC解析 |

日常只修改 `UserConfig.h`，然后重新编译上传。不要将两个工程的 `.ino` 放入同一文件夹。
下载包内使用示例占位信息；如果本地工程原有SSID和密码，更新本地工程时会迁移到配置文件。

## 必填配置

打开对应 `.ino`，再在Arduino IDE标签栏打开 `UserConfig.h`。
填写 `WIFI_SSID`、`WIFI_PASSWORD`、`PC_IP`、`LOCAL_ROLE`、`LOCAL_ID_HEX`。
本机ID必须与UWB实际设置一致，程序不会代替AT设置模块身份。
两台S3各自填写不同的本机ID；两个同角色设备不能使用相同ID。
SSID需要是普通密码认证的2.4GHz网络，与电脑处于可互通的同一局域网。

UWB通信为921600/8N1，电脑串口监视器为115200。
UDP协议和原Python/HTML上位机一致，继续使用端口7788，无需更换上位机。
绿色仅表示Wi-Fi联网，是否收到UDP请以网页设备状态为准。

## 接线

| 连接 | C3默认 | S3默认 |
|---|---|---|
| UWB TX接ESP32接收 | GPIO1 | GPIO17 |
| UWB RX接ESP32发送 | GPIO0 | GPIO18 |
| GND | 两板共地 | 两板共地 |
| 状态灯 | GPIO2红、GPIO3黄、GPIO4绿 | 板载单线可寻址RGB灯 |

S3串口GPIO17/18在代码中通过UART矩阵指定；不要按板卡原生UART标注反向接线。
如现有接线不同，改 `UserConfig.h` 中的 `UWB_RX` 与 `UWB_TX`。S3默认避开GPIO0启动引脚和GPIO19/20原生USB引脚。

## 一号 S3-N16R8 与二号 S3-N8R2

N16R8 / N8R2描述存储配置，不能单独确认开发板厂家或板载RGB的引脚。
下表按常见ESP32-S3-WROOM-1对应型号配置；若实际模块型号不同，以模块数据手册为准。

| Arduino IDE选项 | 一号N16R8 | 二号N8R2 |
|---|---|---|
| Board | 对应具体板型，通用板可选ESP32S3 Dev Module | 同左 |
| Flash Size | 16MB | 8MB |
| Flash Mode | QIO 80MHz（常见WROOM-1） | QIO 80MHz（常见WROOM-1） |
| PSRAM | OPI PSRAM（8MB） | QSPI PSRAM（2MB） |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) | 8M with spiffs (3MB APP/1.5MB SPIFFS) |
| USB CDC On Boot（原生USB） | Enabled | Enabled |
| USB Mode（原生USB） | Hardware CDC and JTAG | Hardware CDC and JTAG |

若使用板上的USB转UART接口输出串口信息，可关闭USB CDC On Boot并使用该接口对应端口。
选项名称基于ESP32 Arduino核心3.3.12，建议使用同版本或兼容新版本。
程序不依赖PSRAM存放应用数据；PSRAM设置须与真实硬件匹配。

## S3板载RGB灯

程序使用Arduino官方 `rgbLedWrite()`，默认选择当前板型的 `RGB_BUILTIN`。

- 黄色：连接中。
- 绿色：Wi-Fi已连接。
- 红色：连接超过15秒，等待5秒再重试；重试阶段恢复黄色。

`RGB_LED_PIN_OVERRIDE = -1` 表示使用板型定义。
确定具体板载灯接GPIO38或48后，可以将该参数改为对应真实GPIO。
例如官方DevKitC-1初版使用GPIO48，v1.1使用GPIO38；N16R8/N8R2并不决定RGB引脚。
板型宏有时是虚拟引脚值，程序直接交给官方API处理，不手动减引脚数。
若板卡有RGB电源使能引脚，按说明填写 `RGB_POWER_PIN`；默认-1代表无需额外使能。
`RGB_LED_BRIGHTNESS` 默认24，可调0~255。
本实现用于WS2812等单线可寻址RGB灯，不适用于三根独立PWM线的RGB电路。
不需要C3的外接红黄绿灯，不需要另装Adafruit_NeoPixel库。

## 验证

编译目标：ESP32 Arduino核心3.3.12；C3、S3 16MB/OPI PSRAM、S3 8MB/QSPI PSRAM。
真实板载RGB引脚、灯色和联网功能仍需上板验证；本次不会自动烧录任何设备。

参考：
- RGB官方示例：https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/GPIO/BlinkRGB/BlinkRGB.ino
- DevKitC-1 v1.1：https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html
- PSRAM封装接口：https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html
