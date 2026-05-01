# ADHMe — ADHD Companion Device

## Hardware
- Board: Waveshare ESP32-S3-Touch-AMOLED-1.8
- Display: SH8601 AMOLED 368x448 via QSPI
- Touch: FT3168 capacitive (dead zone ~15px top edge)
- Audio: ES8311 codec, onboard mic + speaker
- IMU: QMI8658 6-axis
- RTC: PCF85063
- Storage: TF card slot
- Port: /dev/ttyACM0

## Stack
- ESP-IDF 5.3.5
- LVGL 8.x
- FreeRTOS (built in)
- Reference: ~/dev/ESP32-S3-Touch-AMOLED-1.8/examples/ESP-IDF-v5.3.2/

## Apps
- Lock In   — focus timer + pomodoro (1/3/5/8 min)
- Check In  — 4 spectrum sliders → smart recommendation
- Check Back — nudge timer (15/30/60 min)
- Drift     — grounding reset card deck
- Anchor    — self care check + hyperfocus alarm
- Spark     — brain warm-up games
- Wind Down — end of day reflection ritual
- Quick Capture — voice note → TF card → WiFi sync → Whisper

## Button Map
- PWR hold        → power on/off
- PWR short press → wake / home
- PWR double press → Quick Capture from anywhere
- BOOT press      → context action (app-defined)

## Architecture
- Core 0: LVGL rendering + UI state machine
- Core 1: Audio capture, WiFi sync, sensor reads
- Global double-press handler sits above app layer

## Current Status (Apr 24 2026)
- LVGL 9 running on hardware ✓
- Color format: LV_COLOR_FORMAT_RGB565_SWAPPED + LCD_RGB_ELEMENT_ORDER_RGB ✓
- Draw buffers: 20-line internal DMA (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) ✓
- Home screen rendering correctly ✓
- RTC reading (time wrong, ui_timeset.c built but not wired to long-press yet)
- ui_timeset.c has unused step_labels warning to clean up

## Next Steps
1. Wire long-press on clock → STATE_TIMESET
2. Test time setter on hardware
3. Build Check In screen (sliders → LVGL)
4. Build Drift screen (card deck)
5. Wire button handler to physical PWR/BOOT buttons
