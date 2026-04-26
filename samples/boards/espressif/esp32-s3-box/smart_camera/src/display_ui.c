#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <lvgl.h>

#include "display_ui.h"
#include "jpeg_decoder.h"

LOG_MODULE_REGISTER(smart_ui, LOG_LEVEL_INF);

#define UI_THREAD_STACK 4096
#define UI_THREAD_PRIO  5
#define DEC_THREAD_STACK 8192
#define DEC_THREAD_PRIO  6
#define LCD_W 320
#define LCD_H 240
#define CANVAS_BUF_BYTES (LCD_W * LCD_H * 2)
#define JPEG_SLOT_SIZE 65536

static const struct gpio_dt_spec lcd_bl =
    GPIO_DT_SPEC_GET(DT_NODELABEL(lcd_backlight), gpios);

static const struct device *display_dev;
static lv_obj_t *status_label;
static lv_obj_t *canvas;
static lv_obj_t *xh_ring;     /* outer ring */
static lv_obj_t *xh_hline;    /* horizontal line through center */
static lv_obj_t *xh_vline;    /* vertical line through center */
static uint8_t  canvas_buf[CANVAS_BUF_BYTES]
    __attribute__((section(".ext_ram.bss")));

static struct k_mutex ui_lock;

static K_THREAD_STACK_DEFINE(ui_stack, UI_THREAD_STACK);
static struct k_thread ui_thread;
static K_THREAD_STACK_DEFINE(dec_stack, DEC_THREAD_STACK);
static struct k_thread dec_thread;

static uint8_t  jpeg_slot[JPEG_SLOT_SIZE]
    __attribute__((section(".ext_ram.bss")));
static size_t   jpeg_slot_len;
static uint32_t jpeg_slot_seq;
static bool     jpeg_slot_full;
static struct k_mutex slot_lock;
static struct k_sem   slot_filled;

static uint32_t fps_x10;

static void ui_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    while (1) {
        uint32_t sleep_ms;
        k_mutex_lock(&ui_lock, K_FOREVER);
        sleep_ms = lv_timer_handler();
        k_mutex_unlock(&ui_lock);
        if (sleep_ms == LV_NO_TIMER_READY) sleep_ms = 50;
        k_msleep(MIN(sleep_ms, 50));
    }
}

static void dec_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    uint32_t frames_in_window = 0;
    int64_t  window_start = k_uptime_get();
    while (1) {
        k_sem_take(&slot_filled, K_FOREVER);
        k_mutex_lock(&slot_lock, K_FOREVER);
        size_t   len = jpeg_slot_len;
        uint32_t seq = jpeg_slot_seq;
        jpeg_slot_full = false;
        k_mutex_unlock(&slot_lock);
        int rc = jpeg_decode_to_rgb565(jpeg_slot, len, canvas_buf);
        if (rc) { LOG_WRN("decode err seq=%u: %d", seq, rc); continue; }
        k_mutex_lock(&ui_lock, K_FOREVER);
        if (canvas) lv_obj_invalidate(canvas);
        k_mutex_unlock(&ui_lock);
        frames_in_window++;
        int64_t now = k_uptime_get();
        if (now - window_start >= 2000) {
            fps_x10 = (uint32_t)((frames_in_window * 10000) / (now - window_start));
            frames_in_window = 0;
            window_start = now;
            LOG_INF("display fps=%u.%u", fps_x10 / 10, fps_x10 % 10);
        }
    }
}

int display_ui_init(void)
{
    int ret;
    if (!gpio_is_ready_dt(&lcd_bl)) { LOG_ERR("LCD bl GPIO not ready"); return -ENODEV; }
    ret = gpio_pin_configure_dt(&lcd_bl, GPIO_OUTPUT_ACTIVE);
    if (ret) { LOG_ERR("LCD bl cfg fail: %d", ret); return ret; }
    gpio_pin_set_dt(&lcd_bl, 1);
    LOG_INF("LCD backlight ON");

    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) { LOG_ERR("Display not ready"); return -ENODEV; }

    k_mutex_init(&ui_lock);
    k_mutex_init(&slot_lock);
    k_sem_init(&slot_filled, 0, 1);

    memset(canvas_buf, 0, CANVAS_BUF_BYTES);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    status_label = lv_label_create(scr);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label, 300);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xF8F9FA), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "booting...");
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);

    display_blanking_off(display_dev);

    k_thread_create(&ui_thread, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack),
                    ui_task, NULL, NULL, NULL, UI_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&ui_thread, "smart_ui");
    return 0;
}

