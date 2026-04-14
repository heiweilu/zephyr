/*
 * BLE HID Host (Central) - Multi-device
 * Adapted for LVGL input integration — updates g_mouse shared state.
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>

#include "ble_hid.h"

/* ── Shared state for LVGL ─────────────────────────── */

struct mouse_input_state g_mouse;

/* Keyboard event queue: BT thread → LVGL main thread */
static char __aligned(4) kb_msgq_buf[KB_EVENT_QUEUE_SIZE * sizeof(struct kb_event)];
struct k_msgq kb_events;
volatile bool g_kb_connected;

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

/* ── Appearance codes ──────────────────────────────── */

#define BT_APPEARANCE_KEYBOARD 0x03C1
#define BT_APPEARANCE_MOUSE    0x03C2
#define BT_APPEARANCE_GAMEPAD  0x03C4

/* ── Keycode to ASCII ──────────────────────────────── */

static const char hid_keycode_to_ascii[128] = {
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

#define DISC_SVC          0
#define DISC_ENUM         1
#define DISC_REPORT_CHAR  2
#define DISC_BOOT_CHAR    3
#define DISC_CCC          4

#define MAX_REPORT_CHARS 6

struct hid_device {
	struct bt_conn *conn;
	enum hid_device_type type;
	struct bt_uuid_16 disc_uuid;
	struct bt_gatt_discover_params disc_params;
	struct bt_gatt_subscribe_params sub_params[MAX_REPORT_CHARS];
	uint16_t report_handles[MAX_REPORT_CHARS];
	int report_count;
	int report_try_idx;
	int sub_count;
	int disc_phase;
	uint16_t hid_svc_start;
	uint16_t hid_svc_end;
	bool subscribed;
	uint8_t prev_keys[6];
	uint8_t prev_modifiers;
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

/* ── Subscribe response callback ─────────────────────── */

static void subscribe_cb(struct bt_conn *conn, uint8_t err,
			  struct bt_gatt_subscribe_params *params)
{
	if (err) {
		printk("[KB] CCC write err=%u handle=%u\n",
		       err, params->ccc_handle);
	}
}

/* ── HID keycode to LVGL key mapping ─────────────────── */

/* LVGL key codes (from lv_indev.h):
 * LV_KEY_UP=17, DOWN=18, RIGHT=19, LEFT=20
 * LV_KEY_ENTER=10, ESC=27, BACKSPACE=8, DEL=127
 * LV_KEY_NEXT=9(TAB), PREV=11, HOME=2, END=3
 * Printable chars: ASCII 0x20-0x7E
 */
#define LV_KEY_UP        17
#define LV_KEY_DOWN      18
#define LV_KEY_RIGHT     19
#define LV_KEY_LEFT      20
#define LV_KEY_ESC       27
#define LV_KEY_DEL       127
#define LV_KEY_BACKSPACE 8
#define LV_KEY_ENTER     10
#define LV_KEY_NEXT      9
#define LV_KEY_PREV      11
#define LV_KEY_HOME      2
#define LV_KEY_END       3

static uint32_t hid_to_lv_key(uint8_t kc, uint8_t modifiers)
{
	/* Navigation keys */
	switch (kc) {
	case 0x4F: return LV_KEY_RIGHT;
	case 0x50: return LV_KEY_LEFT;
	case 0x51: return LV_KEY_DOWN;   /* Down */
	case 0x52: return LV_KEY_UP;     /* Up */
	case 0x28: return LV_KEY_ENTER;
	case 0x29: return LV_KEY_ESC;
	case 0x2A: return LV_KEY_BACKSPACE;
	case 0x2B: return LV_KEY_NEXT;  /* Tab */
	case 0x4C: return LV_KEY_DEL;
	case 0x4A: return LV_KEY_HOME;
	case 0x4D: return LV_KEY_END;
	}

	/* Printable characters */
	if (kc < sizeof(hid_keycode_to_ascii)) {
		char ch = hid_keycode_to_ascii[kc];
		if (ch >= 'a' && ch <= 'z' &&
		    (modifiers & (MOD_LSHIFT | MOD_RSHIFT))) {
			ch -= 32;
		}
		if (ch >= 0x20 && ch < 0x7F) {
			return (uint32_t)ch;
		}
	}

	return 0; /* Unknown key — skip */
}

/* ── Keyboard report parsing (pushes LVGL events) ────── */

static void parse_keyboard_report(struct hid_device *dev,
				  const uint8_t *data, uint16_t len)
{
	if (len < 8) {
		return;
	}

	uint8_t modifiers = data[0];
	const uint8_t *keys = &data[2];

	/* Detect new key presses */
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
			uint32_t lv_key = hid_to_lv_key(kc, modifiers);
			if (lv_key) {
				struct kb_event evt = {
					.key = lv_key,
					.pressed = 1
				};
				k_msgq_put(&kb_events, &evt, K_NO_WAIT);
			}
		}
	}

	/* Detect key releases */
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
			uint32_t lv_key = hid_to_lv_key(kc, dev->prev_modifiers);
			if (lv_key) {
				struct kb_event evt = {
					.key = lv_key,
					.pressed = 0
				};
				k_msgq_put(&kb_events, &evt, K_NO_WAIT);
			}
		}
	}

	dev->prev_modifiers = modifiers;
	memcpy(dev->prev_keys, keys, 6);
}

