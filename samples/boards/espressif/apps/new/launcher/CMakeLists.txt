cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launcher)

zephyr_compile_definitions(MBEDTLS_TLS1_2_SIG_ALG_RSA_PSS_ENABLED)
zephyr_compile_definitions(MBEDTLS_X509_RSASSA_PSS_SUPPORT)

# ── Core sources ──
target_sources(app PRIVATE
    src/main.c
    src/app_manager.c
    src/launcher_ui.c
    src/ble_hid.c
    src/ai_service.c
    src/audio.c
    src/base64.c
)

# ── App sources ──
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

# ── Embed 30s PCM clip into firmware as a C array (used by app_music) ──
generate_inc_file_for_target(
    app
    src/apps/song1_30s.pcm
    ${ZEPHYR_BINARY_DIR}/include/generated/song1_30s.pcm.inc
)
