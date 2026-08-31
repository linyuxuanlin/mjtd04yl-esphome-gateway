# 图片与来源

本目录只保存本项目自己制作或截取的图片。厂商和媒体产品图在教程中使用原始 HTTPS 地址引用，不复制进仓库。

| 文件 | 类型 | 说明 |
|---|---|---|
| `home-assistant-esp32-diagnostics-offline.jpg` | 本项目截图 | 2026-08-31 截自本机 Home Assistant。只保留诊断卡片，并裁掉设备尾号、区域、账号等标识。 |
| `home-assistant-esp32-diagnostics-online.jpg` | 本项目截图 | 2026-08-31 恢复连接后截自本机 Home Assistant。显示固件版本、已连接、运行时间与 Wi-Fi 信号，并裁掉设备尾号、区域、账号等标识。 |
| `oled-layout.svg` | 本项目自绘 | 根据 `esphome/beetle-esp32-c3.yaml` 的 128×64 显示代码等比例重绘，不是 OLED 实拍。 |
| `oled-wiring.svg` | 本项目自绘 | 根据当前默认引脚画出的接线示意：GPIO0 → SCL、GPIO1 → SDA、3V3 → VCC、GND → GND。 |

教程中的外部产品图：

- 米家智能充电台灯产品实图来自 [Gizmochina 对小米发布物料的报道](https://www.gizmochina.com/2021/09/29/xiaomi-launches-the-mijia-smart-rechargeable-desk-lamp-mijia-electric-kettle-2/)，仅用于识别产品，图片权利归原权利人。
- Beetle ESP32-C3 产品实图来自 [DFRobot DFR0868 官方 Wiki](https://wiki.dfrobot.com/dfr0868/)，图片权利归 DFRobot。

如果你要转载文档，建议把外部产品图替换成自己拍摄的照片；本目录中的截图和自绘图可随仓库的 MIT License 使用。
