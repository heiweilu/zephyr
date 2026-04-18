cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launcher_nes)

# ── Core sources (slim — no AI service / TLS / network) ──
target_sources(app PRIVATE
    src/main.c
    src/boot_splash.c
    src/app_manager.c
    src/launcher_ui.c
    src/ble_hid.c
    src/audio.c
    src/base64.c
)

# ── App sources (placeholders kept; only NES will eventually launch) ──
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

# ──────────────────────────────────────────────────────────────────────────────
#  NES emulator (nofrendo) integration
#  Step 2: compile-only verification — sources are pulled into the link via
#  a forced reference in main.c, but nofrendo_main() is NEVER actually called
#  yet. HOME launcher must keep working as before. Real launching of NES from
#  the icon is the next step.
# ──────────────────────────────────────────────────────────────────────────────

# nofrendo source location (from west-managed esp32-nesemu)
get_filename_component(WEST_TOPDIR ${ZEPHYR_BASE} DIRECTORY)
set(NOFRENDO_DIR ${WEST_TOPDIR}/modules/esp32-nesemu/components/nofrendo)

if(NOT IS_DIRECTORY "${NOFRENDO_DIR}")
    message(FATAL_ERROR
        "nofrendo not found at ${NOFRENDO_DIR}\n"
        "Run:  west update\n"
        "Or:   git clone https://github.com/espressif/esp32-nesemu "
        "${WEST_TOPDIR}/modules/esp32-nesemu")
endif()

# Collect all nofrendo .c sources
file(GLOB NOFRENDO_SRCS_ROOT  "${NOFRENDO_DIR}/*.c")
file(GLOB NOFRENDO_SRCS_CPU   "${NOFRENDO_DIR}/cpu/*.c")
file(GLOB NOFRENDO_SRCS_SNSS  "${NOFRENDO_DIR}/libsnss/*.c")
file(GLOB NOFRENDO_SRCS_MAP   "${NOFRENDO_DIR}/mappers/*.c")
file(GLOB NOFRENDO_SRCS_NES   "${NOFRENDO_DIR}/nes/*.c")
file(GLOB NOFRENDO_SRCS_SND   "${NOFRENDO_DIR}/sndhrdw/*.c")

set(NOFRENDO_SRCS
    ${NOFRENDO_SRCS_ROOT}
    ${NOFRENDO_SRCS_CPU}
    ${NOFRENDO_SRCS_SNSS}
    ${NOFRENDO_SRCS_MAP}
    ${NOFRENDO_SRCS_NES}
    ${NOFRENDO_SRCS_SND})

# Exclude config.c — we rely on osd.c stub for config_t.
# Exclude log.c — its log_init() conflicts with Zephyr CONFIG_LOG=y.
# Exclude memguard.c — replaced by src/nes/memguard.h shadow that maps
# malloc/free/strdup to picolibc directly (no _my_malloc indirection).
list(FILTER NOFRENDO_SRCS EXCLUDE REGEX "config\\.c$")
list(FILTER NOFRENDO_SRCS EXCLUDE REGEX "log\\.c$")
list(FILTER NOFRENDO_SRCS EXCLUDE REGEX "memguard\\.c$")

# nofrendo public include directories
set(NOFRENDO_INCS
    ${NOFRENDO_DIR}
    ${NOFRENDO_DIR}/cpu
    ${NOFRENDO_DIR}/libsnss
    ${NOFRENDO_DIR}/mappers
    ${NOFRENDO_DIR}/nes
    ${NOFRENDO_DIR}/sndhrdw)

# NES ROM binary embedded via INCBIN
set(NES_ROM_FILE "${CMAKE_CURRENT_LIST_DIR}/rom/sbp.nes")
if(NOT EXISTS "${NES_ROM_FILE}")
    message(FATAL_ERROR "NES ROM not found: ${NES_ROM_FILE}")
endif()

configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/src/nes/rom.S.in
    ${CMAKE_CURRENT_BINARY_DIR}/rom.S
    @ONLY)

# Add nofrendo + osd glue + ROM blob to the build
target_sources(app PRIVATE
    src/nes/osd.c
    src/nes/compat.c
    ${CMAKE_CURRENT_BINARY_DIR}/rom.S
    ${NOFRENDO_SRCS})

# Prepend our nes/ dir so noftypes.h shadows nofrendo's
target_include_directories(app BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/nes
    ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(app PRIVATE ${NOFRENDO_INCS})

# Suppress all warnings for legacy nofrendo source
foreach(src ${NOFRENDO_SRCS})
    set_source_files_properties(${src} PROPERTIES COMPILE_FLAGS "-w")
endforeach()

target_compile_definitions(app PRIVATE HOST_LITTLE_ENDIAN=1)

set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/src/nes/osd.c
    PROPERTIES COMPILE_FLAGS
    "-Wno-incompatible-pointer-types -Wno-builtin-macro-redefined")
