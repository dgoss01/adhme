# ADHMe Architecture

## CPU Assignment
- Core 0: LVGL rendering, touch input
- Core 1: buttons, audio, WiFi, RTC, sensors

## Task List
| Task | Core | Priority | Stack |
|------|------|----------|-------|
| lvgl_task | 0 | 5 | 8KB |
| touch_task | 0 | 6 | 4KB |
| button_task | 1 | 7 | 4KB |
| audio_task | 1 | 4 | 8KB |
| wifi_sync_task | 1 | 2 | 8KB |

## App State Machine
States: HOME, LOCK_IN, CHECK_IN, CHECK_BACK,
        DRIFT, ANCHOR, SPARK, WIND_DOWN, QUICK_CAPTURE

QUICK_CAPTURE can interrupt any state via double-press.
PWR short press always returns to HOME.

## Inter-task Communication
- Button events → EventGroup → state machine
- State changes → Queue → LVGL task
- Audio data → RingBuffer → WiFi sync task
