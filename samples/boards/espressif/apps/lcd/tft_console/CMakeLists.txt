cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tft_console)

# Sources for ESP32-S3 Interactive LCD Shell Console
target_sources(app PRIVATE 
    src/main.c 
    src/lcd_shell_backend.c
)
