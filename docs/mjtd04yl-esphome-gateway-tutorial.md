# 用 ESP32-C3 给米家充电台灯补一个本地网关

家里这盏 `MJTD04YL` 用起来一直没什么问题：能在米家里调亮度和色温，灯本身也很轻巧。问题是在 Home Assistant 里，它虽然能跟着小米账号出现，却经常只剩一个不可用的实体。

原因并不复杂。这盏灯没有 Wi-Fi，平时走的是小米 BLE Mesh；手机可以靠近控制，真正要常驻联网则需要一台支持它的米家网关。手边正好有一块 Beetle ESP32-C3，于是想法变成了：让 ESP32 负责蓝牙登录和控制，再把标准 light 实体交给 Home Assistant。

最终链路很短：

```text
Home Assistant ← Wi-Fi / ESPHome API → ESP32-C3 ← BLE Mesh → MJTD04YL
```

控制发生在局域网和蓝牙范围内。米家云只在准备阶段帮我们取一次台灯的长期密钥，之后开灯、关灯、调亮度都不需要绕云端。

## 准备

- 一盏已绑定到自己米家账号的 `MJTD04YL`。
- 一块 DFRobot Beetle ESP32-C3 v1.0.0；其他 ESP32-C3 也可以尝试。
- 2.4 GHz Wi-Fi 与 Home Assistant。
- Python 3.12+，以及一根能传数据的 USB 线。
- 可选：128x64 SSD1306 I2C OLED。

先拉取项目并准备环境：

```sh
git clone https://github.com/linyuxuanlin/mjtd04yl-esphome-gateway.git
cd mjtd04yl-esphome-gateway

python3 -m venv .venv-esphome
source .venv-esphome/bin/activate
pip install -r requirements-esphome.txt
```

## 先说清楚 LTMK

ESP32 不能只凭 MAC 地址控制已经绑定的台灯。它还需要 `gatt_ltmk`，也就是这台设备的长期密钥。每盏灯的值都不同，这也是项目不能提供通用成品固件的原因。

把它当密码保管即可：不要贴到 Issue，不要放进 YAML 示例，也不要上传编译后的 BIN。仓库里的 QR 辅助脚本只把登录 token 放在内存中，最终 LTMK 写到权限为 `600` 的临时文件，整个过程不会打印密钥本身。

先安装取钥工具：

```sh
python3 -m venv .venv-tools
source .venv-tools/bin/activate
pip install -r requirements-tools.txt
```

台灯的 DID 可在 Xiaomi Home 设备诊断信息中查找，也可以按 MiService 文档临时运行 `miservice list`。找到型号 `yeelink.light.lamp21` 对应的 DID 后执行：

```sh
python tools/mjtd04yl_qr_ltmk.py \
  --did <你的台灯 DID> \
  --qr /tmp/mjtd04yl-login.png \
  --output /tmp/mjtd04yl-ltmk.json
```

终端出现 `QR_READY` 后，在 macOS 执行：

```sh
open /tmp/mjtd04yl-login.png
```

用米家或小米账号页面完成扫码确认。看到 `LTMK_READY` 后，把密钥安装到 ESPHome：

```sh
python tools/install_mjtd04yl_secret.py
```

这一步使用了非官方公开接口，可能会随小米服务调整而失效。如果账号地区不是中国大陆，需要同步调整脚本里的 region；也不要在来历不明的机器上运行账号取钥工具。

## 准备 secrets.yaml

接着生成 ESPHome API、OTA 和临时配网热点的随机凭据：

```sh
python tools/generate_esphome_secrets.py
```

打开 `esphome/secrets.yaml`，补上台灯 BLE MAC：

```yaml
mjtd04yl_mac_address: "AA:BB:CC:DD:EE:FF"
```

如果刚才的安装脚本成功，`mjtd04yl_gatt_ltmk` 已经在同一个文件中。这个文件被 `.gitignore` 排除，不要强制提交。

## 选择固件

只想把台灯接入 HA，使用纯网关版：

```sh
esphome run esphome/beetle-esp32-c3-gateway.yaml
```

如果还接了 OLED，则使用：

```sh
esphome run esphome/beetle-esp32-c3.yaml
```

OLED 默认 SCL 为 GPIO0、SDA 为 GPIO1。它把天气实体的温湿度放在 OUT 一行，把空气净化器等室内传感器放在 IN 一行。对应实体 ID 也写在 `secrets.yaml`，换成自己 Home Assistant 里的值即可。

两份入口配置都引用同一份 `packages/mjtd04yl-gateway-base.yaml`。换句话说，纯网关并不是删几行凑出来的另一套代码；BLE 协议、密钥和诊断实体仍与 OLED 版完全一致。

## 第一次启动

ESPHome 刷写完成后，如果板子还没有 Wi-Fi，会出现一个以 `Setup` 结尾的热点。连接后打开 `http://192.168.4.1/` 填入家庭 Wi-Fi，也可以直接用 Improv Serial 配网。

设备上线后，到 Home Assistant 的“设置 → 设备与服务 → 添加集成”，选择 ESPHome，输入节点 IP。加密密钥在 `esphome/secrets.yaml` 的 `esphome_api_encryption_key`。

正常情况下会看到一个“米家智能充电台灯” light 实体，以及一个“本地连接”诊断实体。后者为开启，才说明 ESP32 已经完成 BLE 登录并建立命令通道。

建议按这个顺序测试：

1. 开灯和关灯。
2. 亮度调到 20%、50%、100%。
3. 色温在暖白和冷白之间移动。
4. 给 ESP32 断电，再换普通 USB 电源启动。

## 几个容易踩的坑

台灯离 ESP32 太远时，HA 节点仍可能在线，但“本地连接”会关闭。Wi-Fi 在线和 BLE 登录是两件事，排查时先看这个诊断实体。

如果米家 App 正在近距离占用台灯连接，ESP32 可能暂时连不上，退出设备控制页后等待重连即可。无需重置或解绑台灯。

纯网关不是通用 Bluetooth Proxy。这里使用专用 BLE client，是因为普通代理只转发蓝牙流量，并不知道 Xiaomi Mesh 的登录、分片和 AES-CCM 命令格式。

最后，ESPHome 生成的 factory/OTA BIN 都含有你的 LTMK。适合留在自己的备份里，不适合发到公开 Release。真正可分享的是源码和构建方法。

## 参考与致谢

- [ESPHome Packages](https://esphome.io/components/packages/)
- [ESPHome External Components](https://esphome.io/components/external_components/)
- [ESPHome Security Best Practices](https://esphome.io/guides/security_best_practices/)
- [MiService](https://github.com/Yonsm/MiService)
- [MJTD04YL MIoT Spec](https://miot-spec.org/miot-spec-v2/instance?type=urn:miot-spec-v2:device:light:0000A001:yeelink-lamp21:1:0000C802)
