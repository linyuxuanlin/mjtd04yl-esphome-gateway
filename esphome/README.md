# ESPHome 固件配置

两份入口 YAML 共用 `packages/mjtd04yl-gateway-base.yaml` 与同一个自定义组件，因此 BLE 登录和控制逻辑不会出现两个分支。

| 配置 | 用途 | 额外硬件 / 依赖 |
|---|---|---|
| `beetle-esp32-c3-gateway.yaml` | 纯网关，推荐从这里开始 | 无 |
| `beetle-esp32-c3.yaml` | 网关加 128x64 单页 OLED | SSD1306、室内温湿度、天气、PM2.5 与 VOC 实体 |

## 硬件

已验证开发板为 DFRobot Beetle ESP32-C3 v1.0.0。纯网关只需 USB 供电。OLED 版默认接线：

| OLED | Beetle ESP32-C3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | GPIO0 |
| SDA | GPIO1 |

屏幕默认是 `SSD1306 128x64`、地址 `0x3C`、旋转 180°。如果 I2C 能扫描到设备但画面异常，可在入口 YAML 中尝试 `SH1106 128x64`。

## 本地配置

```sh
python3 tools/generate_esphome_secrets.py
```

再编辑 `secrets.yaml`：

- `mjtd04yl_mac_address`：台灯 BLE MAC。
- `mjtd04yl_gatt_ltmk`：64 位十六进制长期密钥。
- OLED 版还要填写室内温度、室内湿度、天气、PM2.5 与 VOC 实体 ID。

## OLED 单页界面

屏幕不轮播，默认同时显示时间、HA/台灯/Wi-Fi 状态、室内外温湿度、PM2.5 和 VOC。实心状态点表示已连接，空心点表示断开。异常提示由入口 YAML 顶部的 substitutions 控制：

| 提示 | 默认条件 | 常驻标记 |
|---|---|---|
| 室内温度偏高 / 偏低 | 比室外高或低 3°C | 温度后的 ↑ / ↓ |
| 室内湿度偏低 / 偏高 | 低于 40% 或高于 70% | 湿度后的 ↓ / ↑ |
| PM2.5 偏高 | 高于 35 | 数值后的 ↑ |
| VOC 偏高 | 高于 400 ppb | 数值后的 ↑ |

数值第一次跨过阈值时，屏幕会覆盖显示一个 5 秒消息框，随后自动回到单页面板；HA 或台灯连接状态变化也会显示同样的短消息。上述阈值只是界面提示，用户可按传感器单位与房间情况修改，不应当作医疗或法规标准。

生成脚本只补充缺失的随机凭据，不会覆盖已经存在的值，也不会把值打印到终端。`secrets.yaml` 必须保持在 Git 之外。

## 构建与刷写

```sh
esphome config esphome/beetle-esp32-c3-gateway.yaml
esphome compile esphome/beetle-esp32-c3-gateway.yaml
esphome run esphome/beetle-esp32-c3-gateway.yaml
```

把文件名换成 `beetle-esp32-c3.yaml` 即可构建 OLED 版。首次走 USB；已经在线后可以使用节点 IP OTA。构建产物在 `esphome/.esphome/build/<node>/build/`，其中所有可刷写二进制都含设备密钥，不应公开。

## 配网与 HA

入口配置没有写死家庭 Wi-Fi。设备首次启动会创建名称以 `Setup` 结尾的临时热点，也支持 Improv Serial。配网信息保存在 ESP32 flash 中，后续换 USB 电源或移动位置，只要仍能连接同一 Wi-Fi 且在台灯蓝牙范围内，就能继续工作。

Home Assistant 添加 ESPHome 节点后会出现：台灯 light、BLE 本地连接、节点状态、Wi-Fi 信号、运行时间、ESPHome 版本和重启按钮。
