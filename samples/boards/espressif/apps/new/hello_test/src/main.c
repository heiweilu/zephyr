#include <zephyr/kernel.h>

int main(void)
{
    int count = 0;
    printk("\n\n*** Hello from CHD-ESP32-S3-BOX! ***\n");
    printk("Board: %s\n", CONFIG_BOARD_TARGET);
    while (1) {
        printk("tick %d\n", count++);
        k_msleep(2000);
    }
    return 0;
}
