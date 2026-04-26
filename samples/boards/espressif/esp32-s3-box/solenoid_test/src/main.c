/* solenoid_test main
 *
 * 行为：
 *   1) 启动后每 3 秒自动「开炮」一次：继电器 ON 80ms，OFF 2920ms。
 *   2) 按下 BOOT 按钮立刻「开炮」一次（手动单发）。
 *   3) 每次开炮在 USB 串口打印一行日志。
 *
 * 引脚：
 *   - relay-fire 别名 → GPIO38（ACTIVE_LOW，4路继电器 IN1）
 *   - sw0 别名 → GPIO0（BOOT 键，板载）
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(solenoid_test, LOG_LEVEL_INF);

#define PULSE_MS         80      /* 推杆通电时长，越短越省命越省 */
#define AUTO_PERIOD_MS   3000    /* 自动开炮周期 */

static const struct gpio_dt_spec relay =
	GPIO_DT_SPEC_GET(DT_ALIAS(relay_fire), gpios);
static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static struct gpio_callback button_cb;
static struct k_work btn_fire_work;
static atomic_t fire_count = ATOMIC_INIT(0);

static void fire_once(const char *src)
{
	int n = atomic_inc(&fire_count) + 1;

	LOG_INF("FIRE #%d (%s, pulse=%dms)", n, src, PULSE_MS);
	gpio_pin_set_dt(&relay, 1);   /* ACTIVE_LOW: 1 = 拉低 = 继电器吸合 */
	k_msleep(PULSE_MS);
	gpio_pin_set_dt(&relay, 0);   /* 释放 */
}

static void btn_fire_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	fire_once("button");
}

static void button_isr(const struct device *port,
		       struct gpio_callback *cb,
		       gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* 中断里不能 sleep，丢给 system workqueue */
	k_work_submit(&btn_fire_work);
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&relay)) {
		LOG_ERR("relay gpio not ready");
		return -1;
	}
	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("button gpio not ready");
		return -1;
	}

	ret = gpio_pin_configure_dt(&relay, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("relay configure failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret) {
		LOG_ERR("button configure failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) {
		LOG_ERR("button interrupt configure failed: %d", ret);
		return ret;
	}

	k_work_init(&btn_fire_work, btn_fire_handler);
	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);

	LOG_INF("solenoid_test ready: relay=GPIO38 (ACTIVE_LOW), button=BOOT (GPIO0)");
	LOG_INF("auto-fire every %d ms, pulse=%d ms; press BOOT for manual shot",
		AUTO_PERIOD_MS, PULSE_MS);

	while (1) {
		k_msleep(AUTO_PERIOD_MS - PULSE_MS);
		fire_once("auto");
	}

	return 0;
}