/* ── Mouse report parsing (updates g_mouse for LVGL) ── */

static void parse_mouse_report(struct hid_device *dev,
			       const uint8_t *data, uint16_t len)
{
	if (len < 3) {
		return;
	}

	uint16_t buttons;
	int16_t x, y;
	int8_t vwheel = 0;

	if (len >= 7) {
		/* Logitech Report Protocol (7 bytes) */
		buttons = data[0] | (data[1] << 8);
		x = data[2] | ((data[3] & 0x0F) << 8);
		if (x & 0x800) {
			x |= 0xF000;
		}
		y = (data[3] >> 4) | (data[4] << 4);
		if (y & 0x800) {
			y |= 0xF000;
		}
		vwheel = (int8_t)data[5];
	} else {
		/* Boot Protocol (3-4 bytes) */
		buttons = data[0];
		x = (int8_t)data[1];
		y = (int8_t)data[2];
		vwheel = (len >= 4) ? (int8_t)data[3] : 0;
	}

	/* Update shared state for LVGL */
	g_mouse.x = CLAMP((int)g_mouse.x + x, 0, SCREEN_WIDTH - 1);
	g_mouse.y = CLAMP((int)g_mouse.y + y, 0, SCREEN_HEIGHT - 1);
	g_mouse.buttons = buttons;
	g_mouse.wheel = vwheel;

	/* Log button changes */
	uint16_t btn_changed = buttons ^ dev->prev_buttons;
	if (btn_changed) {
		printk("[Mouse] btn=0x%04x", buttons);
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
		printk("\n");
	}
	if (vwheel) {
		printk("[Mouse] vw=%d\n", vwheel);
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
		/* Skip HID++ vendor reports */
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
		break;
	}

	return BT_GATT_ITER_CONTINUE;
}

/* ── GATT discovery state machine ────────────────────── */

static void try_next_report(struct hid_device *dev);