void display_ui_set_status(const char *text)
{
    if (!status_label || !text) return;
    k_mutex_lock(&ui_lock, K_FOREVER);
    lv_label_set_text(status_label, text);
    k_mutex_unlock(&ui_lock);
}

void display_ui_show_canvas(void)
{
    k_mutex_lock(&ui_lock, K_FOREVER);
    if (status_label) { lv_obj_delete(status_label); status_label = NULL; }
    canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(canvas, canvas_buf, LCD_W, LCD_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);

    /* Crosshair overlay: 3 simple LVGL objects above the canvas. */
    lv_color_t green = lv_color_hex(0x00FF00);

    xh_ring = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(xh_ring);
    lv_obj_set_size(xh_ring, 44, 44);
    lv_obj_set_style_radius(xh_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(xh_ring, green, 0);
    lv_obj_set_style_border_width(xh_ring, 2, 0);
    lv_obj_set_style_bg_opa(xh_ring, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(xh_ring, LV_OBJ_FLAG_HIDDEN);

    xh_hline = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(xh_hline);
    lv_obj_set_size(xh_hline, 60, 2);
    lv_obj_set_style_bg_color(xh_hline, green, 0);
    lv_obj_set_style_bg_opa(xh_hline, LV_OPA_COVER, 0);
    lv_obj_add_flag(xh_hline, LV_OBJ_FLAG_HIDDEN);

    xh_vline = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(xh_vline);
    lv_obj_set_size(xh_vline, 2, 60);
    lv_obj_set_style_bg_color(xh_vline, green, 0);
    lv_obj_set_style_bg_opa(xh_vline, LV_OPA_COVER, 0);
    lv_obj_add_flag(xh_vline, LV_OBJ_FLAG_HIDDEN);

    k_mutex_unlock(&ui_lock);

    k_thread_create(&dec_thread, dec_stack, K_THREAD_STACK_SIZEOF(dec_stack),
                    dec_task, NULL, NULL, NULL, DEC_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&dec_thread, "smart_dec");
}

void display_ui_set_target(int cx, int cy)
{
    if (!xh_ring) return;
    k_mutex_lock(&ui_lock, K_FOREVER);
    if (cx < 0 || cy < 0 || cx >= LCD_W || cy >= LCD_H) {
        lv_obj_add_flag(xh_ring,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(xh_hline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(xh_vline, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* canvas is centered, so the screen origin == canvas origin (320x240). */
        lv_obj_set_pos(xh_ring,  cx - 22, cy - 22);
        lv_obj_set_pos(xh_hline, cx - 30, cy - 1);
        lv_obj_set_pos(xh_vline, cx - 1,  cy - 30);
        lv_obj_clear_flag(xh_ring,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(xh_hline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(xh_vline, LV_OBJ_FLAG_HIDDEN);
    }
    k_mutex_unlock(&ui_lock);
}

void display_ui_push_jpeg(const uint8_t *jpeg, size_t len, uint32_t seq)
{
    static bool first_logged;
    if (!first_logged) {
        LOG_INF("first jpeg push: len=%u seq=%u", (unsigned)len, seq);
        first_logged = true;
    }
    if (!jpeg || len == 0 || len > JPEG_SLOT_SIZE) {
        LOG_WRN("push rejected: jpeg=%p len=%u", jpeg, (unsigned)len);
        return;
    }
    k_mutex_lock(&slot_lock, K_FOREVER);
    memcpy(jpeg_slot, jpeg, len);
    jpeg_slot_len = len;
    jpeg_slot_seq = seq;
    if (!jpeg_slot_full) { jpeg_slot_full = true; k_sem_give(&slot_filled); }
    k_mutex_unlock(&slot_lock);
}

uint32_t display_ui_get_fps_x10(void) { return fps_x10; }