#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/tools/common.sh"
"$PYTHON" "$ROOT/tools/generate_profiles.py"
"$PYTHON" -m platformio run -d "$ROOT" -e waveshare_amoled_143c
mkdir -p "$ROOT/firmware"
BUILD="$ROOT/.pio/build/waveshare_amoled_143c"
cp "$BUILD/firmware.bin" "$ROOT/firmware/application.bin"
cp "$BUILD/bootloader.bin" "$ROOT/firmware/bootloader.bin"
cp "$BUILD/partitions.bin" "$ROOT/firmware/partitions.bin"
cp "$PLATFORMIO_CORE_DIR/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$ROOT/firmware/boot_app0.bin"
"$PYTHON" -m esptool --chip esp32s3 merge_bin \
  --flash_mode keep --flash_freq keep --flash_size keep \
  -o "$ROOT/firmware/car-hud-143c-full.bin" \
  0x0 "$ROOT/firmware/bootloader.bin" \
  0x8000 "$ROOT/firmware/partitions.bin" \
  0xe000 "$ROOT/firmware/boot_app0.bin" \
  0x10000 "$ROOT/firmware/application.bin"
"$PYTHON" "$ROOT/tools/manifest.py"
echo "完整固件：$ROOT/firmware/car-hud-143c-full.bin（烧录地址 0x0）"
