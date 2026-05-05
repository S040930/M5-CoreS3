# Build And Verify

## Build

PlatformIO:

```bash
~/.platformio/penv/bin/pio run -e m5cores3
```

Upload:

```bash
~/.platformio/penv/bin/pio run -e m5cores3 -t upload --upload-port /dev/cu.usbmodemXXXX
```

Monitor:

```bash
~/.platformio/penv/bin/pio run -e m5cores3 -t monitor --upload-port /dev/cu.usbmodemXXXX
```

## Quick Check

```bash
bash scripts/check-fast.sh
```

The quick check intentionally fails if it still finds old pre-refactor paths such as display, SPIFFS web UI, or old board partition references in the active build surface.

## Lint

```bash
bash scripts/lint.sh
```

Notes:

- `scripts/lint.sh` expects a current `compile_commands.json`.
- Running a fresh `~/.platformio/penv/bin/pio run -e m5cores3` is the supported way to generate that compilation database.

## Runtime Verification

### AirPlay Connection Chain (Must Have Logs)

For "visible but no sound" regressions, treat runtime verification as a 2-stage check:

1. Connection chain:
- boot log contains AirPlay startup, mDNS success, and RTSP listen success on port 7000
- iPhone can discover the `_raop._tcp` target
- when selecting the target, device log must show client connection and RTSP method logs such as `ANNOUNCE`, `SETUP`, `RECORD`, or `SET_PARAMETER`
- receiver state must advance from `discoverable` to `session_establishing` or `streaming`

2. Playback chain (only after stage 1 passes):
- playback desired/running/output-active diagnostics are printed
- `audio_pipeline_start()` result is visible
- if speaker is externally owned, owner tag is printed explicitly

If RTSP startup fails, mDNS advertisement must be rolled back (no discoverable ghost target).

### Provisioned Device

Expected behavior with valid credentials already stored in NVS:

- device boots
- joins STA Wi-Fi
- publishes `_raop._tcp`
- appears as an AirPlay audio target
- plays through the CoreS3 speaker path
- volume changes and metadata updates work without deleted modules

### Misconfigured Device

Expected behavior without credentials:

- boot log clearly states credentials are missing
- receiver state enters idle/config-required
- no display UI, USB tool, HTTP page, SoftAP, or OTA fallback is started

## Validation Focus

This refactor is successful only if the default `m5cores3` product remains:

- single-board
- AirPlay-only
- single-app partitioned
- free of stale references to removed subsystems in build scripts and docs
