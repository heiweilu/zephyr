README — Camera firmware (CHD-ESP32-S3-BOX)
=============================================

Pure camera live-preview + in-memory gallery build. NO BT / NO WiFi so the
LCD-CAM peripheral can deliver pixels without corruption (those subsystems
were proven to corrupt the parallel DVP bus on this board, hence the split
firmware approach: launcher firmware vs. this dedicated camera firmware).

Build & flash::

    west build -p auto -b esp32s3_devkitc/esp32s3/procpu \
        -d build_camera samples/boards/espressif/apps/new/camera
    west flash -d build_camera

Display path
------------

Frames are written to the ST7789V LCD via ``display_write()`` directly,
bypassing LVGL. LVGL's image rendering pipeline produced tearing / byte-order
artefacts on this board; direct LCD writes (mirroring the ``camera_test``
sample pattern) eliminate them.

Controls (BOOT button = GPIO0, single-button UI)
------------------------------------------------

LIVE mode (camera preview):

  - short press (<1.5 s) : capture current frame to PSRAM
  - long  press (>=1.5 s): enter GALLERY (jumps to most recent photo)

GALLERY mode:

  - short press (<1.5 s)        : next photo (wraps around)
  - long  press (1.5 s - 3 s)   : delete current photo (auto-exit if empty)
  - extra-long press (>=3 s)    : exit GALLERY back to LIVE

Visual indicator: in GALLERY mode an 8-pixel banner is drawn at the top of
the LCD as a *segmented progress bar* -- divided into ``photo_count`` equal
segments; the segment for the currently viewed photo is white, the rest red.
This shows both "you are in the gallery" and "which photo of how many".

Storage
-------

Photos live in a static PSRAM array (``.ext_ram.bss``)::

    photos[10][320 * 240 * 2] = 1.5 MB

Capacity: 10 photos. Lost on reboot.

Implementation notes
--------------------

- Two threads + msgq: ``main`` dequeues video buffers and posts to
  ``disp_msgq``; ``disp_thread`` consumes, blits to LCD (or to gallery),
  and re-enqueues; ``btn_thread`` polls BOOT button at 100 Hz.
- Camera stream runs continuously; in GALLERY mode the disp thread silently
  consumes vbufs and re-enqueues them but draws the chosen photo to the LCD
  instead. This avoids the complexity of ``video_stream_stop`` /
  ``video_stream_start`` on mode switch.
- Capture is implemented as a flag (``capture_request``) sampled by the disp
  thread before blit, so the saved photo matches what was on screen.
- Delete is implemented in disp thread (``gallery_delete_request`` flag) via
  ``memcpy`` shift-down; ``gallery_idx`` is clamped; ``photo_count == 0``
  forces back to LIVE.
