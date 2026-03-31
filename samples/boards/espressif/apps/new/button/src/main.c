/*
 * Copyright (c) 2026 heiweilu
 * SPDX-License-Identifier: Apache-2.0
 *
 * Button interrupt demo with debounce.
 * GPIO0 (BOOT) edge-triggered callback + 30ms delayed work for debounce.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* DTS alias sw0 -> GPIO0 BOOT button (active-low, pull-up) */
#define SW0_NODE DT_ALIAS(sw0)

#if !DT_NODE_HAS_STATUS(SW0_NODE, okay)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif

#define DEBOUNCE_MS 30

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;
static struct k_work_delayable debounce_work;
static int last_state = -1;

/** Delayed work handler: read stable button state after debounce period. */
static void debounce_handler(struct k_work *work)
{
	int val = gpio_pin_get_dt(&button);

	if (val != last_state) {
		last_state = val;
		printk("Button %s (pin %d)\n",
		       val ? "PRESSED" : "RELEASED", button.pin);
	}
}

/** GPIO interrupt callback: schedule debounce work, resetting timer on bounce. */
static void button_isr(const struct device *dev, struct gpio_callback *cb,
		       uint32_t pins)
{
	k_work_reschedule(&debounce_work, K_MSEC(DEBOUNCE_MS));
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button)) {
		printk("Error: button device %s is not ready\n", button.port->name);
		return 0;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n",
		       ret, button.port->name, button.pin);
		return 0;
	}

	/* Trigger on both rising and falling edges */
	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n",
		       ret, button.port->name, button.pin);
		return 0;
	}

	k_work_init_delayable(&debounce_work, debounce_handler);

	gpio_init_callback(&button_cb_data, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	printk("Button configured at %s pin %d (debounce %dms)\n",
	       button.port->name, button.pin, DEBOUNCE_MS);
	printk("Press the button\n");

	return 0;
}
