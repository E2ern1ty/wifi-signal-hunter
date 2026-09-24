# WiFi Signal Hunter — M5StickS3 玩机项目

设备：M5Stack M5StickS3（SKU K150，ESP32-S3-PICO-1-N8R8）

- 芯片：ESP32-S3-PICO-1，8MB 片内 Flash + 8MB PSRAM
- 板载：BMI270 六轴 IMU、MEMS 麦克风（ES8311 编解码）、红外收发、
  1.14" LCD（240×135）、蜂鸣喇叭（AW8737 功放）、按键 KEY1=G11 / KEY2=G12、250mAh 电池
- USB：芯片原生 USB-Serial/JTAG，串口 `/dev/cu.usbmodem1101`
- Arduino 支持：M5Unified / M5GFX 已内置 board_M5StickS3 自动识别（通过 PM1 电源芯片探测），
  无需专用板级包；编译用通用 `esp32s3` 板定义即可

## wifi_scanner — WiFi 信号强度探测器

一个 WiFi 信号寻踪器：

- **列表页**：循环扫描周边 WiFi（约 2.5s 一轮），首次扫描按信号强度排序后
  **顺序固定**（新信号源追加到末尾，连续 3 轮扫不到才移除），显示信号格 /
  SSID / 信道 / dBm，标题栏带 scanning 状态与实时电量
- **追踪页**：按 BSSID 锁定信号源后进入，**单信道快扫（每秒 4~5 次刷新）**，
  大字号实时 dBm（EMA 平滑）、信号条、4 分钟历史曲线，以及盖革计数器式蜂鸣——
  离信号源越近响得越急，凭声音就能摸到 AP 的位置
- **重力翻转**：BMI270 检测横屏方向，倒拿 180° 屏幕自动转（0.5s 防抖）
- 追踪中每 15s 补一次全扫防止 AP 换信道跟丢；快扫连续 8 次丢失自动全扫重找

| 按键 | 列表页 | 追踪页 |
|---|---|---|
| BtnA（正面大键）短按 | 下移选择（可滚动） | 立即全扫 |
| BtnA 长按 0.6s | 显示/隐藏隐藏 SSID 信号源 | — |
| BtnB（侧键） | 进入追踪 | 返回列表 |

### 编译/烧录

```bash
arduino-cli compile --fqbn \
  'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB' \
  wifi_scanner

arduino-cli upload -p /dev/cu.usbmodem1101 --fqbn \
  'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB' \
  wifi_scanner
```

串口监视：`screen /dev/cu.usbmodem1101 115200`（输出 `[scan]` / `[track]` 日志）

> ⚠️ **macOS 串口注意**：这台 Mac 打开串口瞬间会拉高 DTR/RTS，可能把 S3 踢进下载模式
> （屏幕灭、串口只打印 "waiting for download"）。恢复方法：**拔插一次 USB** 即可正常开机。
> 刷完机后也需要拔插一次 USB 才会启动新固件（S3 的 USB 下载模式只有断电能清）。

### 还原出厂固件

出厂固件（UIFlow2）完整备份：`/tmp/stick_s3/flash_full.bin`（8MB，建议转存到安全位置）。

```bash
./restore_stock.sh            # 默认 /dev/cu.usbmodem1101
./restore_stock.sh /dev/cu.XX # 指定其他串口
```

## 相关资料

- [chat-stick](https://github.com/steveruizok/chat-stick) — M5StickS3 原生 AI 语音对话棒（参考了它的按键/板级配置结论）
- [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX)
- [M5Stack 官方文档](https://docs.m5stack.com)
