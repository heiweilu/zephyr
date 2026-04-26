cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smart_camera)

target_sources(app PRIVATE
	src/main.c
	src/wifi_mgr.c
	src/display_ui.c
	src/jpeg_decoder.c
	src/mjpeg_client.c
	src/track_client.c
)

target_include_directories(app PRIVATE src)

# Reuse LVGL's bundled TJpgDec (compiled in via CONFIG_LV_USE_TJPGD=y).
# Add the tjpgd source dir so we can `#include "tjpgd.h"` directly.
target_include_directories(app PRIVATE
	${ZEPHYR_BASE}/../modules/lib/gui/lvgl/src/libs/tjpgd
	${ZEPHYR_BASE}/../modules/lib/gui/lvgl/src
)
