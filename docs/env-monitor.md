# ENV.3 seasonal reminder

## Purpose
`env_monitor` is a local-first comfort reminder for the CoreS3 ENV.3 sensor set (`SHT30 + QMP6988`).
It samples temperature, humidity, and pressure on the device, applies seasonal thresholds locally,
and only calls the Omni voice path when a reminder is actually needed.

Current hardware assumptions:

- `SHT30` at `0x44`
- `QMP6988` probe order: `0x70`, then fallback `0x56`

Probe success now means more than “a device handle was created”. `env_monitor` only commits to a
`QMP6988` address after a real `chip_id == 0x5C` read succeeds; if `0x70` ACKs poorly or returns the
wrong chip ID, the monitor still continues to probe `0x56`.

CoreS3 does not give `env_monitor` an exclusive I2C bus. The same bus is already shared with:

- board-level audio codec / amp control
- `LTR553` auto-brightness sensor

So an `ENV.III` failure does not automatically mean “the whole I2C bus is dead”; it can also mean
that only the external ENV.III path is not ACKing, or that the current `QMP6988` register/chip-id
assumptions do not match the attached module.

Pressure is **diagnostic only**. It is logged around `101.3 kPa ± 5 kPa`, but it never triggers a reminder.

## Season mapping
Season selection is automatic and based on the local calendar:

- Summer: months `4` to `9`
- Winter: all other months

If the system time is not ready yet, the monitor keeps sampling and logging, but seasonal reminder logic
stays paused until the clock becomes valid.

## Thresholds
Configured values are seasonal comfort bands for temperature and humidity.

| Season | Temperature | Humidity |
|---|---:|---:|
| Summer | `23-28 C` | `40-80 %` |
| Winter | `20-25 C` | `30-60 %` |

Reminder logic currently uses two triggers:

- Temperature too high -> "建议开空调"
- Humidity too low -> "建议开加湿器"

If both are true, the reminder is merged into one short sentence.

## Reminder policy
- Hysteresis keeps the state from flapping when readings hover around the threshold.
- Cooldown prevents reminder spam while the condition stays out of range.
- AirPlay activity or `realtime_voice_is_response_active()` causes the reminder to be deferred.
- Deferred reminders are spoken later through `realtime_voice_speak_text()`, which reuses the existing
  Omni one-shot playback and speaker arbitration.

## Configuration
Runtime defaults are exposed in `components/app_core/Kconfig.projbuild` and mapped from `config.toml`
through `scripts/pio_prebuild.py`.

Relevant keys:

- `CONFIG_ENV_MONITOR_ENABLE`
- `CONFIG_ENV_MONITOR_POLL_INTERVAL_SEC`
- `CONFIG_ENV_MONITOR_COOLDOWN_SEC`
- `CONFIG_ENV_TEMP_HYSTERESIS_C`
- `CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT`
- `CONFIG_ENV_SUMMER_TEMP_MIN_C` / `MAX_C`
- `CONFIG_ENV_SUMMER_HUMIDITY_MIN_PCT` / `MAX_PCT`
- `CONFIG_ENV_WINTER_TEMP_MIN_C` / `MAX_C`
- `CONFIG_ENV_WINTER_HUMIDITY_MIN_PCT` / `MAX_PCT`
- `CONFIG_ENV_MONITOR_DEBUG_LOG`

## Verification
Suggested checks:

1. `bash scripts/check-fast.sh`
2. `~/.platformio/penv/bin/pio run -e m5cores3`
3. `~/.platformio/penv/bin/pio run -e m5cores3 -t upload`
4. `~/.platformio/penv/bin/pio device monitor`

Expected logs:

- `env monitor started`
- `i2c diag: shared bus with board codec + auto_brightness; detected_addrs=...`
- `i2c diag focus: 0x44=... 0x56=... 0x70=...`
- `env probe failed: no ... ack on shared bus` when the external module is absent on expected addresses
- `ENV.3 sensor ready`
- seasonal sample summaries when debug logging is enabled
- deferred reminder logs when AirPlay or voice is busy

## I2C NACK troubleshooting
When you see `i2c.master: I2C transaction unexpected nack detected`, use the new `env_monitor`
logs to answer two separate questions:

1. Does the address ACK at all?
- `0x44` missing: likely `SHT30` path / wiring / power issue
- both `0x70` and `0x56` missing: likely `QMP6988` path / wiring / power issue
- only built-in/shared-bus devices show up: more likely external ENV.III chain issue than voice logic

If `0x44=noack 0x56=noack 0x70=noack` appears together with the shared-bus board addresses still ACKing,
the intended conclusion is: the ENV.III sensor chain is absent from the bus under the current hardware
assumptions, not that the voice pipeline or the entire I2C controller is broken.

2. If the address ACKs, does it match the current driver assumption?
- `QMP6988 chip_id read failed`: address responded poorly enough that register access still failed
- `QMP6988 chip_id mismatch`: some device ACKed, but it does not look like the expected `QMP6988`
- reset / calibration / ctrl register failures: chip-id matched, but later protocol steps failed

Useful conclusions:

- `LTR553` and codec-related addresses normal, but `0x44/0x56/0x70` absent:
  prioritize ENV.III module connection, power, or module variant checks
- `0x56/0x70` ACK present but `chip_id != 0x5C`:
  prioritize module/driver mismatch over generic bus debugging
- many unrelated addresses unstable:
  suspect wider bus quality, pull-up, interference, or power sequencing issues

## Retry behavior
- Missing-ACK failures now back off on a longer retry window than transient register/protocol failures.
- The goal is to keep the diagnosis explicit without turning repeated `unexpected nack` events into a high-frequency wall of noise.
