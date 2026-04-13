/*
 * BLE HID Host (Central) - Multi-device
 *
 * Scans for BLE HID devices (keyboards, mice, gamepads),
 * connects up to 2 devices simultaneously, discovers HID service,
 * subscribes to Input Reports, parses keypresses and mouse events.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

/* ── HID Service UUIDs ─────────────────────────────── */

#ifndef BT_UUID_HIDS_BOOT_KB_IN_REPORT_VAL
#define BT_UUID_HIDS_BOOT_KB_IN_REPORT_VAL 0x2A22
#endif
#define BT_UUID_HIDS_BOOT_KB_IN_REPORT \
	BT_UUID_DECLARE_16(BT_UUID_HIDS_BOOT_KB_IN_REPORT_VAL)

#ifndef BT_UUID_HIDS_BOOT_MOUSE_IN_REPORT_VAL
#define BT_UUID_HIDS_BOOT_MOUSE_IN_REPORT_VAL 0x2A33
#endif
#define BT_UUID_HIDS_BOOT_MOUSE_IN_REPORT \
	BT_UUID_DECLARE_16(BT_UUID_HIDS_BOOT_MOUSE_IN_REPORT_VAL)

#ifndef BT_UUID_HIDS_REPORT_VAL
#define BT_UUID_HIDS_REPORT_VAL 0x2A4D
#endif
#define BT_UUID_HIDS_REPORT \
	BT_UUID_DECLARE_16(BT_UUID_HIDS_REPORT_VAL)

/* ── BLE GAP Appearance codes ──────────────────────── */

#define BT_APPEARANCE_KEYBOARD 0x03C1
#define BT_APPEARANCE_MOUSE    0x03C2
#define BT_APPEARANCE_GAMEPAD  0x03C4

/* ── HID Keycode to ASCII map (US layout, partial) ── */

