# 验证设备清单

公开清单不记录 IP、DID、MAC、token 或密钥。

| 设备 | 型号 | 连接 | 项目状态 |
|---|---|---|---|
| 米家智能充电台灯 | `MJTD04YL` / `yeelink.light.lamp21` | Xiaomi BLE Mesh | 本地开关、亮度、色温已验证 |
| ESP32 节点 | DFRobot Beetle ESP32-C3 v1.0.0 | Wi-Fi + BLE | 纯网关与 OLED 两套配置均可编译 |
| 128x64 OLED（可选） | SSD1306 I2C | GPIO0 / GPIO1 | 室内外温湿度面板已验证 |

其他 ESP32-C3 开发板理论上可用，但可能需要修改 board、USB 日志和引脚配置。其他米家 BLE Mesh 灯具也可能使用相似协议，但本项目不宣称兼容。
