/*
 * app_camera.c — placeholder.
 *
 * The full launcher firmware enables BT/WiFi which corrupts the ESP32-S3
 * LCD-CAM peripheral; OV3660 cannot deliver pixel data while either is up.
 * To use the camera, build and flash the dedicated Camera firmware at
 * samples/boards/espressif/apps/new/camera/.
 */

#include <lvgl.h>
#include <zephyr/sys/printk.h>
#include "../app_manager.h"

extern const lv_font_t lv_font_source_han_sans_sc_16_cjk;

static void back_cb(lv_event_t *e)
{
(void)e;
app_manager_back_to_home();
}

static void on_create(lv_obj_t *screen)
{
lv_obj_set_style_bg_color(screen, lv_color_hex(0x111111), 0);
lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

lv_obj_t *title = lv_label_create(screen);
lv_label_set_text(title, "Camera 不可用");
lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
lv_obj_set_style_text_font(title, &lv_font_source_han_sans_sc_16_cjk, 0);
lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

lv_obj_t *desc = lv_label_create(screen);
lv_label_set_text(desc,
"本固件启用了 BT/WiFi，\n会破坏 LCD-CAM 外设。\n\n请烧录 Camera 专版固件:\nsamples/.../new/camera");
lv_obj_set_style_text_color(desc, lv_color_hex(0xCCCCCC), 0);
lv_obj_set_style_text_font(desc, &lv_font_source_han_sans_sc_16_cjk, 0);
lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
lv_obj_align(desc, LV_ALIGN_CENTER, 0, 0);

lv_obj_t *btn = lv_button_create(screen);
lv_obj_set_size(btn, 100, 36);
lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -18);
lv_obj_set_style_radius(btn, 18, 0);
lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
lv_obj_t *lbl = lv_label_create(btn);
lv_label_set_text(lbl, LV_SYMBOL_LEFT "  Home");
lv_obj_center(lbl);

lv_group_t *g = app_manager_get_kb_group();
if (g) {
lv_group_add_obj(g, btn);
}
printk("[Cam] Placeholder shown (use Camera firmware for live preview)\n");
}

static void on_destroy(void) {}

const app_info_t app_camera = {
.name = "Camera",
.icon_color = LV_COLOR_MAKE(0xEC, 0x48, 0x99),
.icon_symbol = LV_SYMBOL_IMAGE,
.on_create = on_create,
.on_destroy = on_destroy,
};