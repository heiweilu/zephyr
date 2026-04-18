# launcher_full firmware for CHD-ESP32-S3-BOX
#
# Merges launcher's home-screen icon grid UI with face_recognize's
# camera + qwen-vl-plus + CosyVoice TTS pipeline.
#
# 2a: skeleton copied from face_recognize, builds 1:1.
# 2b+: home_ui.c / app_camera.c / app_ai_vision.c will replace main.c.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launcher_full)

target_sources(app PRIVATE
	src/main.c
	src/home_ui.c
	src/app_camera.c
	src/app_ai_vision.c
	src/app_imu.c
	src/jpeg_enc.c
	src/ai_client.c
	src/audio.c
	src/tts_client.c
)
