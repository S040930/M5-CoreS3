#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if command -v rg >/dev/null 2>&1; then
  if rg -n '^config\.max_uri_handlers = 32;#include ' main/network/web_server.c >/dev/null 2>&1; then
    echo "Refusing to build: detected corrupted C source prologue in main/network/web_server.c"
    exit 1
  fi
fi

echo "Running quick baseline build for m5cores3..."
~/.platformio/penv/bin/pio run -e m5cores3
