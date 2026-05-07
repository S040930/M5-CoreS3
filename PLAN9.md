# Pixel Buddy Face UI Plan

## Summary
Build the CoreS3 screen as a simple expressive assistant face: two large pixel-style eyes, a small mouth or waveform, and minimal status text. The UI should make voice state obvious at a glance while staying light enough for LVGL on 320x240.

Default visual direction:
- Black background.
- Large rounded pixel eyes centered in the screen.
- Cyan/green as normal voice color, warm yellow for thinking, red for error.
- Small `omi` label at top.
- Bottom micro status text only when useful.

## Key Changes
- Replace the current `AirPlay` / `omni` label-only UI with a state-driven face renderer in `components/screen_ui`.
- Keep the existing public API in `screen_ui.h`; drive the face from current `screen_ui_set_voice_state()`, `screen_ui_set_playing()`, and `screen_ui_set_state()`.
- Add internal face state:
  - `STANDBY`: half-closed dim eyes, slow breathing.
  - `LISTENING`: open eyes, centered pupils, subtle pulse.
  - `SENDING` / `THINKING`: pupils drift sideways, three-dot or small mouth animation.
  - `SPEAKING`: mouth/waveform animates continuously.
  - `ERROR`: red flattened eyes and short error hint.
  - AirPlay background state: preserve face, show tiny AirPlay indicator instead of switching to a dashboard.

## Implementation Notes
- Use LVGL primitive objects only: rectangles, labels, simple bars/dots. No new dependency.
- Add a small internal face model to `screen_ui.c`:
  - eye objects
  - pupil objects
  - mouth/wave bars
  - top label
  - status label
  - animation phase counter
- Use the existing LVGL timer to update animation every configured UI tick.
- Keep geometry fixed and responsive for 320x240:
  - eyes around upper-middle
  - mouth/wave below eyes
  - status line near bottom
- Preserve tap behavior: tapping while voice overlay is active still calls the existing PTT callback.

## Visual State Rules
- `SCREEN_UI_VOICE_OFF`: hide voice face only if AirPlay is the primary mode; otherwise show calm standby face.
- `SCREEN_UI_VOICE_STANDBY`: dim half-closed eyes.
- `SCREEN_UI_VOICE_LISTENING`: bright open eyes and small listening pulse.
- `SCREEN_UI_VOICE_SENDING`: eyes blink narrow, status `sending`.
- `SCREEN_UI_VOICE_THINKING`: pupils drift, mouth becomes animated dots.
- `SCREEN_UI_VOICE_SPEAKING`: animated mouth/waveform.
- `SCREEN_UI_VOICE_ERROR`: red eyes, short error text if provided.

## Test Plan
- Build: `bash scripts/check-fast.sh`.
- If dependency state still blocks full build, report the exact blocker and run the closest compile/config check available.
- Device validation:
  - Boot shows calm face.
  - `Hi ESP` changes to listening face.
  - User speech upload shows sending/thinking.
  - Assistant reply shows speaking mouth animation.
  - Timer reminder uses the speaking face.
  - Error state visibly changes to red.
  - AirPlay discoverable/idle does not replace the face with a text-only screen.

## Assumptions
- First version should be simple and robust, not a full character system.
- No new image assets, no new dependencies, no broad UI framework rewrite.
- Existing `screen_ui.h` API remains stable.
- The face should support the voice assistant first; AirPlay status stays secondary.
