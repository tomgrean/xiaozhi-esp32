# Auto Car Board

简介
--
此目录包含针对“Auto Car”硬件板的配置与说明。该板基于 `bread_compact_wifi`（ESP32 系列）开发，并添加了用于控制小型车辆（miniauto）的 UART 控制接口与常用外设引脚定义。

特性
--
- 基于 ESP32 系列芯片（请参考项目根目录的 `sdkconfig` 与组件清单以获取确切型号）。
- UART 控制通道，用于控制电机驱动、舵机、LED 指示及外部传感器。

引脚（参考，实际以电路板 silkscreen/原理图为准）
--
- UART_TX: GPIO13
- UART_RX: GPIO14

接线说明
--
1. 将miniauto的蓝牙控制模块取出。
2. 将GND、5V和UART的TX和RX接到miniauto的蓝牙模块插座上（注意RX TX交叉连接）。

软件与构建
--
先决条件
- 已安装 ESP-IDF 开发环境并正确配置（参见项目根目录 README）。

构建与烧录
--
在项目根目录下使用 standard ESP-IDF 构建流程：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

（将 `/dev/ttyUSB0` 替换为你电脑上的串口设备）

配置
--
如需调整 UART 端口或其他选项，请编辑 `main/boards/auto-car` 中对应的 config.h、Kconfig/CMakeLists 或在 `menuconfig` 中调整相应选项。

运行与测试
--
1. 烧录完成后，使用串口终端（如 `minicom` 或 `picocom`）连接主控串口以观察启动日志：

```bash
picocom -b 9600 /dev/ttyUSB0
```

2. 将控制命令通过 UART 发送到设备（协议参见 `main/protocols` 或项目文档）。

排错
--
- 无输出：检查电源、串口连接和波特率。
- 控制无响应：确认 UART RX/TX 是否交叉，检查固件中 UART 端口配置是否正确。

贡献与维护
--
欢迎提交 issue 或 pull request。请在 PR 中说明硬件型号、测试步骤与复现指南。


