/* Minimal BLE test - just bt_enable and scan */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			  struct net_buf_simple *ad)
{
	char dev[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, dev, sizeof(dev));
	printk("[SCAN] %s RSSI %d\n", dev, rssi);
}

int main(void)
{
	int err;

	printk("\n\n=== BLE Minimal Test ===\n");
	printk("Calling bt_enable...\n");

	err = bt_enable(NULL);
	if (err) {
		printk("bt_enable failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized OK!\n");

	struct bt_le_scan_param scan_param = {
		.type     = BT_LE_SCAN_TYPE_ACTIVE,
		.options  = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
	};

	err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("Scan start failed (err %d)\n", err);
		return 0;
	}

	printk("Scanning...\n");
	return 0;
}
