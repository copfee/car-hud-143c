#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == '--help' || "${1:-}" == '-h' ]]; then
  echo '用法：./flash.sh [--port /dev/cu.usbmodemXXXX] [--baud 460800] [--verify-only]'
  echo '自动选择唯一的 USB 串口；完整固件烧录到 0x0。不执行整片擦除。'
  exit 0
fi
source "$(dirname "$0")/tools/common.sh"
port=""; baud=460800; verify=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) port="${2:?缺少串口路径}"; shift 2;;
    --baud) baud="${2:?缺少波特率}"; shift 2;;
    --verify-only) verify=true; shift;;
    *) echo "未知参数：$1" >&2; exit 2;;
  esac
done
"$PYTHON" "$ROOT/tools/manifest.py" --verify
if $verify; then exit 0; fi
if [[ -z "$port" ]]; then
  port="$("$PYTHON" - <<'PY'
from serial.tools import list_ports
ports = [p for p in list_ports.comports() if p.vid is not None]
if len(ports) != 1:
    raise SystemExit('未找到唯一的 USB 串口，请连接开发板，或用 --port 指定：\n' + '\n'.join(p.device for p in ports))
print(ports[0].device)
PY
)"
fi
"$PYTHON" -m esptool --chip esp32s3 --port "$port" --baud "$baud" \
  --before default_reset --after hard_reset write_flash \
  --flash_mode keep --flash_freq keep --flash_size 8MB \
  0x0 "$ROOT/firmware/car-hud-143c-full.bin"
