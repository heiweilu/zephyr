/*
 * BLE HID Host (Central) - Phase 1
 *
 * Scans for BLE HID keyboards, connects, discovers HID service,
 * subscribes to Boot Keyboard Input Report, prints keypresses to console.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
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
/* Most HID UUIDs defined in <zephyr/bluetooth/uuid.h>. Only define missing ones. */

/* Boot Keyboard Input Report: 0x2A22 (not in all Zephyr versions) */
#ifndef BT_UUID_HIDS_BOOT_KB_IN_REPORT_VAL
#define BT_UUID_HIDS_BOOT_KB_IN_REPORT_VAL 0x2A22
#endif
#define BT_UUID_HIDS_BOOT_KB_IN_REPORT \
	BT_UUID_DECLARE_16(BT_UUID_HIDS_BOOT_KB_IN_REPORT_VAL)

/* HID Report: 0x2A4D (fallback for keyboards without Boot Protocol) */
#ifndef BT_UUID_HIDS_REPORT_VAL
#define BT_UUID_HIDS_REPORT_VAL 0x2A4D
#endif
#define BT_UUID_HIDS_REPORT \
	BT_UUID_DECLARE_16(BT_UUID_HIDS_REPORT_VAL)

/* ── HID Keycode to ASCII map (US layout, partial) ── */

static const char hid_keycode_to_ascii[128] = {
	[0x00] = 0,    /* No event */
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
	[0x28] = '\n', /* Enter */
	[0x29] = 0x1B, /* Escape */
	[0x2A] = '\b', /* Backspace */
	[0x2B] = '\t', /* Tab */
	[0x2C] = ' ',  /* Space */
	[0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']',
	[0x31] = '\\', [0x33] = ';', [0x34] = '\'', [0x35] = '`',
	[0x36] = ',', [0x37] = '.', [0x38] = '/',
};

/* Modifier key bit positions in byte 0 of boot keyboard report */
#define MOD_LCTRL  BIT(0)
#define MOD_LSHIFT BIT(1)
#define MOD_LALT   BIT(2)
#define MOD_LGUI   BIT(3)
#define MOD_RCTRL  BIT(4)
#define MOD_RSHIFT BIT(5)
#define MOD_RALT   BIT(6)
#define MOD_RGUI   BIT(7)

/* ── State ───────────────────────────────────────────── */

static struct bt_conn *default_conn;
static int connect_retry_count;
#define MAX_CONNECT_RETRIES 3
static struct bt_uuid_16 discover_uuid = BT_UUID_INIT_16(0);
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;

/* Discovery state machine */
#define DISC_SVC          0  /* Discovering HID service */
#define DISC_BOOT_KB_CHAR 1  /* Looking for Boot KB Input Report (0x2A22) */
#define DISC_REPORT_CHAR  2  /* Fallback: looking for Report (0x2A4D) */
#define DISC_CCC          3  /* Looking for CCC descriptor */
static int disc_phase;
static uint16_t hid_svc_start_handle;
static uint16_t hid_svc_end_handle;

/* Previous report for key change detection */
static uint8_t prev_keys[6];
static uint8_t prev_modifiers;

/* ── Forward declarations ────────────────────────────── */

static void start_scan(void);

/* ── Boot Keyboard Report parsing ────────────────────── */

/*
 * Boot Keyboard Input Report format (8 bytes):
 *   Byte 0: Modifier keys (bitfield)
 *   Byte 1: Reserved (0x00)
 *   Byte 2-7: Key codes (up to 6 simultaneous keys)
 */
static void parse_boot_keyboard_report(const uint8_t *data, uint16_t len)
{
	if (len < 8) {
		printk("[HID] Short report (%u bytes), raw:", len);
		for (uint16_t i = 0; i < len; i++) {
			printk(" %02x", data[i]);
		}
		printk("\n");
		return;
	}

	uint8_t modifiers = data[0];
	/* data[1] is reserved */
	const uint8_t *keys = &data[2];

	/* Print raw report */
	printk("[HID] mod=0x%02x keys=[%02x %02x %02x %02x %02x %02x]",
	       modifiers, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]);

	/* Detect modifier changes */
	uint8_t mod_changed = modifiers ^ prev_modifiers;
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

	/* Detect new key presses */
	for (int i = 0; i < 6; i++) {
		uint8_t kc = keys[i];
		if (kc == 0x00 || kc == 0x01) { /* No event or rollover error */
			continue;
		}

		/* Check if this key was in previous report */
		bool was_pressed = false;
		for (int j = 0; j < 6; j++) {
			if (prev_keys[j] == kc) {
				was_pressed = true;
				break;
			}
		}

		if (!was_pressed) {
			/* New key press */
			char ch = (kc < sizeof(hid_keycode_to_ascii)) ?
				  hid_keycode_to_ascii[kc] : 0;

			/* Apply shift for uppercase letters */
			if (ch >= 'a' && ch <= 'z' &&
			    (modifiers & (MOD_LSHIFT | MOD_RSHIFT))) {
				ch -= 32; /* to uppercase */
			}

			if (ch >= 0x20 && ch < 0x7F) {
				printk(" KEY_DOWN:'%c'(0x%02x)", ch, kc);
			} else if (ch == '\n') {
				printk(" KEY_DOWN:ENTER(0x%02x)", kc);
			} else if (ch == '\b') {
				printk(" KEY_DOWN:BKSP(0x%02x)", kc);
			} else if (ch == '\t') {
				printk(" KEY_DOWN:TAB(0x%02x)", kc);
			} else if (ch == 0x1B) {
				printk(" KEY_DOWN:ESC(0x%02x)", kc);
			} else {
				printk(" KEY_DOWN:0x%02x", kc);
			}
		}
	}

	/* Detect key releases */
	for (int i = 0; i < 6; i++) {
		uint8_t kc = prev_keys[i];
		if (kc == 0x00 || kc == 0x01) {
			continue;
		}

		bool still_pressed = false;
		for (int j = 0; j < 6; j++) {
			if (keys[j] == kc) {
				still_pressed = true;
				break;
			}
		}

		if (!still_pressed) {
			printk(" KEY_UP:0x%02x", kc);
		}
	}

	printk("\n");

	/* Save state */
	prev_modifiers = modifiers;
	memcpy(prev_keys, keys, 6);
}

