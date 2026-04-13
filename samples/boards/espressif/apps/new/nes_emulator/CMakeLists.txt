cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nes_emulator)

# ── nofrendo source location (from west-managed esp32-nesemu) ─────────────────
# ZEPHYR_BASE = <workspace>/zephyr ; WEST_TOPDIR = <workspace>
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

# Exclude config.c — we provide our own config_t stub in osd.c
list(FILTER NOFRENDO_SRCS EXCLUDE REGEX "config\\.c$")

# nofrendo public include directories
set(NOFRENDO_INCS
    ${NOFRENDO_DIR}
    ${NOFRENDO_DIR}/cpu
    ${NOFRENDO_DIR}/libsnss
    ${NOFRENDO_DIR}/mappers
    ${NOFRENDO_DIR}/nes
    ${NOFRENDO_DIR}/sndhrdw)

# ── NES ROM binary embedded via INCBIN ───────────────────────────────────────
set(NES_ROM_FILE "${CMAKE_CURRENT_LIST_DIR}/rom/nestest.nes")
if(NOT EXISTS "${NES_ROM_FILE}")
    message(FATAL_ERROR
        "NES ROM not found: ${NES_ROM_FILE}\n"
        "Place any iNES format ROM as:  nes_emulator/rom/nestest.nes\n"
        "  e.g. nestest.nes from: https://www.qmtpro.com/~nes/misc/nestest.nes")
endif()

# Generate rom.S with the absolute ROM path substituted
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/src/rom.S.in
    ${CMAKE_CURRENT_BINARY_DIR}/rom.S
    @ONLY)

# ── Build target ─────────────────────────────────────────────────────────────
target_sources(app PRIVATE
    src/main.c
    src/osd.c
    src/compat.c
    ${CMAKE_CURRENT_BINARY_DIR}/rom.S
    ${NOFRENDO_SRCS})

# Prepend src/ so our header overrides (noftypes.h, memguard.h) shadow nofrendo's
target_include_directories(app BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(app PRIVATE ${NOFRENDO_INCS})

# Suppress all warnings for legacy nofrendo source (bool redefinition, etc.)
foreach(src ${NOFRENDO_SRCS})
    set_source_files_properties(${src} PROPERTIES COMPILE_FLAGS "-w")
endforeach()

# nofrendo preprocessor definitions
# NOTE: Do NOT define NOFRENDO_DEBUG (even as 0).
# Defining it makes memguard.h's #ifdef NOFRENDO_DEBUG trigger,
# redefining malloc/free as macros which breaks picolibc stdlib.h.
target_compile_definitions(app PRIVATE
    HOST_LITTLE_ENDIAN=1)

# Suppress bool/type-compat warnings in our OSD layer where nofrendo's
# enum-bool typedef conflicts with picolibc's _Bool keyword from stdbool.h.
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/src/osd.c
    PROPERTIES COMPILE_FLAGS
    "-Wno-incompatible-pointer-types -Wno-builtin-macro-redefined")
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/src/main.c
    PROPERTIES COMPILE_FLAGS
    "-Wno-builtin-macro-redefined")
