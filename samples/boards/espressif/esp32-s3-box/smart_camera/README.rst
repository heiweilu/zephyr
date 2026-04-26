smart_camera (Phase 1 — Wi-Fi + LCD status)
============================================

Pocket form-factor "remote screen" for a high-resolution USB camera that lives
on the PC. The PC runs OpenCV / InsightFace / YOLOv8 against the camera and
streams the already-annotated 320x240 frames over Wi-Fi to this ESP32-S3
firmware (later phases).

Phase 1 only brings up the LCD and Wi-Fi.

Hardware
--------

* CHD-ESP32-S3-BOX (ESP32-S3, 8 MB PSRAM)
* ST7789V 320x240 LCD on SPI3
* USB-Serial (UART console)
* USB cable to PC for power and serial only

Build
-----

.. code-block:: console

   cp samples/boards/espressif/esp32s3/smart_camera/src/secrets.h.example \
      samples/boards/espressif/esp32s3/smart_camera/src/secrets.h
   # edit secrets.h: WIFI_SSID, WIFI_PSK, PC_SERVER_HOST

   west build -b esp32s3_devkitc/esp32s3/procpu \
              samples/boards/espressif/esp32s3/smart_camera \
              -d build/smart_camera -p
   west flash --build-dir build/smart_camera

Verification
------------

Serial console::

  *** Booting Zephyr OS ***
  [INF] === smart_camera Phase 1 ===
  [INF] Scanning WiFi ...
  [INF] WiFi connected to <SSID>
  [INF] Wi-Fi OK
  IP: 192.168.x.x
  Server: 192.168.x.x:8080

LCD shows the same three-line status.