/* ── GATT notification callback ──────────────────────── */

static uint8_t notify_func(struct bt_conn *conn,
			    struct bt_gatt_subscribe_params *params,
			    const void *data, uint16_t length)
{
	if (!data) {
		printk("[HID] Unsubscribed\n");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	parse_boot_keyboard_report(data, length);
	return BT_GATT_ITER_CONTINUE;
}

/* ── GATT discovery state machine ────────────────────── */

/*
 * Discovery flow:
 * 1. Discover HID Service (0x1812)
 * 2. Discover Boot Keyboard Input Report characteristic (0x2A22)
 * 3. Discover CCC descriptor
 * 4. Subscribe to notifications
 */
static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		/* Boot KB not found → fall back to Report characteristic */
		if (disc_phase == DISC_BOOT_KB_CHAR) {
			printk("[HID] Boot KB (0x2A22) not found, "
			       "trying Report (0x2A4D)...\n");
			disc_phase = DISC_REPORT_CHAR;

			memcpy(&discover_uuid, BT_UUID_HIDS_REPORT,
			       sizeof(discover_uuid));
			discover_params.uuid = &discover_uuid.uuid;
			discover_params.func = discover_func;
			discover_params.start_handle = hid_svc_start_handle;
			discover_params.end_handle = hid_svc_end_handle;
			discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

			err = bt_gatt_discover(conn, &discover_params);
			if (err) {
				printk("[HID] Discover Report failed (err %d)\n",
				       err);
			}
			return BT_GATT_ITER_STOP;
		}

		printk("[HID] Discover complete (no more attributes)\n");
		(void)memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	printk("[HID] Discover: handle %u, phase %d\n",
	       attr->handle, disc_phase);

	/* Step 1: Found HID Service → discover Boot KB Input Report */
	if (disc_phase == DISC_SVC) {
		struct bt_gatt_service_val *svc = attr->user_data;

		hid_svc_start_handle = attr->handle + 1;
		hid_svc_end_handle = svc->end_handle;
		printk("[HID] Found HID Service (handles %u-%u)\n",
		       attr->handle, hid_svc_end_handle);

		disc_phase = DISC_BOOT_KB_CHAR;
		memcpy(&discover_uuid, BT_UUID_HIDS_BOOT_KB_IN_REPORT,
		       sizeof(discover_uuid));
		discover_params.uuid = &discover_uuid.uuid;
		discover_params.start_handle = hid_svc_start_handle;
		discover_params.end_handle = hid_svc_end_handle;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			printk("[HID] Discover Boot KB failed (err %d)\n", err);
		}
	}
	/* Step 2: Found characteristic (Boot KB or Report) → discover CCC */
	else if (disc_phase == DISC_BOOT_KB_CHAR ||
		 disc_phase == DISC_REPORT_CHAR) {
		printk("[HID] Found %s at handle %u\n",
		       disc_phase == DISC_BOOT_KB_CHAR ?
		       "Boot KB Input Report" : "HID Report", attr->handle);

		disc_phase = DISC_CCC;
		subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);

		memcpy(&discover_uuid, BT_UUID_GATT_CCC, sizeof(discover_uuid));
		discover_params.uuid = &discover_uuid.uuid;
		discover_params.start_handle = attr->handle + 2;
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			printk("[HID] Discover CCC failed (err %d)\n", err);
		}
	}
	/* Step 3: Found CCC → subscribe */
	else if (disc_phase == DISC_CCC) {
		printk("[HID] Found CCC at handle %u, subscribing...\n",
		       attr->handle);

		subscribe_params.notify = notify_func;
		subscribe_params.value = BT_GATT_CCC_NOTIFY;
		subscribe_params.ccc_handle = attr->handle;

		err = bt_gatt_subscribe(conn, &subscribe_params);
		if (err && err != -EALREADY) {
			printk("[HID] Subscribe failed (err %d)\n", err);
		} else {
			printk("[HID] *** SUBSCRIBED to HID Input Report ***\n");
		}
	}

	return BT_GATT_ITER_STOP;
}

