#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define BUTTON_NODE DT_ALIAS(sw0)

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;

static void button_isr(const struct device *dev, struct gpio_callback *cb,
		       uint32_t pins)
{
	int val = gpio_pin_get_dt(&button);
	printk("IRQ: Button %s (raw=%d)\n",
	       val ? "PRESSED" : "RELEASED", val);
}

int main(void)
{
	int ret;

	printk("\n*** CHD-ESP32-S3-BOX Button Test (Interrupt) ***\n");

	if (!gpio_is_ready_dt(&button)) {
		printk("ERROR: GPIO device not ready\n");
		return -1;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		printk("ERROR: gpio_pin_configure_dt failed: %d\n", ret);
		return -1;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		printk("ERROR: gpio_pin_interrupt_configure_dt failed: %d\n", ret);
		return -1;
	}

	gpio_init_callback(&button_cb_data, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	printk("Button on GPIO%d ready (IRQ mode). Press Boot button...\n",
	       button.pin);

	while (1) {
		k_msleep(1000);
	}

	return 0;
}
