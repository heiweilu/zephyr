# Face recognize firmware for CHD-ESP32-S3-BOX
#
# Phase 0: live preview + WiFi connect (verify LCD-CAM coexistence)
# Phase 1+: HTTP/TLS to Aliyun bailian qwen-vl-plus

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(face_recognize)

target_sources(app PRIVATE
	src/main.c
	src/jpeg_enc.c
	src/ai_client.c
	src/audio.c
	src/tts_client.c
)