/* ── BLE Scanning ────────────────────────────────────── */

/* BLE GAP Appearance codes */
#define BT_APPEARANCE_KEYBOARD      0x03C1
#define BT_APPEARANCE_MOUSE         0x03C2
#define BT_APPEARANCE_GAMEPAD       0x03C4

/* Scan result context for collecting AD data across callbacks */
struct scan_result {
	bt_addr_le_t addr;
	bool has_hid_uuid;
	bool has_hid_appearance;
	char name[32];
	uint16_t appearance;
};

static struct scan_result current_scan;

static void try_connect(const bt_addr_le_t *addr, const char *reason)
{
	int err;

	printk("[SCAN] %s Connecting...\n", reason);

	err = bt_le_scan_stop();
	if (err) {
		printk("[SCAN] Stop scan failed (err %d)\n", err);
		return;
	}

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT,
				&default_conn);
	if (err) {
		printk("[SCAN] Create connection failed (err %d)\n", err);
		start_scan();
	}
}

static bool eir_found(struct bt_data *data, void *user_data)
{
	int i;

	switch (data->type) {
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED: {
		size_t len = MIN(data->data_len, sizeof(current_scan.name) - 1);
		memcpy(current_scan.name, data->data, len);
		current_scan.name[len] = '\0';
		break;
	}

	case BT_DATA_GAP_APPEARANCE:
		if (data->data_len == 2) {
			memcpy(&current_scan.appearance, data->data, 2);
			current_scan.appearance = sys_le16_to_cpu(current_scan.appearance);
			if (current_scan.appearance == BT_APPEARANCE_KEYBOARD ||
			    current_scan.appearance == BT_APPEARANCE_MOUSE ||
			    current_scan.appearance == BT_APPEARANCE_GAMEPAD) {
				current_scan.has_hid_appearance = true;
			}
		}
		break;

	case BT_DATA_UUID16_SOME:
	case BT_DATA_UUID16_ALL:
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

	return true; /* Continue parsing all AD fields */
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			  struct net_buf_simple *ad)
{
	/* Only interested in connectable events */
	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	/* Reset scan result */
	memset(&current_scan, 0, sizeof(current_scan));
	bt_addr_le_copy(&current_scan.addr, addr);

	/* Parse all AD fields */
	bt_data_parse(ad, eir_found, NULL);

	/* Print device info */
	char dev[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, dev, sizeof(dev));

	if (current_scan.name[0]) {
		printk("[SCAN] %s '%s' RSSI %d", dev, current_scan.name, rssi);
	} else {
		printk("[SCAN] %s RSSI %d", dev, rssi);
	}

	if (current_scan.has_hid_uuid) {
		printk(" [HID]");
	}
	if (current_scan.has_hid_appearance) {
		printk(" [Appearance:0x%04x]", current_scan.appearance);
	}
	printk("\n");

	/* Try to connect if it looks like a HID device */
	if (current_scan.has_hid_uuid) {
		try_connect(addr, "Found HID UUID!");
	} else if (current_scan.has_hid_appearance) {
		try_connect(addr, "Found HID Appearance!");
	}
}

static void start_scan(void)
{
	int err;

	struct bt_le_scan_param scan_param = {
		.type     = BT_LE_SCAN_TYPE_ACTIVE,
		.options  = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
	};

	err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("[BLE] Scan start failed (err %d)\n", err);
		return;
	}

	printk("[BLE] Scanning for HID devices (UUID 0x1812)...\n");
}

