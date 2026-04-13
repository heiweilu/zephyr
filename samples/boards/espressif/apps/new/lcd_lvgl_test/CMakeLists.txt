cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lcd_lvgl_test)

set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})

target_sources(app PRIVATE src/main.c)

target_include_directories(app PRIVATE
    ${LVGL_DIR}/demos/
)

# Widgets demo sources
target_sources_ifdef(CONFIG_LV_USE_DEMO_WIDGETS app PRIVATE
    ${LVGL_DIR}/demos/widgets/assets/img_clothes.c
    ${LVGL_DIR}/demos/widgets/assets/img_demo_widgets_avatar.c
    ${LVGL_DIR}/demos/widgets/assets/img_demo_widgets_needle.c
    ${LVGL_DIR}/demos/widgets/assets/img_lvgl_logo.c
    ${LVGL_DIR}/demos/widgets/lv_demo_widgets.c
)

# Benchmark demo sources
target_sources_ifdef(CONFIG_LV_USE_DEMO_BENCHMARK app PRIVATE
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_avatar.c
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_lvgl_logo_argb.c
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_lvgl_logo_rgb.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_12_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_14_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_16_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_18_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_20_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_24_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_26_aligned.c
    ${LVGL_DIR}/demos/benchmark/lv_demo_benchmark.c
)