static const char hid_keycode_to_ascii[128] = {
	[0x00] = 0,
	[0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
	[0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
	[0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
	[0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
	[0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
	[0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
	[0x1C] = 'y', [0x1D] = 'z',
	[0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
	[0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
	[0x26] = '9', [0x27] = '0',
	[0x28] = '\n', [0x29] = 0x1B, [0x2A] = '\b', [0x2B] = '\t',
	[0x2C] = ' ',
	[0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']',
	[0x31] = '\\', [0x33] = ';', [0x34] = '\'', [0x35] = '`',
	[0x36] = ',', [0x37] = '.', [0x38] = '/',
};

#define MOD_LCTRL  BIT(0)
#define MOD_LSHIFT BIT(1)
#define MOD_LALT   BIT(2)
#define MOD_LGUI   BIT(3)
#define MOD_RCTRL  BIT(4)
#define MOD_RSHIFT BIT(5)
#define MOD_RALT   BIT(6)
#define MOD_RGUI   BIT(7)

/* ── Multi-device state ────────────────────────────── */

#define MAX_HID_DEVICES 2

enum hid_device_type {
	HID_TYPE_UNKNOWN,
	HID_TYPE_KEYBOARD,
	HID_TYPE_MOUSE,
	HID_TYPE_GAMEPAD,
};

/* Discovery phases */
#define DISC_SVC          0
#define DISC_ENUM         1
#define DISC_REPORT_CHAR  2
#define DISC_BOOT_CHAR    3
#define DISC_CCC          4

#define MAX_REPORT_CHARS 6

struct hid_device {
	struct bt_conn *conn;
	enum hid_device_type type;

	/* GATT discovery */
	struct bt_uuid_16 disc_uuid;
	struct bt_gatt_discover_params disc_params;

	/* Multi-report subscriptions */
	struct bt_gatt_subscribe_params sub_params[MAX_REPORT_CHARS];
	uint16_t report_handles[MAX_REPORT_CHARS]; /* value handles */
	int report_count;
	int report_try_idx;  /* which report we're currently subscribing */
	int sub_count;       /* how many successfully subscribed */

	int disc_phase;
	uint16_t hid_svc_start;
	uint16_t hid_svc_end;
	bool subscribed;

	/* Keyboard state */
	uint8_t prev_keys[6];
	uint8_t prev_modifiers;

	/* Mouse state */
	uint16_t prev_buttons;
};

static struct hid_device hid_devices[MAX_HID_DEVICES];
static int connect_retry_count;
#define MAX_CONNECT_RETRIES 3

/* ── Helpers ───────────────────────────────────────── */

static struct hid_device *find_device(struct bt_conn *conn)
{
	for (int i = 0; i < MAX_HID_DEVICES; i++) {
		if (hid_devices[i].conn == conn) {
			return &hid_devices[i];
		}
	}
	return NULL;
}

static struct hid_device *alloc_device(void)
{
	for (int i = 0; i < MAX_HID_DEVICES; i++) {
		if (!hid_devices[i].conn) {
			return &hid_devices[i];
		}
	}
	return NULL;
}

static int active_count(void)
{
	int n = 0;
	for (int i = 0; i < MAX_HID_DEVICES; i++) {
		if (hid_devices[i].conn) {
			n++;
		}
	}
	return n;
}

static bool is_connected(const bt_addr_le_t *addr)
{
	for (int i = 0; i < MAX_HID_DEVICES; i++) {
		if (hid_devices[i].conn &&
		    bt_addr_le_eq(addr,
				  bt_conn_get_dst(hid_devices[i].conn))) {
			return true;
		}
	}
	return false;
}

static const char *type_str(enum hid_device_type t)
{
	switch (t) {
	case HID_TYPE_KEYBOARD: return "KB";
	case HID_TYPE_MOUSE:    return "Mouse";
	case HID_TYPE_GAMEPAD:  return "Gamepad";
	default:                return "HID";
	}
}

/* ── Forward declarations ────────────────────────────── */

static void start_scan(void);
static void start_hid_discovery(struct hid_device *dev);

/* ── Bond cleanup (for RPA-rotating devices like M720) ── */

#define MAX_STALE_BONDS 4
static bt_addr_le_t stale_bonds[MAX_STALE_BONDS];
static int stale_bond_count;

static void find_stale_bond_cb(const struct bt_bond_info *info,
			       void *user_data)
{
	/* Keep bonds for currently connected devices */
	for (int i = 0; i < MAX_HID_DEVICES; i++) {
		if (hid_devices[i].conn &&
		    bt_addr_le_eq(&info->addr,
				  bt_conn_get_dst(hid_devices[i].conn))) {
			return;
		}
	}

	if (stale_bond_count < MAX_STALE_BONDS) {
		bt_addr_le_copy(&stale_bonds[stale_bond_count++],
				&info->addr);
	}
}

static void cleanup_stale_bonds(void)
{
	stale_bond_count = 0;
	bt_foreach_bond(BT_ID_DEFAULT, find_stale_bond_cb, NULL);

	for (int i = 0; i < stale_bond_count; i++) {
		char addr[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&stale_bonds[i], addr, sizeof(addr));
		printk("[BLE] Removing stale bond: %s\n", addr);
		bt_unpair(BT_ID_DEFAULT, &stale_bonds[i]);
	}
}

/* ── Keyboard report parsing ─────────────────────────── */

static void parse_keyboard_report(struct hid_device *dev,
				  const uint8_t *data, uint16_t len)
{
	if (len < 8) {
		printk("[KB] Short report (%u), raw:", len);
		for (uint16_t i = 0; i < len; i++) {
			printk(" %02x", data[i]);
		}
		printk("\n");
		return;
	}

	uint8_t modifiers = data[0];
	const uint8_t *keys = &data[2];

	printk("[KB] mod=0x%02x keys=[%02x %02x %02x %02x %02x %02x]",
	       modifiers, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]);

	/* Modifier changes */
	uint8_t mod_changed = modifiers ^ dev->prev_modifiers;
	if (mod_changed) {
		printk(" MOD:");
		if (mod_changed & MOD_LCTRL)  printk(" %cLCtrl",  (modifiers & MOD_LCTRL)  ? '+' : '-');
		if (mod_changed & MOD_LSHIFT) printk(" %cLShift", (modifiers & MOD_LSHIFT) ? '+' : '-');
		if (mod_changed & MOD_LALT)   printk(" %cLAlt",   (modifiers & MOD_LALT)   ? '+' : '-');
		if (mod_changed & MOD_LGUI)   printk(" %cLGui",   (modifiers & MOD_LGUI)   ? '+' : '-');
		if (mod_changed & MOD_RCTRL)  printk(" %cRCtrl",  (modifiers & MOD_RCTRL)  ? '+' : '-');
		if (mod_changed & MOD_RSHIFT) printk(" %cRShift", (modifiers & MOD_RSHIFT) ? '+' : '-');
		if (mod_changed & MOD_RALT)   printk(" %cRAlt",   (modifiers & MOD_RALT)   ? '+' : '-');
		if (mod_changed & MOD_RGUI)   printk(" %cRGui",   (modifiers & MOD_RGUI)   ? '+' : '-');
	}

	/* New key presses */
	for (int i = 0; i < 6; i++) {
		uint8_t kc = keys[i];
		if (kc == 0x00 || kc == 0x01) {
			continue;
		}

		bool was_pressed = false;
		for (int j = 0; j < 6; j++) {
			if (dev->prev_keys[j] == kc) {
				was_pressed = true;
				break;
			}
		}

		if (!was_pressed) {
			char ch = (kc < sizeof(hid_keycode_to_ascii)) ?
				  hid_keycode_to_ascii[kc] : 0;
			if (ch >= 'a' && ch <= 'z' &&
			    (modifiers & (MOD_LSHIFT | MOD_RSHIFT))) {
				ch -= 32;
			}
			if (ch >= 0x20 && ch < 0x7F) {
				printk(" KEY_DOWN:'%c'(0x%02x)", ch, kc);
			} else if (ch == '\n') {
				printk(" KEY_DOWN:ENTER");
			} else if (ch == '\b') {
				printk(" KEY_DOWN:BKSP");
			} else if (ch == '\t') {
				printk(" KEY_DOWN:TAB");
			} else if (ch == 0x1B) {
				printk(" KEY_DOWN:ESC");
			} else {
				printk(" KEY_DOWN:0x%02x", kc);
			}
		}
	}

	/* Key releases */
	for (int i = 0; i < 6; i++) {
		uint8_t kc = dev->prev_keys[i];
		if (kc == 0x00 || kc == 0x01) {
			continue;
		}

		bool still = false;
		for (int j = 0; j < 6; j++) {
			if (keys[j] == kc) {
				still = true;
				break;
			}
		}
		if (!still) {
			printk(" KEY_UP:0x%02x", kc);
		}
	}

	printk("\n");
	dev->prev_modifiers = modifiers;
	memcpy(dev->prev_keys, keys, 6);
}

/* ── Mouse report parsing ────────────────────────────── */

static void parse_mouse_report(struct hid_device *dev,
			       const uint8_t *data, uint16_t len)
{
	if (len < 3) {
		printk("[Mouse] Short (%u), raw:", len);
		for (uint16_t i = 0; i < len; i++) {
			printk(" %02x", data[i]);
		}
		printk("\n");
		return;
	}

	uint16_t buttons;
	int16_t x, y;
	int8_t vwheel = 0, hwheel = 0;

	if (len >= 7) {
		/*
		 * Logitech Report Protocol (7 bytes):
		 *   [btns_lo, btns_hi, X12+Y12 packed, vwheel, hwheel]
		 * X: 12-bit signed = data[2] | (data[3] & 0x0F) << 8
		 * Y: 12-bit signed = (data[3] >> 4) | (data[4] << 4)
		 */
		buttons = data[0] | (data[1] << 8);
		x = data[2] | ((data[3] & 0x0F) << 8);
		if (x & 0x800) {
			x |= 0xF000; /* sign extend 12-bit */
		}
		y = (data[3] >> 4) | (data[4] << 4);
		if (y & 0x800) {
			y |= 0xF000;
		}
		vwheel = (int8_t)data[5];
		hwheel = (int8_t)data[6];
	} else {
		/*
		 * Boot Protocol (3-4 bytes):
		 *   [buttons, X(s8), Y(s8), wheel(s8)]
		 */
		buttons = data[0];
		x = (int8_t)data[1];
		y = (int8_t)data[2];
		vwheel = (len >= 4) ? (int8_t)data[3] : 0;
	}

	uint16_t btn_changed = buttons ^ dev->prev_buttons;

	if (btn_changed || x || y || vwheel || hwheel) {
		printk("[Mouse] btn=0x%04x", buttons);

		if (btn_changed) {
			if (btn_changed & 0x01)
				printk(" %cL", (buttons & 0x01) ? '+' : '-');
			if (btn_changed & 0x02)
				printk(" %cR", (buttons & 0x02) ? '+' : '-');
			if (btn_changed & 0x04)
				printk(" %cM", (buttons & 0x04) ? '+' : '-');
			if (btn_changed & 0x08)
				printk(" %cBack", (buttons & 0x08) ? '+' : '-');
			if (btn_changed & 0x10)
				printk(" %cFwd", (buttons & 0x10) ? '+' : '-');
		}

		if (x || y) {
			printk(" dXY=(%d,%d)", x, y);
		}
		if (vwheel) {
			printk(" vw=%d", vwheel);
		}
		if (hwheel) {
			printk(" hw=%d", hwheel);
		}
		printk("\n");
	}

	dev->prev_buttons = buttons;
}

/* ── GATT notification callback ──────────────────────── */

static struct hid_device *find_device_by_sub(
	struct bt_gatt_subscribe_params *params, int *report_idx)
{
	for (int i = 0; i < MAX_HID_DEVICES; i++) {
		ptrdiff_t off = params - hid_devices[i].sub_params;
		if (off >= 0 && off < MAX_REPORT_CHARS) {
			if (report_idx) {
				*report_idx = (int)off;
			}
			return &hid_devices[i];
		}
	}
	return NULL;
}

static uint8_t notify_func(struct bt_conn *conn,
			    struct bt_gatt_subscribe_params *params,
			    const void *data, uint16_t length)
{
	int rpt_idx = -1;
	struct hid_device *dev = find_device_by_sub(params, &rpt_idx);

	if (!dev) {
		return BT_GATT_ITER_STOP;
	}

	if (!data) {
		printk("[%s] Unsubscribed (report#%d)\n",
		       type_str(dev->type), rpt_idx);
		dev->sub_count--;
		if (dev->sub_count <= 0) {
			dev->subscribed = false;
		}
		return BT_GATT_ITER_STOP;
	}

	switch (dev->type) {
	case HID_TYPE_KEYBOARD:
		parse_keyboard_report(dev, data, length);
		break;
	case HID_TYPE_MOUSE: {
		const uint8_t *d = data;
		/* Skip HID++ Logitech vendor reports:
		 * - Long (>7 bytes): not standard mouse
		 * - Short (7B, d[0]==0x10, d[1]!=0x00): HID++ short
		 */
		if (length > 7) {
			break;
		}
		if (length == 7 && d[0] == 0x10 && d[1] != 0x00) {
			break;
		}
		parse_mouse_report(dev, d, length);
		break;
	}
	default:
		printk("[%s] rpt#%d raw(%u):", type_str(dev->type),
		       rpt_idx, length);
		for (uint16_t i = 0; i < length; i++) {
			printk(" %02x", ((const uint8_t *)data)[i]);
		}
		printk("\n");
		break;
	}

	return BT_GATT_ITER_CONTINUE;
}

/* ── GATT discovery state machine ────────────────────── */

/* ── Helper: try subscribing to next Report char ─────── */

static void try_next_report(struct hid_device *dev);

static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	struct hid_device *dev = CONTAINER_OF(params,
					      struct hid_device, disc_params);
	int err;

	if (!attr) {
		/* Enumeration done → try subscribing to reports */
		if (dev->disc_phase == DISC_ENUM) {
			printk("[%s] Enum done: %d Report chars found\n",
			       type_str(dev->type), dev->report_count);
			if (dev->report_count > 0) {
				dev->report_try_idx = 0;
				try_next_report(dev);
			} else {
				/* No Report chars → try Boot */
				printk("[%s] No Report chars, trying Boot...\n",
				       type_str(dev->type));
				dev->disc_phase = DISC_BOOT_CHAR;
				if (dev->type == HID_TYPE_MOUSE) {
					memcpy(&dev->disc_uuid,
					       BT_UUID_HIDS_BOOT_MOUSE_IN_REPORT,
					       sizeof(dev->disc_uuid));
				} else {
					memcpy(&dev->disc_uuid,
					       BT_UUID_HIDS_BOOT_KB_IN_REPORT,
					       sizeof(dev->disc_uuid));
				}
				dev->disc_params.uuid = &dev->disc_uuid.uuid;
				dev->disc_params.func = discover_func;
				dev->disc_params.start_handle = dev->hid_svc_start;
				dev->disc_params.end_handle = dev->hid_svc_end;
				dev->disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

				err = bt_gatt_discover(conn, &dev->disc_params);
				if (err) {
					printk("[%s] Discover Boot failed (%d)\n",
					       type_str(dev->type), err);
				}
			}
			return BT_GATT_ITER_STOP;
		}

		/* Boot char not found either */
		if (dev->disc_phase == DISC_BOOT_CHAR) {
			printk("[%s] No usable HID characteristic found\n",
			       type_str(dev->type));
			return BT_GATT_ITER_STOP;
		}

		/* CCC not found for current report → try next */
		if (dev->disc_phase == DISC_CCC) {
			printk("[%s] CCC not found for report %d, trying next\n",
			       type_str(dev->type), dev->report_try_idx);
			dev->report_try_idx++;
			try_next_report(dev);
			return BT_GATT_ITER_STOP;
		}

		return BT_GATT_ITER_STOP;
	}

	/* Step 0: Found HID Service → enumerate all characteristics */
	if (dev->disc_phase == DISC_SVC) {
		struct bt_gatt_service_val *svc = attr->user_data;

		dev->hid_svc_start = attr->handle + 1;
		dev->hid_svc_end = svc->end_handle;
		printk("[%s] HID Service (handles %u-%u)\n",
		       type_str(dev->type), attr->handle, dev->hid_svc_end);

		/* Enumerate ALL characteristics in the service */
		dev->disc_phase = DISC_ENUM;
		dev->report_count = 0;
		dev->disc_params.uuid = NULL; /* Any UUID */
		dev->disc_params.start_handle = dev->hid_svc_start;
		dev->disc_params.end_handle = dev->hid_svc_end;
		dev->disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &dev->disc_params);
		if (err) {
			printk("[%s] Enumerate chars failed (%d)\n",
			       type_str(dev->type), err);
		}
	}
	/* Step 1: Enumerate — collect all char info */
	else if (dev->disc_phase == DISC_ENUM) {
		struct bt_gatt_chrc *chrc = attr->user_data;
		uint16_t uuid16 = 0;

		if (chrc->uuid->type == BT_UUID_TYPE_16) {
			uuid16 = BT_UUID_16(chrc->uuid)->val;
		}

		printk("[%s]   char handle=%u val=%u UUID=0x%04x props=0x%02x",
		       type_str(dev->type), attr->handle,
		       chrc->value_handle, uuid16, chrc->properties);

		/* Track Report characteristics (0x2A4D) that support NOTIFY */
		if (uuid16 == BT_UUID_HIDS_REPORT_VAL &&
		    (chrc->properties & BT_GATT_CHRC_NOTIFY) &&
		    dev->report_count < MAX_REPORT_CHARS) {
			dev->report_handles[dev->report_count] = chrc->value_handle;
			printk(" [REPORT #%d, notifiable]", dev->report_count);
			dev->report_count++;
		}
		printk("\n");

		return BT_GATT_ITER_CONTINUE; /* Continue enumerating */
	}
	/* Step 2: Found Boot char → discover CCC */
	else if (dev->disc_phase == DISC_BOOT_CHAR) {
		printk("[%s] Found Boot Input at handle %u\n",
		       type_str(dev->type), attr->handle);

		dev->disc_phase = DISC_CCC;
		dev->sub_params[dev->report_try_idx].value_handle =
			bt_gatt_attr_value_handle(attr);

		memcpy(&dev->disc_uuid, BT_UUID_GATT_CCC,
		       sizeof(dev->disc_uuid));
		dev->disc_params.uuid = &dev->disc_uuid.uuid;
		dev->disc_params.start_handle = attr->handle + 2;
		dev->disc_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

		err = bt_gatt_discover(conn, &dev->disc_params);
		if (err) {
			printk("[%s] Discover CCC failed (%d)\n",
			       type_str(dev->type), err);
		}
	}
	/* Step 3: Found CCC → subscribe, then try next report */
	else if (dev->disc_phase == DISC_CCC) {
		int idx = dev->report_try_idx;
		struct bt_gatt_subscribe_params *sp = &dev->sub_params[idx];

		sp->notify = notify_func;
		sp->value = BT_GATT_CCC_NOTIFY;
		sp->ccc_handle = attr->handle;

		err = bt_gatt_subscribe(conn, sp);
		if (err && err != -EALREADY) {
			printk("[%s] Subscribe report#%d failed (%d)\n",
			       type_str(dev->type), idx, err);
		} else {
			printk("[%s] *** SUBSCRIBED report#%d (val=%u ccc=%u) ***\n",
			       type_str(dev->type), idx,
			       sp->value_handle, attr->handle);
			dev->sub_count++;
			dev->subscribed = true;
		}

		/* Try to subscribe to next report char */
		dev->report_try_idx++;
		if (dev->report_try_idx < dev->report_count) {
			try_next_report(dev);
		} else {
			printk("[%s] All %d reports subscribed\n",
			       type_str(dev->type), dev->sub_count);
			/* Resume scanning for more devices */
			if (active_count() < MAX_HID_DEVICES) {
				start_scan();
			}
		}
	}

	return BT_GATT_ITER_STOP;
}

/* Try to subscribe to Report char at report_try_idx */
static void try_next_report(struct hid_device *dev)
{
	int err;

	if (dev->report_try_idx >= dev->report_count) {
		/* All reports tried, fall back to Boot char */
		printk("[%s] All %d reports tried, trying Boot char...\n",
		       type_str(dev->type), dev->report_count);
		dev->disc_phase = DISC_BOOT_CHAR;
		if (dev->type == HID_TYPE_MOUSE) {
			memcpy(&dev->disc_uuid,
			       BT_UUID_HIDS_BOOT_MOUSE_IN_REPORT,
			       sizeof(dev->disc_uuid));
		} else {
			memcpy(&dev->disc_uuid,
			       BT_UUID_HIDS_BOOT_KB_IN_REPORT,
			       sizeof(dev->disc_uuid));
		}
		dev->disc_params.uuid = &dev->disc_uuid.uuid;
		dev->disc_params.func = discover_func;
		dev->disc_params.start_handle = dev->hid_svc_start;
		dev->disc_params.end_handle = dev->hid_svc_end;
		dev->disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(dev->conn, &dev->disc_params);
		if (err) {
			printk("[%s] Discover Boot failed (%d)\n",
			       type_str(dev->type), err);
		}
		return;
	}

	uint16_t val_handle = dev->report_handles[dev->report_try_idx];

	printk("[%s] Trying Report #%d (val_handle=%u)...\n",
	       type_str(dev->type), dev->report_try_idx, val_handle);

	dev->disc_phase = DISC_CCC;
	dev->sub_params[dev->report_try_idx].value_handle = val_handle;

	memcpy(&dev->disc_uuid, BT_UUID_GATT_CCC,
	       sizeof(dev->disc_uuid));
	dev->disc_params.uuid = &dev->disc_uuid.uuid;
	dev->disc_params.func = discover_func;
	dev->disc_params.start_handle = val_handle + 1;
	dev->disc_params.end_handle = dev->hid_svc_end;
	dev->disc_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

	err = bt_gatt_discover(dev->conn, &dev->disc_params);
	if (err) {
		printk("[%s] Discover CCC for report#%d failed (%d)\n",
		       type_str(dev->type), dev->report_try_idx, err);
	}
}

static void start_hid_discovery(struct hid_device *dev)
{
	int err;

	dev->disc_phase = DISC_SVC;
	memcpy(&dev->disc_uuid, BT_UUID_HIDS, sizeof(dev->disc_uuid));
	dev->disc_params.uuid = &dev->disc_uuid.uuid;
	dev->disc_params.func = discover_func;
	dev->disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	dev->disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	dev->disc_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(dev->conn, &dev->disc_params);
	if (err) {
		printk("[%s] Discover HID failed (%d)\n",
		       type_str(dev->type), err);
	}
}

/* ── BLE Scanning ────────────────────────────────────── */

struct scan_result {
	bool has_hid_uuid;
	bool has_hid_appearance;
	char name[32];
	uint16_t appearance;
};

static struct scan_result current_scan;

static bool eir_found(struct bt_data *data, void *user_data)
{
	int i;

	switch (data->type) {
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED: {
		size_t len = MIN(data->data_len,
				 sizeof(current_scan.name) - 1);
		memcpy(current_scan.name, data->data, len);
		current_scan.name[len] = '\0';
		break;
	}

	case BT_DATA_GAP_APPEARANCE:
		if (data->data_len == 2) {
			memcpy(&current_scan.appearance, data->data, 2);
			current_scan.appearance =
				sys_le16_to_cpu(current_scan.appearance);
			if (current_scan.appearance == BT_APPEARANCE_KEYBOARD ||
			    current_scan.appearance == BT_APPEARANCE_MOUSE ||
			    current_scan.appearance == BT_APPEARANCE_GAMEPAD) {
				current_scan.has_hid_appearance = true;
			}
		}
		break;

	case BT_DATA_UUID16_SOME:
	case BT_DATA_UUID16_ALL:
	case BT_DATA_SOLICIT16:
		if (data->data_len % sizeof(uint16_t) != 0U) {
			break;
		}
		for (i = 0; i < data->data_len; i += sizeof(uint16_t)) {
			uint16_t u16;

			memcpy(&u16, &data->data[i], sizeof(u16));
			if (sys_le16_to_cpu(u16) == BT_UUID_HIDS_VAL) {
				current_scan.has_hid_uuid = true;
			}
		}
		break;
	}

	return true;
}

static enum hid_device_type guess_type(const char *name,
				       uint16_t appearance)
{
	/* Appearance code is most reliable */
	if (appearance == BT_APPEARANCE_KEYBOARD) {
		return HID_TYPE_KEYBOARD;
	}
	if (appearance == BT_APPEARANCE_MOUSE) {
		return HID_TYPE_MOUSE;
	}
	if (appearance == BT_APPEARANCE_GAMEPAD) {
		return HID_TYPE_GAMEPAD;
	}

	/* Fallback: name heuristic */
	if (name[0]) {
		if (strstr(name, "Keyboard") || strstr(name, "keyboard") ||
		    strstr(name, "KB")) {
			return HID_TYPE_KEYBOARD;
		}
		if (strstr(name, "Mouse") || strstr(name, "mouse") ||
		    strstr(name, "M720") || strstr(name, "M750") ||
		    strstr(name, "Triathlon")) {
			return HID_TYPE_MOUSE;
		}
		if (strstr(name, "Gamepad") || strstr(name, "gamepad") ||
		    strstr(name, "Controller") || strstr(name, "controller")) {
			return HID_TYPE_GAMEPAD;
		}
	}

	return HID_TYPE_UNKNOWN;
}

static void try_connect(const bt_addr_le_t *addr,
			enum hid_device_type type)
{
	struct hid_device *dev;
	int err;

	/* Mouse needs faster interval for responsive tracking */
	static const struct bt_le_conn_param mouse_param =
		BT_LE_CONN_PARAM_INIT(6, 16, 0, 400);
	/* 7.5ms-20ms interval, 0 latency, 4s timeout */

	dev = alloc_device();
	if (!dev) {
		printk("[SCAN] No free slot (max %d)\n", MAX_HID_DEVICES);
		return;
	}

	printk("[SCAN] Connecting as %s...\n", type_str(type));

	err = bt_le_scan_stop();
	if (err) {
		printk("[SCAN] Stop scan failed (%d)\n", err);
		return;
	}

	dev->type = type;
	const struct bt_le_conn_param *param =
		(type == HID_TYPE_MOUSE) ? &mouse_param :
					   BT_LE_CONN_PARAM_DEFAULT;
	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				param, &dev->conn);
	if (err) {
		printk("[SCAN] Create conn failed (%d)\n", err);
		memset(dev, 0, sizeof(*dev));
		start_scan();
	}
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi,
			  uint8_t type, struct net_buf_simple *ad)
{
	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	/* Skip already-connected devices */
	if (is_connected(addr)) {
		return;
	}

	/* No free slots */
	if (!alloc_device()) {
		return;
	}

	/* Reset and parse AD */
	memset(&current_scan, 0, sizeof(current_scan));
	bt_data_parse(ad, eir_found, NULL);

	/* Only log HID devices or named devices to reduce noise */
	if (current_scan.has_hid_uuid || current_scan.has_hid_appearance ||
	    current_scan.name[0]) {
		char dev[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, dev, sizeof(dev));

		if (current_scan.name[0]) {
			printk("[SCAN] %s '%s' RSSI %d", dev,
			       current_scan.name, rssi);
		} else {
			printk("[SCAN] %s RSSI %d", dev, rssi);
		}
		if (current_scan.has_hid_uuid) {
			printk(" [HID]");
		}
		if (current_scan.has_hid_appearance) {
			printk(" [Appear:0x%04x]", current_scan.appearance);
		}
		printk("\n");
	}

	/* Connect if it's an HID device */
	if (current_scan.has_hid_uuid || current_scan.has_hid_appearance) {
		enum hid_device_type dt = guess_type(current_scan.name,
						     current_scan.appearance);
		try_connect(addr, dt);
	}
}

static void start_scan(void)
{
	int err;

	if (active_count() >= MAX_HID_DEVICES) {
		printk("[BLE] All %d slots connected\n", MAX_HID_DEVICES);
		return;
	}

	struct bt_le_scan_param scan_param = {
		.type     = BT_LE_SCAN_TYPE_ACTIVE,
		.options  = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
	};

	err = bt_le_scan_start(&scan_param, device_found);
	if (err == -EALREADY) {
		return; /* Already scanning */
	}
	if (err) {
		printk("[BLE] Scan start failed (%d)\n", err);
		return;
	}

	printk("[BLE] Scanning (%d/%d connected)...\n",
	       active_count(), MAX_HID_DEVICES);
}

/* ── Connection callbacks ────────────────────────────── */

static bool le_param_req(struct bt_conn *conn,
			 struct bt_le_conn_param *param)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Param req: %s interval %u-%u latency %u timeout %u\n",
	       addr, param->interval_min, param->interval_max,
	       param->latency, param->timeout);

	/* Accept any parameters the peripheral requests */
	return true;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Params updated: %s interval %u (%.2fms) latency %u timeout %u\n",
	       addr, interval, interval * 1.25, latency, timeout);
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	struct hid_device *dev = find_device(conn);
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		printk("[BLE] Failed to connect %s (err %u)\n", addr, conn_err);
		if (dev) {
			bt_conn_unref(dev->conn);
			memset(dev, 0, sizeof(*dev));
		}
		start_scan();
		return;
	}

	if (!dev) {
		printk("[BLE] Connected %s but no device slot!\n", addr);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	printk("[BLE] Connected: %s as %s (%d/%d)\n",
	       addr, type_str(dev->type),
	       active_count(), MAX_HID_DEVICES);

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		printk("[BLE] Set security failed (%d), trying discovery\n",
		       err);
		start_hid_discovery(dev);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct hid_device *dev = find_device(conn);
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Disconnected: %s (%s), reason 0x%02x\n",
	       addr, dev ? type_str(dev->type) : "?", reason);

	if (dev) {
		bt_conn_unref(dev->conn);
		memset(dev, 0, sizeof(*dev));
	}

	connect_retry_count++;
	if (connect_retry_count >= MAX_CONNECT_RETRIES) {
		printk("[BLE] Max retries (%d). Waiting 10s...\n",
		       MAX_CONNECT_RETRIES);
		connect_retry_count = 0;
		k_sleep(K_SECONDS(10));
	}

	start_scan();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("[BLE] Security: %s level %u\n", addr, level);
		/* GATT discovery deferred to pairing_complete() */
	} else {
		printk("[BLE] Security failed: %s level %u err %d\n",
		       addr, level, err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
	.security_changed = security_changed,
};

/* ── Pairing callbacks ───────────────────────────────── */

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Pairing cancelled: %s\n", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	struct hid_device *dev = find_device(conn);
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Paired: %s (%s), bonded=%s\n",
	       addr, dev ? type_str(dev->type) : "?",
	       bonded ? "yes" : "no");

	/* Pairing & key exchange done — safe to discover HID */
	if (dev) {
		connect_retry_count = 0; /* Reset retries on success */
		start_hid_discovery(dev);
	}
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Pairing failed: %s, reason %d\n", addr, reason);

	/* Remove stale bonds (keep active connections' bonds intact) */
	cleanup_stale_bonds();
}

static struct bt_conn_auth_cb auth_cb = {
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* ── Main ────────────────────────────────────────────── */

int main(void)
{
	int err;

	printk("=== BLE HID Host (Multi-device) ===\n");
	printk("Supports: keyboards, mice, gamepads (max %d)\n\n",
	       MAX_HID_DEVICES);

	err = bt_enable(NULL);
	if (err) {
		printk("[BLE] BT init failed (%d)\n", err);
		return 0;
	}

	printk("[BLE] Bluetooth initialized\n");

	/* Clear stale bonds — keyboard rotates addresses */
	bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

	err = bt_conn_auth_cb_register(&auth_cb);
	if (err) {
		printk("[BLE] Auth cb register failed (%d)\n", err);
	}

	err = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (err) {
		printk("[BLE] Auth info cb register failed (%d)\n", err);
	}

	start_scan();
	return 0;
}