static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	struct hid_device *dev = CONTAINER_OF(params,
					      struct hid_device, disc_params);
	int err;

	if (!attr) {
		if (dev->disc_phase == DISC_ENUM) {
			printk("[%s] Enum done: %d Report chars\n",
			       type_str(dev->type), dev->report_count);
			if (dev->report_count > 0) {
				dev->report_try_idx = 0;
				try_next_report(dev);
			} else {
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
				bt_gatt_discover(conn, &dev->disc_params);
			}
			return BT_GATT_ITER_STOP;
		}
		if (dev->disc_phase == DISC_BOOT_CHAR) {
			printk("[%s] No usable HID char\n", type_str(dev->type));
			return BT_GATT_ITER_STOP;
		}
		if (dev->disc_phase == DISC_CCC) {
			dev->report_try_idx++;
			try_next_report(dev);
			return BT_GATT_ITER_STOP;
		}
		return BT_GATT_ITER_STOP;
	}

	if (dev->disc_phase == DISC_SVC) {
		struct bt_gatt_service_val *svc = attr->user_data;
		dev->hid_svc_start = attr->handle + 1;
		dev->hid_svc_end = svc->end_handle;
		printk("[%s] HID Service (handles %u-%u)\n",
		       type_str(dev->type), attr->handle, dev->hid_svc_end);

		dev->disc_phase = DISC_ENUM;
		dev->report_count = 0;
		dev->disc_params.uuid = NULL;
		dev->disc_params.start_handle = dev->hid_svc_start;
		dev->disc_params.end_handle = dev->hid_svc_end;
		dev->disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		bt_gatt_discover(conn, &dev->disc_params);
	} else if (dev->disc_phase == DISC_ENUM) {
		struct bt_gatt_chrc *chrc = attr->user_data;
		uint16_t uuid16 = 0;
		if (chrc->uuid->type == BT_UUID_TYPE_16) {
			uuid16 = BT_UUID_16(chrc->uuid)->val;
		}
		if (uuid16 == BT_UUID_HIDS_REPORT_VAL &&
		    (chrc->properties & BT_GATT_CHRC_NOTIFY) &&
		    dev->report_count < 1) {  /* Only subscribe to 1st report for keyboards */
			dev->report_handles[dev->report_count++] =
				chrc->value_handle;
		}
		return BT_GATT_ITER_CONTINUE;
	} else if (dev->disc_phase == DISC_BOOT_CHAR) {
		dev->disc_phase = DISC_CCC;
		dev->sub_params[dev->report_try_idx].value_handle =
			bt_gatt_attr_value_handle(attr);
		memcpy(&dev->disc_uuid, BT_UUID_GATT_CCC,
		       sizeof(dev->disc_uuid));
		dev->disc_params.uuid = &dev->disc_uuid.uuid;
		dev->disc_params.start_handle = attr->handle + 2;
		dev->disc_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
		bt_gatt_discover(conn, &dev->disc_params);
	} else if (dev->disc_phase == DISC_CCC) {
		int idx = dev->report_try_idx;
		struct bt_gatt_subscribe_params *sp = &dev->sub_params[idx];
		sp->notify = notify_func;
		sp->subscribe = subscribe_cb;
		sp->value = BT_GATT_CCC_NOTIFY;
		sp->ccc_handle = attr->handle;
		atomic_set_bit(sp->flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

		err = bt_gatt_subscribe(conn, sp);
		if (err && err != -EALREADY) {
			printk("[%s] Subscribe #%d failed (%d)\n",
			       type_str(dev->type), idx, err);
		} else {
			dev->sub_count++;
			dev->subscribed = true;
		}

		dev->report_try_idx++;
		if (dev->report_try_idx < dev->report_count) {
			try_next_report(dev);
		} else {
			printk("[%s] All %d reports subscribed\n",
			       type_str(dev->type), dev->sub_count);
			if (active_count() < MAX_HID_DEVICES) {
				start_scan();
			}
		}
	}

	return BT_GATT_ITER_STOP;
}

static void try_next_report(struct hid_device *dev)
{
	if (dev->report_try_idx >= dev->report_count) {
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
		bt_gatt_discover(dev->conn, &dev->disc_params);
		return;
	}

	uint16_t val_handle = dev->report_handles[dev->report_try_idx];
	dev->disc_phase = DISC_CCC;
	dev->sub_params[dev->report_try_idx].value_handle = val_handle;
	memcpy(&dev->disc_uuid, BT_UUID_GATT_CCC,
	       sizeof(dev->disc_uuid));
	dev->disc_params.uuid = &dev->disc_uuid.uuid;
	dev->disc_params.func = discover_func;
	dev->disc_params.start_handle = val_handle + 1;
	dev->disc_params.end_handle = dev->hid_svc_end;
	dev->disc_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
	bt_gatt_discover(dev->conn, &dev->disc_params);
}

