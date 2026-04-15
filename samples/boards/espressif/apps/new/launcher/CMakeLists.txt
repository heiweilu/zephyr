cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launcher)

# ── Core sources ──
target_sources(app PRIVATE
    src/main.c
    src/app_manager.c
    src/launcher_ui.c
    src/ble_hid.c
)

# ── Placeholder app sources ──
target_sources(app PRIVATE
    src/apps/app_ai_assistant.c
    src/apps/app_camera.c
    src/apps/app_nes.c
    src/apps/app_photos.c
    src/apps/app_terminal.c
    src/apps/app_music.c
    src/apps/app_face.c
    src/apps/app_imu.c
)
