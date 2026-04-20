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
