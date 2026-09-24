#!/bin/bash
# 一键还原 M5StickS3 出厂固件（UIFlow2）
# 备份来源：2026-09-24 用 esptool 从本机导出的完整 8MB flash 镜像
set -e

PORT="${1:-/dev/cu.usbmodem1101}"
BACKUP="$(dirname "$0")/backup/flash_full.bin"
ESPTOOL="/tmp/esptool-venv/bin/esptool.py"
[ -x "$ESPTOOL" ] || ESPTOOL="esptool"

if [ ! -f "$BACKUP" ]; then
  echo "备份文件不存在: $BACKUP" >&2
  echo "请先确认 /tmp/stick_s3/ 目录还在（重启后 /tmp 可能被清空，" >&2
  echo "建议把 flash_full.bin 转存到永久位置后再运行）。" >&2
  exit 1
fi

echo "即将把出厂固件写回 $PORT （8MB，约 3-5 分钟，期间请勿断电）"
read -p "继续？(y/N) " ans
[ "$ans" = "y" ] || exit 1

"$ESPTOOL" --port "$PORT" --baud 921600 --chip esp32s3 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 "$BACKUP"
echo "还原完成，设备已自动复位。"
