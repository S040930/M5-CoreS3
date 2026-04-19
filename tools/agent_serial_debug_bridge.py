#!/usr/bin/env python3
"""Append DEBUG_NDJSON lines from serial (stdin) to Cursor debug log for session 3ef6c4.

Usage (from project root):
  pio device monitor 2>&1 | python3 tools/agent_serial_debug_bridge.py

Or pipe saved monitor output:
  cat serial_capture.txt | python3 tools/agent_serial_debug_bridge.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / ".cursor" / "debug-3ef6c4.log"
# Line may include UART noise before the marker; grab last JSON object on line.
PAT = re.compile(r"DEBUG_NDJSON:(\{.*\})\s*")


def main() -> None:
    for line in sys.stdin:
        m = PAT.search(line)
        if m:
            LOG.parent.mkdir(parents=True, exist_ok=True)
            with open(LOG, "a", encoding="utf-8") as f:
                f.write(m.group(1) + "\n")
        sys.stdout.write(line)


if __name__ == "__main__":
    main()
