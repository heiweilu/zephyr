#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_led_strip))
#define STRIP_NODE DT_CHOSEN(zephyr_led_strip)
#else
#define STRIP_NODE DT_INVALID_NODE
#endif

void main(void)
{
    /* 尝试通过 chosen 节点获取，如果失败则通过 label */
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_led_strip))
    const struct device *led_strip = DEVICE_DT_GET(DT_CHOSEN(zephyr_led_strip));
#else
    const struct device *led_strip = device_get_binding("WS2812_LED");
#endif
    if (!led_strip || !device_is_ready(led_strip)) {
        LOG_ERR("LED strip device not found or not ready");
        return;
    }

    LOG_INF("LED strip device ready: %s", led_strip->name);

#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_led_strip))
    /* 读取 devicetree 属性：链长、reset-delay、color-mapping 长度 */
#if DT_NODE_HAS_PROP(STRIP_NODE, chain_length)
    LOG_INF("chain-length: %d", DT_PROP(STRIP_NODE, chain_length));
#endif
#if DT_NODE_HAS_PROP(STRIP_NODE, reset_delay)
    LOG_INF("reset-delay: %d us", DT_PROP(STRIP_NODE, reset_delay));
#endif
#if DT_NODE_HAS_PROP(STRIP_NODE, color_mapping)
    LOG_INF("color-mapping entries: %d", DT_PROP_LEN(STRIP_NODE, color_mapping));
#endif
#endif


    LOG_INF("Initialization info printed. No LED update performed yet.");
    /* 后续步骤：添加像素缓冲并调用 led_strip_update_rgb() */
    while (1) {
        k_sleep(K_SECONDS(2));
    }
}