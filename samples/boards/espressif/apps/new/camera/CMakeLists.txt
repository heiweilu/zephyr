# Camera-only firmware for CHD-ESP32-S3-BOX
#
# Pure live-preview build that omits BT/WiFi/AI so the LCD-CAM peripheral is
# not corrupted. Companion to ../launcher.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(camera)

target_sources(app PRIVATE
	src/main.c
)
