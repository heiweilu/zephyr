/*
 * WiFi Test — Phase 5: Module 10
 * Scan AP list, connect to a target SSID, acquire IPv4 via DHCP, reconnect on drop.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(wifi_test, LOG_LEVEL_INF);

#define APP_WIFI_SSID "xxx"
#define APP_WIFI_PSK  "xxx"

#define IPV4_EVENT_MASK NET_EVENT_IPV4_ADDR_ADD

#define CONNECT_RETRY_MAX 5

static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct k_work_delayable reconnect_work;

static K_SEM_DEFINE(scan_done_sem, 0, 1);
static K_SEM_DEFINE(connect_sem, 0, 1);
static K_SEM_DEFINE(ipv4_sem, 0, 1);

static int scan_count;
static int connect_status = -1;
static bool ipv4_ready;
static bool manual_disconnect;

static const char *security_txt(enum wifi_security_type security)
{
	switch (security) {
	case WIFI_SECURITY_TYPE_NONE:
		return "OPEN";
	case WIFI_SECURITY_TYPE_PSK:
		return "WPA2-PSK";
	case WIFI_SECURITY_TYPE_SAE:
		return "WPA3-SAE";
	case WIFI_SECURITY_TYPE_WPA_PSK:
		return "WPA-PSK";
	case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
		return "WPA-AUTO";
	default:
		return "OTHER";
	}
}

static void print_ipv4_info(struct net_if *iface)
{
	for (int index = 0; index < NET_IF_MAX_IPV4_ADDR; index++) {
		char buf[NET_IPV4_ADDR_LEN];

		if (iface->config.ip.ipv4 == NULL) {
			return;
		}

		if (iface->config.ip.ipv4->unicast[index].ipv4.addr_type != NET_ADDR_DHCP) {
			continue;
		}

		LOG_INF("IPv4 address: %s",
			net_addr_ntop(AF_INET,
				      &iface->config.ip.ipv4->unicast[index].ipv4.address.in_addr,
				      buf, sizeof(buf)));
		LOG_INF("Subnet mask : %s",
			net_addr_ntop(AF_INET,
				      &iface->config.ip.ipv4->unicast[index].netmask,
				      buf, sizeof(buf)));
		LOG_INF("Gateway     : %s",
			net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw, buf, sizeof(buf)));
		LOG_INF("Lease time  : %u sec", iface->config.dhcpv4.lease_time);
	}
}

static void handle_scan_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_scan_result *entry = cb->info;
	char ssid[WIFI_SSID_MAX_LEN + 1];

	if (cb->info == NULL || cb->info_length != sizeof(struct wifi_scan_result)) {
		return;
	}

	memcpy(ssid, entry->ssid, entry->ssid_length);
	ssid[entry->ssid_length] = '\0';

	if (scan_count == 0) {
		printk("\n%-4s | %-32s | %-4s | %-5s | %-10s\n",
		       "No.", "SSID", "CHAN", "RSSI", "SEC");
	}

	scan_count++;
	printk("%-4d | %-32s | %-4u | %-5d | %-10s\n",
	       scan_count, ssid, entry->channel, entry->rssi, security_txt(entry->security));
}

static void handle_wifi_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			      struct net_if *iface)
{
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		handle_scan_result(cb);
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		LOG_INF("WiFi scan done, %d AP found", scan_count);
		k_sem_give(&scan_done_sem);
		break;
	case NET_EVENT_WIFI_CONNECT_RESULT:
		if (cb->info && cb->info_length == sizeof(struct wifi_status)) {
			const struct wifi_status *status = cb->info;

			connect_status = status->status;
			if (connect_status == WIFI_STATUS_CONN_SUCCESS) {
				LOG_INF("WiFi connected to %s", APP_WIFI_SSID);
			} else {
				LOG_ERR("WiFi connect failed, status=%d", connect_status);
			}
		}
		k_sem_give(&connect_sem);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		if (cb->info && cb->info_length == sizeof(struct wifi_status)) {
			const struct wifi_status *status = cb->info;

			LOG_WRN("WiFi disconnected, reason/status=%d", status->status);
		}

		ipv4_ready = false;
		if (!manual_disconnect) {
			k_work_reschedule(&reconnect_work, K_SECONDS(2));
		}
		break;
	default:
		break;
	}
}

static void handle_ipv4_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			      struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	LOG_INF("DHCPv4 address acquired");
	print_ipv4_info(iface);
	ipv4_ready = true;
	k_sem_give(&ipv4_sem);
}

static int request_scan(void)
{
	int ret;

	scan_count = 0;
	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
	if (ret) {
		LOG_ERR("WiFi scan request failed: %d", ret);
		return ret;
	}

	LOG_INF("WiFi scan requested");
	return 0;
}

static int request_connect(void)
{
	struct wifi_connect_req_params params = {0};

	params.ssid = (const uint8_t *)APP_WIFI_SSID;
	params.ssid_length = strlen(APP_WIFI_SSID);
	params.psk = (const uint8_t *)APP_WIFI_PSK;
	params.psk_length = strlen(APP_WIFI_PSK);
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.channel = WIFI_CHANNEL_ANY;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = SYS_FOREVER_MS;

	connect_status = -1;
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params,
			sizeof(struct wifi_connect_req_params));
}

static void reconnect_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Attempting WiFi reconnect to %s", APP_WIFI_SSID);
	if (request_connect() != 0) {
		LOG_WRN("Reconnect request failed, retry later");
		k_work_reschedule(&reconnect_work, K_SECONDS(3));
	}
}

int main(void)
{
	int ret;

	LOG_INF("=== WiFi Test ===");

	sta_iface = net_if_get_default();
	if (sta_iface == NULL) {
		LOG_ERR("No default network interface");
		return -1;
	}

	k_work_init_delayable(&reconnect_work, reconnect_work_handler);

	net_mgmt_init_event_callback(&wifi_cb, handle_wifi_event,
				     NET_EVENT_WIFI_SCAN_RESULT |
				     NET_EVENT_WIFI_SCAN_DONE |
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, handle_ipv4_event, IPV4_EVENT_MASK);
	net_mgmt_add_event_callback(&ipv4_cb);

	for (int attempt = 0; attempt < 10; attempt++) {
		ret = request_scan();
		if (ret == 0) {
			break;
		}
		k_msleep(500);
	}

	if (ret != 0) {
		LOG_ERR("WiFi scan could not start");
		return ret;
	}

	if (k_sem_take(&scan_done_sem, K_SECONDS(15)) != 0) {
		LOG_ERR("WiFi scan timeout");
		return -ETIMEDOUT;
	}

	for (int attempt = 1; attempt <= CONNECT_RETRY_MAX; attempt++) {
		ret = request_connect();
		if (ret != 0) {
			LOG_ERR("Connect request failed: %d", ret);
			k_msleep(1000);
			continue;
		}

		LOG_INF("Connect requested, attempt %d/%d", attempt, CONNECT_RETRY_MAX);

		if (k_sem_take(&connect_sem, K_SECONDS(20)) != 0) {
			LOG_ERR("WiFi connect timeout");
			continue;
		}

		if (connect_status == WIFI_STATUS_CONN_SUCCESS) {
			break;
		}

		k_msleep(1000);
	}

	if (connect_status != WIFI_STATUS_CONN_SUCCESS) {
		LOG_ERR("Unable to connect to %s", APP_WIFI_SSID);
		return -EIO;
	}

	if (k_sem_take(&ipv4_sem, K_SECONDS(30)) != 0) {
		LOG_ERR("DHCPv4 timeout");
		return -ETIMEDOUT;
	}

	LOG_INF("WiFi module test OK");

	while (1) {
		k_sleep(K_SECONDS(10));
		if (ipv4_ready) {
			LOG_INF("Alive: connected to %s", APP_WIFI_SSID);
		}
	}

	return 0;
}