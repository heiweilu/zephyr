<div align="center">

# ✨ CHD-ESP32-S3-BOX Launcher

**A polished, multi-app desktop OS for the ESP32-S3 — built on Zephyr RTOS + LVGL.**

![Zephyr](https://img.shields.io/badge/Zephyr-v4.2-7c5295?style=flat-square&logo=zephyr&logoColor=white)
![ESP32-S3](https://img.shields.io/badge/ESP32--S3-PSRAM%208MB-E7352C?style=flat-square&logo=espressif&logoColor=white)
![LVGL](https://img.shields.io/badge/LVGL-9.x-2196F3?style=flat-square)
![BLE](https://img.shields.io/badge/BLE-HID%20Mouse-009688?style=flat-square&logo=bluetooth&logoColor=white)
![Audio](https://img.shields.io/badge/Audio-ES8311%20%2F%20ES7210-A855F7?style=flat-square)
![License](https://img.shields.io/badge/License-Apache%202.0-blue?style=flat-square)

A miniature smartphone-style launcher that runs natively on the
**CHD-ESP32-S3-BOX** dev board: 320×240 ST7789V LCD, ES8311 DAC,
ES7210 ADC, 8 MB PSRAM, BLE, Wi-Fi, microphone, camera, IMU.

</div>

---

## 🚀 Features

| App | Icon | What it does |
|---|---|---|
| 🤖 **AI Assistant** | Indigo orb | One-tap voice loop — ASR (Qwen3-ASR-flash) → LLM (Qwen-turbo) → TTS (Qwen3-TTS-flash) over DashScope, ~8 s end-to-end |
| 🎵 **Music** | Purple play | Embedded 30 s 16 kHz mono PCM clip, light-neumorphic player UI with progress bar |
| 📷 **Camera** | Pink lens | OV3660 live preview |
| 🎮 **NES** | Red controller | Retro NES emulator stub |
| 🖼️ **Photos** | Cyan grid | Image gallery |
| 💻 **Terminal** | Green prompt | On-screen shell |
| 🙂 **Face** | Yellow smile | Face animations |
| 📐 **IMU** | Orange axis | ICM-42607 motion demo |

Plus: **system-wide BLE-HID mouse cursor** that drives every app's UI.

## 🎨 UI Highlights

- Light neumorphic palette (#E6E9EF surface, indigo / purple accents)
- Full Chinese/English glyphs (Source Han Sans SC, 16 px CJK font)
- LVGL keyboard input group → mouse-clickable on any focusable widget
- Splash → home grid → app screen → back-to-home navigation flow

## 📐 Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  main.c  ─┬─►  launcher_ui (home grid)                       │
│           ├─►  app_manager  ◄── on_create / on_destroy       │
│           ├─►  ble_hid       (BLE-HID mouse, lv_indev)        │
│           ├─►  ai_service    (DashScope ASR/LLM/TTS, TLS 1.2)│
│           └─►  audio.c       (I2S TX/RX + ES8311/ES7210)     │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │
   apps/  app_ai_assistant.c  app_music.c  app_camera.c  …
```

- `app_manager.c` keeps a static table of `app_info_t` (name, icon, color,
  `on_create`, `on_destroy`) and switches LVGL screens on launch / back.
- `audio.c` exposes both **one-shot** (`audio_play`, `audio_record`) and
  **streaming** (`audio_stream_start` / `_feed` / `_stop`) APIs.
- `ble_hid.c` registers a Zephyr `lv_indev_drv` so the BLE-HID mouse moves
  LVGL's pointer across every screen.

## 🎵 Music app — embedded PCM

The Music app plays a **30-second clip baked directly into firmware** (no
SD card needed). The clip lives at
[`src/apps/song1_30s.pcm`](src/apps/song1_30s.pcm) as raw 16-bit signed
mono @16 kHz; CMake converts it to a C array via Zephyr's
`generate_inc_file_for_target()`:

```cmake
generate_inc_file_for_target(
    app
    src/apps/song1_30s.pcm
    ${ZEPHYR_BINARY_DIR}/include/generated/song1_30s.pcm.inc
)
```

A worker thread feeds 320-sample (20 ms) chunks through `audio_stream_*`
while a 200 ms LVGL timer drives the progress bar / time label.

### Replacing the clip

```powershell
# Source must be 16 kHz / mono / 16-bit PCM WAV
$src = "D:\path\to\your.wav"
$dst = "src\apps\song1_30s.pcm"
$bytes = [System.IO.File]::ReadAllBytes($src)
$cut = 30 * 16000 * 2          # 30 s × 16 kHz × 2 bytes
[System.IO.File]::WriteAllBytes($dst, $bytes[44..(44 + $cut - 1)])
```

> 💡 Use `ffmpeg -i input.mp3 -ar 16000 -ac 1 -sample_fmt s16 song.wav`
> to convert any source first.

## 🛠️ Build & Flash

```powershell
# Activate the Zephyr venv
cd zephyrproject; .\.venv\Scripts\Activate.ps1

# Pristine build
west build -b esp32s3_devkitc/esp32s3/procpu `
    zephyr/samples/boards/espressif/apps/new/launcher --pristine

# Flash + monitor
west flash
west espressif monitor --port COM5
```

Approximate footprint:

| Region | Used | Notes |
|---|---|---|
| FLASH | ≈ 13 % of 16 MB | bootloader + app + embedded PCM |
| DRAM | ≈ 86 % of 320 KB | LVGL + Wi-Fi + BLE |
| PSRAM (`ext_dram_seg`) | ≈ 65 % of 8 MB | LVGL framebuffers + audio slabs |
| DROM (XIP flash) | ≈ 6 % of 32 MB | constants + embedded PCM |

## 🎮 BLE Mouse Pairing

1. Power up the board, wait for the home grid
2. Put your BLE-HID mouse into pairing mode (most have a long-press button)
3. Watch UART for `[BLE] Paired ...` — the mouse cursor appears on screen
4. Click any icon to launch its app

> 🛈 The board's BOOT button (GPIO 0) is **not** used as input — push-to-talk
> in the AI assistant is triggered by clicking the on-screen Siri orb.

## ⚠️ Hardware notes

- The on-board SD card slot uses GPIO 0 / 43 / 44, which collide with
  UART0's default TX/RX pins **and** the BLE controller's resource budget.
  Embedded PCM (this design) sidesteps that conflict.
- USB CDC is the default console — no need for an extra USB-to-UART
  bridge.

## 📜 License

Apache 2.0 — same as Zephyr RTOS.
