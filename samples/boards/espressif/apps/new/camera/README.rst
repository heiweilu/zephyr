README — Camera firmware (CHD-ESP32-S3-BOX)
=============================================

Pure camera live-preview build. NO BT / NO WiFi so the LCD-CAM peripheral can
deliver pixels without corruption.

Build & flash:

  west build -p auto -b esp32s3_devkitc/esp32s3/procpu \
      samples/boards/espressif/apps/new/camera
  west flash

Controls (BOOT button = GPIO0):

  - short press  : capture photo to PSRAM
  - long press   : enter gallery view
  - in gallery, short press  : back to camera
  - in gallery, long press   : delete current photo

Photos live in PSRAM (lost on reboot).
