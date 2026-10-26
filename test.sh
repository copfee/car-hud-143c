#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/tools/common.sh"
"$PYTHON" "$ROOT/tools/generate_profiles.py"
NATIVE="$ROOT/.tools/native"
cmake -S "$ROOT/tests" -B "$NATIVE" -DCMAKE_BUILD_TYPE=Debug -DPython3_EXECUTABLE="$PYTHON"
cmake --build "$NATIVE" -j 8
ctest --test-dir "$NATIVE" --output-on-failure
"$PYTHON" -m unittest discover -s "$ROOT/tests" -p 'test_*.py'
