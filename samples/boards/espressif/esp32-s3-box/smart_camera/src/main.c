#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "display_ui.h"
#include "wifi_mgr.h"
#include "mjpeg_client.h"
#include "track_client.h"
#include "secrets.h"
LOG_MODULE_REGISTER(smart_main, LOG_LEVEL_INF);

static void on_jpeg_frame(const struct mjpeg_frame *f, void *user)
{
    ARG_UNUSED(user);
    if (!f) return;
    display_ui_push_jpeg(f->data, f->len, f->seq);
}

static void on_track(int cx, int cy, void *user)
{
    ARG_UNUSED(user);
    display_ui_set_target(cx, cy);
}

int main(void)
{
    int ret;
    char ip[32] = {0};
    char status[96];

    LOG_INF("smart_camera boot");

    ret = display_ui_init();
    if (ret) { LOG_ERR("display_ui_init: %d", ret); return ret; }

    display_ui_set_status("Connecting Wi-Fi...");

    ret = wifi_mgr_connect_blocking(ip, sizeof(ip));
    if (ret) {
        LOG_ERR("wifi connect: %d", ret);
        snprintf(status, sizeof(status), "Wi-Fi FAIL: %d", ret);
        display_ui_set_status(status);
        return ret;
    }

    LOG_INF("Wi-Fi OK ip=%s server=%s:%d", ip, PC_SERVER_HOST, PC_SERVER_PORT);
    snprintf(status, sizeof(status), "ip=%s\nsrv=%s:%d", ip, PC_SERVER_HOST, PC_SERVER_PORT);
    display_ui_set_status(status);

    k_msleep(1500);

    display_ui_show_canvas();

    ret = mjpeg_client_start(PC_SERVER_HOST, PC_SERVER_PORT, "/mjpeg",
                             on_jpeg_frame, NULL);
    if (ret) { LOG_ERR("mjpeg start: %d", ret); return ret; }

    ret = track_client_start(PC_SERVER_HOST, PC_SERVER_PORT, on_track, NULL);
    if (ret) { LOG_ERR("track start: %d", ret); return ret; }

    while (1) {
        k_msleep(5000);
        LOG_INF("display fps=%u.%u", display_ui_get_fps_x10()/10, display_ui_get_fps_x10()%10);
    }
    return 0;
}