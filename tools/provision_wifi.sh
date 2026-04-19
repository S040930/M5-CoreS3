#!/usr/bin/env bash
# Push Wi-Fi credentials to the receiver while it runs the setup AP (ESP32-AirPlay-Setup).
# Device will save to NVS and reboot. Do NOT commit passwords; pass them on the command line locally only.
#
# Usage:
#   ./tools/provision_wifi.sh "<SSID>" "<password>" [http://192.168.4.1]
#
# Example:
#   ./tools/provision_wifi.sh "JIE 9229" "your_password"
#
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <ssid> <password> [base_url]" >&2
  echo "  base_url default: http://192.168.4.1" >&2
  echo "  Connect your PC to Wi-Fi ESP32-AirPlay-Setup first." >&2
  exit 1
fi

SSID=$1
PASS=$2
BASE=${3:-http://192.168.4.1}

# jq would be nicer; use python for JSON escaping to handle quotes in SSID/password
BODY=$(python3 -c 'import json,sys; print(json.dumps({"ssid":sys.argv[1],"password":sys.argv[2]}))' "$SSID" "$PASS")

echo "POST ${BASE}/api/wifi/config"
curl -sS -X POST "${BASE}/api/wifi/config" \
  -H "Content-Type: application/json" \
  -d "$BODY" \
  | python3 -m json.tool || true

echo ""
echo "If success is true, the device will reboot and join the network."
