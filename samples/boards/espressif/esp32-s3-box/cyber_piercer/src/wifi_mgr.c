/*
 * smart_camera Wi-Fi manager.
 * Adapted from samples/boards/espressif/apps/new/cloud_ai/src/main.c.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi_mgr.h"
#include "secrets.h"

LOG_MODULE_REGISTER(wifi_mgr, LOG_LEVEL_INF);

static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static K_SEM_DEFINE(wifi_connected_sem, 0, 1);
static K_SEM_DEFINE(ipv4_ready_sem, 0, 1);
static K_SEM_DEFINE(scan_done_sem, 0, 1);

static void handle_wifi_event(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
		k_sem_give(&scan_done_sem);
	} else if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		if (cb->info && cb->info_length == sizeof(struct wifi_status)) {
			const struct wifi_status *status = cb->info;
			if (status->status == WIFI_STATUS_CONN_SUCCESS) {
				LOG_INF("WiFi connected to %s", WIFI_SSID);
				k_sem_give(&wifi_connected_sem);
			} else {
				LOG_ERR("WiFi connect failed: %d", status->status);
			}
		}
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_WRN("WiFi disconnected");
	}
}

static void handle_ipv4_event(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		k_sem_give(&ipv4_ready_sem);
	}
}

static int format_iface_ipv4(struct net_if *iface, char *out, size_t out_size)
{
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *if_addr = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (!if_addr->is_used ||
		    if_addr->addr_type != NET_ADDR_DHCP) {
			continue;
		}
		if (!net_addr_ntop(AF_INET, &if_addr->address.in_addr,
				   out, out_size)) {
			return -EINVAL;
		}
		return 0;
	}
	return -ENOENT;
}

int wifi_mgr_connect_blocking(char *ip_str_out, size_t ip_str_size)
{
	struct wifi_connect_req_params params = {0};
	int ret;

	sta_iface = net_if_get_default();
	if (!sta_iface) {
		LOG_ERR("No default network interface");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&wifi_cb, handle_wifi_event,
				     NET_EVENT_WIFI_SCAN_DONE |
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, handle_ipv4_event,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	params.ssid = (const uint8_t *)WIFI_SSID;
	params.ssid_length = strlen(WIFI_SSID);
	params.psk = (const uint8_t *)WIFI_PSK;
	params.psk_length = strlen(WIFI_PSK);
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.channel = WIFI_CHANNEL_ANY;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = SYS_FOREVER_MS;

	LOG_INF("Scanning WiFi ...");
	for (int attempt = 1; attempt <= 10; attempt++) {
		ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
		if (ret == 0) {
			break;
		}
		k_msleep(500);
	}
	if (ret) {
		LOG_ERR("WiFi scan request failed: %d", ret);
		return ret;
	}
	if (k_sem_take(&scan_done_sem, K_SECONDS(15))) {
		LOG_ERR("WiFi scan timeout");
		return -ETIMEDOUT;
	}

	LOG_INF("Connecting to WiFi: %s ...", WIFI_SSID);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params,
		       sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("WiFi connect request failed: %d", ret);
		return ret;
	}

	if (k_sem_take(&wifi_connected_sem, K_SECONDS(30))) {
		LOG_ERR("WiFi connect timeout");
		return -ETIMEDOUT;
	}

	LOG_INF("Waiting for IPv4 lease ...");
	if (k_sem_take(&ipv4_ready_sem, K_SECONDS(30))) {
		LOG_ERR("DHCPv4 timeout");
		return -ETIMEDOUT;
	}

	if (ip_str_out && ip_str_size > 0) {
		if (format_iface_ipv4(sta_iface, ip_str_out, ip_str_size) < 0) {
			snprintk(ip_str_out, ip_str_size, "?.?.?.?");
		}
	}

	return 0;
}