static void start_hid_discovery(struct hid_device *dev)
{
	dev->disc_phase = DISC_SVC;
	memcpy(&dev->disc_uuid, BT_UUID_HIDS, sizeof(dev->disc_uuid));
	dev->disc_params.uuid = &dev->disc_uuid.uuid;
	dev->disc_params.func = discover_func;
	dev->disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	dev->disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	dev->disc_params.type = BT_GATT_DISCOVER_PRIMARY;
	bt_gatt_discover(dev->conn, &dev->disc_params);
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
		for (int i = 0; i < data->data_len; i += sizeof(uint16_t)) {
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

static enum hid_device_type guess_type(const char *name, uint16_t appearance)
{
	if (appearance == BT_APPEARANCE_KEYBOARD) return HID_TYPE_KEYBOARD;
	if (appearance == BT_APPEARANCE_MOUSE)    return HID_TYPE_MOUSE;
	if (appearance == BT_APPEARANCE_GAMEPAD)  return HID_TYPE_GAMEPAD;
	if (name[0]) {
		if (strstr(name, "Keyboard") || strstr(name, "keyboard") ||
		    strstr(name, "KB"))
			return HID_TYPE_KEYBOARD;
		if (strstr(name, "Mouse") || strstr(name, "mouse") ||
		    strstr(name, "M720") || strstr(name, "Triathlon"))
			return HID_TYPE_MOUSE;
		if (strstr(name, "Gamepad") || strstr(name, "gamepad") ||
		    strstr(name, "Controller"))
			return HID_TYPE_GAMEPAD;
	}
	return HID_TYPE_UNKNOWN;
}

static void try_connect(const bt_addr_le_t *addr,
			enum hid_device_type type)
{
	struct hid_device *dev;
	static const struct bt_le_conn_param mouse_param =
		BT_LE_CONN_PARAM_INIT(6, 16, 0, 400);

	dev = alloc_device();
	if (!dev) {
		return;
	}

	printk("[SCAN] Connecting as %s...\n", type_str(type));
	bt_le_scan_stop();

	dev->type = type;
	const struct bt_le_conn_param *param =
		(type == HID_TYPE_MOUSE) ? &mouse_param :
					   BT_LE_CONN_PARAM_DEFAULT;
	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
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
	if (is_connected(addr) || !alloc_device()) {
		return;
	}

	memset(&current_scan, 0, sizeof(current_scan));
	bt_data_parse(ad, eir_found, NULL);

	/* Only log HID devices (that we will try to connect to) */
	if (current_scan.has_hid_uuid || current_scan.has_hid_appearance) {
		char dev[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, dev, sizeof(dev));
		enum hid_device_type dt = guess_type(current_scan.name,
						     current_scan.appearance);
		printk("[SCAN] %s '%s' → %s\n", dev, current_scan.name,
		       type_str(dt));
		/* Save name for LVGL status display */
		if (dt == HID_TYPE_MOUSE && current_scan.name[0]) {
			strncpy(g_mouse.name, current_scan.name,
				sizeof(g_mouse.name) - 1);
		}
		try_connect(addr, dt);
	}
}

static void start_scan(void)
{
	if (active_count() >= MAX_HID_DEVICES) {
		return;
	}
	struct bt_le_scan_param scan_param = {
		.type     = BT_LE_SCAN_TYPE_ACTIVE,
		.options  = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err = bt_le_scan_start(&scan_param, device_found);
	if (err && err != -EALREADY) {
		printk("[BLE] Scan start failed (%d)\n", err);
	}
}

/* ── Connection callbacks ────────────────────────────── */

static bool le_param_req(struct bt_conn *conn,
			 struct bt_le_conn_param *param)
{
	return true;
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	struct hid_device *dev = find_device(conn);
	char addr[BT_ADDR_LE_STR_LEN];
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
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	printk("[BLE] Connected: %s as %s (%d/%d)\n",
	       addr, type_str(dev->type), active_count(), MAX_HID_DEVICES);

	if (dev->type == HID_TYPE_MOUSE) {
		g_mouse.connected = true;
	} else if (dev->type == HID_TYPE_KEYBOARD) {
		g_kb_connected = true;
	}

	bt_conn_set_security(conn, BT_SECURITY_L2);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct hid_device *dev = find_device(conn);
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Disconnected: %s reason 0x%02x\n", addr, reason);

	if (dev) {
		if (dev->type == HID_TYPE_MOUSE) {
			g_mouse.connected = false;
		} else if (dev->type == HID_TYPE_KEYBOARD) {
			g_kb_connected = false;
		}
		bt_conn_unref(dev->conn);
		memset(dev, 0, sizeof(*dev));
	}

	connect_retry_count++;
	if (connect_retry_count >= MAX_CONNECT_RETRIES) {
		connect_retry_count = 0;
		k_sleep(K_SECONDS(10));
	}
	start_scan();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (!err) {
		char addr[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
		printk("[BLE] Security: %s level %u\n", addr, level);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_req = le_param_req,
	.security_changed = security_changed,
};

/* ── Pairing callbacks ───────────────────────────────── */

static void auth_cancel(struct bt_conn *conn) {}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	struct hid_device *dev = find_device(conn);
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Paired: %s, bonded=%s\n", addr, bonded ? "yes" : "no");

	if (dev) {
		connect_retry_count = 0;
		start_hid_discovery(dev);
	}
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	printk("[BLE] Pairing failed, reason %d — clearing all bonds\n", reason);
	bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
}

static struct bt_conn_auth_cb auth_cb = {
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* ── Init ────────────────────────────────────────────── */

int ble_hid_init(void)
{
	int err;

	k_msgq_init(&kb_events, kb_msgq_buf,
		    sizeof(struct kb_event), KB_EVENT_QUEUE_SIZE);

	err = bt_enable(NULL);
	if (err) {
		printk("[BLE] BT init failed (%d)\n", err);
		return err;
	}
	printk("[BLE] Bluetooth initialized\n");

	settings_load();

	bt_conn_auth_cb_register(&auth_cb);
	bt_conn_auth_info_cb_register(&auth_info_cb);

	start_scan();
	return 0;
}
