# Audio Fidelity QA Protocol

## Goal
- Validate that `PURE` fidelity mode improves clarity and reduces crackle/distortion without destabilizing playback timing.

## Test Matrix
- Device mode: `PURE` vs `ENHANCED`.
- Latency target: `220 ms` (balanced baseline).
- Content set:
  - Strong bass / dense mix
  - Fast transients / electronic percussion
  - Vocal-focused acoustic track

## Pre-Check
- Confirm mode via `GET /api/core/audio/fidelity`.
- Confirm runtime stats endpoint works: `GET /api/core/audio/stats`.
- Start with fresh session (new RTSP connection, no stale paused stream).

## Listening Procedure
- For each track, play 90-120 seconds in `PURE`, then repeat same segment in `ENHANCED`.
- Repeat with:
  - Pause/resume after 10+ seconds.
  - 2-3 rapid track skips.
  - Volume up/down sweep (small and large steps).

## Subjective Pass/Fail
- Pass:
  - No periodic zipper/click artifacts.
  - Vocals remain clean at high-energy passages.
  - No obvious harsh clipping on peaks.
- Fail:
  - Repeating ticks every ~1 second.
  - Audible pumping/breathing or crunchy distortion.
  - Long silence bursts after resume/skip.

## Objective Metrics
- Capture `GET /api/core/audio/stats` every 5 seconds while playing.
- Track deltas over a 5-minute window:
  - `dsp_limiter_events`
  - `late_frames`
  - `packets_dropped`
  - `buffer_underruns`
  - `output_latency_us`
- Expected in stable `PURE` run:
  - `buffer_underruns` growth near zero on healthy Wi-Fi
  - `late_frames` and `packets_dropped` remain low and non-bursting
  - `dsp_limiter_events` only rises on genuine loud passages

## Acceptance
- `PURE` accepted when:
  - Subjective score is better than or equal to `ENHANCED` on all three tracks.
  - No recurrent crackle in 10-minute continuous playback.
  - No statistically significant rise in underrun/late/drop counters.

## AirPlay Silent Playback Diagnosis

When AirPlay is "visible but silent" (mDNS discoverable, RTSP session established, but no audio output):

### Check Connection Chain
1. **Serial log for session establishment**:
   - Look for `ANNOUNCE complete:` line after iPhone initiates AirPlay
   - Verify `codec=`, `sr=`, `ch=`, `bits=`, `encrypt=` match expected values
   - Example: `ANNOUNCE complete: codec=ALAC sr=44100 ch=2 bits=16 encrypt=AES-CBC`

2. **Check RTSP method entry**:
   - Firmware should log `SETUP`, `RECORD`, `SET_PARAMETER` method entries
   - If missing, RTSP session may not be processing frames

### Check Decryption Chain
3. **Decryption diagnostics**:
   - Serial log will occasionally show `crypto_diag:` lines when decrypted payload is silent
   - Example: `crypto_diag: decrypted payload_peak=0 len=352 samples=88 count=50`
   - **If present**: AES-CBC decrypt succeeded, but payload is all zeros → key/IV mismatch or source corruption

### Check Decoder Output
4. **Decoder diagnostics**:
   - Look for `decoder_diag:` lines (logged every 50 silent frames)
   - Example: `decoder_diag: ALAC decoded silent PCM pcm_peak=0 samples=352 count=50`
   - **If present**: ALAC decoder succeeded, but PCM output is all zeros → decoder configuration issue or silent input

### Check Playout Chain
5. **Stream and output diagnostics**:
   - Look for `stream_diag:` lines when frames are queued to buffer
   - Look for `[fidelity]` lines with `in_peak=` and `out_peak=` values
   - **Both zero for extended period**: audio pipeline has real frames, but they are being muted

6. **Output source**:
   - `[fidelity]` line includes `source=real` (real decoded frame) vs. gaps/silence
   - `gap_concealment start:` indicates buffer underrun and fallback to repeat/mute
   - `silence fill:` indicates no good frame to repeat → prolonged muting

### Diagnosis Algorithm
- **Step 1**: If no `ANNOUNCE complete:` → session not established; check mDNS.
- **Step 2**: If `ANNOUNCE` OK but no `decoder_diag:` silent warnings → audio may be working; check volume.
- **Step 3**: If `decoder_diag: ... peak=0` repeatedly → fix key/IV or source.
- **Step 4**: If `[fidelity] ... in_peak=0 out_peak=0` sustained → pipeline silent; check receiver state.
- **Step 5**: If mostly `gap_concealment start:` and `silence fill:` → buffer underrun; check network/jitter.

### Enable Serial Verbose Output
To see all diagnostics:
```
CONFIG_LOG_DEFAULT_LEVEL=4  # or set in menuconfig: Component config > Log output > Default log verbosity
```
