#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${HUD_PYTHON:-}" ]]; then
  PYTHON="$HUD_PYTHON"
elif [[ -x "$ROOT/../../work/firmware-env/bin/python" ]]; then
  PYTHON="$ROOT/../../work/firmware-env/bin/python"
else
  PYTHON="$ROOT/.tools/venv/bin/python"
  if [[ ! -x "$PYTHON" ]]; then python3 -m venv "$ROOT/.tools/venv"; fi
fi
if ! "$PYTHON" -c 'from importlib.metadata import version; assert version("esptool")=="4.8.1" and version("platformio")=="6.1.18"' >/dev/null 2>&1; then
  "$PYTHON" -m pip install 'esptool==4.8.1' 'platformio==6.1.18'
fi
if [[ -z "${PLATFORMIO_CORE_DIR:-}" ]]; then
  if [[ -d "$ROOT/../../work/platformio" ]]; then
    export PLATFORMIO_CORE_DIR="$ROOT/../../work/platformio"
  else
    export PLATFORMIO_CORE_DIR="$ROOT/.tools/platformio"
  fi
fi
