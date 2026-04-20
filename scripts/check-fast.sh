#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if command -v rg >/dev/null 2>&1; then
  if rg -n 'components/boards/partitions\.csv|main/network/web_server\.c|tools/usb_web|components/display|components/spiffs_storage|m5stack-core-s3' \
      CMakeLists.txt README.md PROJECT.md docs platformio.ini sdkconfig.defaults sdkconfig.defaults.m5cores3 sdkconfig.m5cores3 >/dev/null 2>&1; then
    echo "Refusing quick check: stale pre-refactor paths are still referenced"
    exit 1
  fi
fi

echo "Running quick baseline build for m5cores3..."
~/.platformio/penv/bin/pio run -e m5cores3
