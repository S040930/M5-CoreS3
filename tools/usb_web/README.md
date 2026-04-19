# M5CoreS3 USB Web Control

This tool serves a local Chromium-only Web Serial page for managing an
`m5cores3` receiver over the built-in USB Serial/JTAG port.

## Usage

1. Flash the `m5cores3` firmware build.
2. Connect the M5CoreS3 to your computer over USB.
3. Start the local server:

```bash
python3 tools/usb_web/server.py
```

4. Open the printed `http://127.0.0.1:8765/index.html` URL in Chrome or Edge.
5. Click `Connect Device` and select the M5CoreS3 serial port (the same device you use for `pio device monitor` / flash — often `cu.usbmodem*` on macOS).

Use a **data-capable** USB cable (USB-C to USB-C or USB-A to USB-C is fine). The Core S3 may have **two USB-C ports**; choose the one that exposes the **programming / USB Serial JTAG** interface. **Close PlatformIO Serial Monitor** (or any tool holding the port) before clicking Connect — only one application can open the serial port at a time.

## Disconnect and reconnect (same browser tab)

You can **Disconnect** and click **Connect Device** again without refreshing the page and **without restarting** `server.py`. The Python process only serves static files; Web Serial talks straight to the USB device. If Connect fails after a disconnect or after the board reboots (Wi‑Fi reset, pairing reset, etc.), wait a few seconds for USB to re-enumerate, then try Connect again — or refresh the page once. Ensure no other app still holds the serial port.

## Scope

The USB page supports:

- receiver status
- Wi-Fi scanning and configuration
- device name updates
- restart
- AirPlay pairing reset
- Wi-Fi credential reset

It intentionally does not expose OTA, file management, or development
diagnostics.