/* ── Connection callbacks ────────────────────────────── */

static void start_hid_discovery(struct bt_conn *conn)
{
	int err;

	disc_phase = DISC_SVC;
	memcpy(&discover_uuid, BT_UUID_HIDS, sizeof(discover_uuid));
	discover_params.uuid = &discover_uuid.uuid;
	discover_params.func = discover_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(conn, &discover_params);
	if (err) {
		printk("[HID] Discover failed (err %d)\n", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		printk("[BLE] Failed to connect to %s (err %u)\n", addr, conn_err);

		bt_conn_unref(default_conn);
		default_conn = NULL;
		start_scan();
		return;
	}

	printk("[BLE] Connected: %s (attempt %d/%d)\n", addr,
	       connect_retry_count + 1, MAX_CONNECT_RETRIES);

	/* Request security (pairing) — HID keyboards require encryption.
	 * GATT discovery is deferred to security_changed() callback. */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		printk("[BLE] Set security failed (err %d), trying discovery anyway\n", err);
		if (conn == default_conn) {
			start_hid_discovery(conn);
		}
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Disconnected: %s, reason 0x%02x\n", addr, reason);

	if (default_conn != conn) {
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;

	/* Clear key state */
	memset(prev_keys, 0, sizeof(prev_keys));
	prev_modifiers = 0;

	connect_retry_count++;
	if (connect_retry_count >= MAX_CONNECT_RETRIES) {
		printk("[BLE] Max retries (%d) reached. Waiting 10s before retry...\n",
		       MAX_CONNECT_RETRIES);
		connect_retry_count = 0;
		k_sleep(K_SECONDS(10));
	}

	/* Restart scanning */
	start_scan();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("[BLE] Security changed: %s level %u\n", addr, level);
		/* GATT discovery deferred to pairing_complete() to avoid
		 * conflict with SMP key distribution on weak BLE stacks */
	} else {
		printk("[BLE] Security failed: %s level %u err %d\n",
		       addr, level, err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
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
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Pairing complete: %s, bonded: %s\n", addr,
	       bonded ? "yes" : "no");

	/* Pairing & key exchange done — now safe to discover HID */
	if (conn == default_conn) {
		start_hid_discovery(conn);
	}
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("[BLE] Pairing failed: %s, reason %d\n", addr, reason);

	/* Clear stale bonds — keyboard changes address frequently */
	bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
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

	printk("=== BLE HID Host (Phase 1) ===\n");
	printk("Scanning for Bluetooth keyboards...\n\n");

	err = bt_enable(NULL);
	if (err) {
		printk("[BLE] Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("[BLE] Bluetooth initialized\n");

	/* Clear stale bonds — keyboard rotates addresses */
	bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

	err = bt_conn_auth_cb_register(&auth_cb);
	if (err) {
		printk("[BLE] Auth callback register failed (err %d)\n", err);
	}

	err = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (err) {
		printk("[BLE] Auth info callback register failed (err %d)\n", err);
	}

	start_scan();
	return 0;
}
