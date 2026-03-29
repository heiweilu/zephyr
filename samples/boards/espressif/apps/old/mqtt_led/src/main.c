/*
 * Copyright (c) 2025 Hei Weilu
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED strip (from overlay alias led-strip) */
#define STRIP_NODE DT_ALIAS(led_strip)
#if DT_NODE_HAS_PROP(STRIP_NODE, chain_length)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)
#else
#define STRIP_NUM_PIXELS 1
#endif

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_NUM_PIXELS];

/* Network events */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED | NET_EVENT_IPV4_ADDR_ADD)
static struct net_mgmt_event_callback l4_cb;
static K_SEM_DEFINE(net_ok, 0, 1);

/* MQTT */
static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buffer[1024];
static uint8_t tx_buffer[1024];
static uint8_t payload_buf[256];
static bool mqtt_connected_flag;

#ifndef CONFIG_APP_MQTT_BROKER_HOST
#define CONFIG_APP_MQTT_BROKER_HOST "broker.emqx.io"
#endif
#ifndef CONFIG_APP_MQTT_BROKER_PORT
#define CONFIG_APP_MQTT_BROKER_PORT 1883
#endif

#define MQTT_SUB_TOPIC "esp32s3/led/cmd"
#define MQTT_PUB_TOPIC "esp32s3/led/status"

/* Wi-Fi auto connection configuration */
#define WIFI_SSID "Ultra"
#define WIFI_PSK  "12345678"

/* Helper: set all LEDs */
static void set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
	for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels[i].r = r;
		pixels[i].g = g;
		pixels[i].b = b;
	}
	if (led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS) < 0) {
		LOG_ERR("LED update failed");
	}
}

/* Network event handler */
static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 acquired");
		k_sem_give(&net_ok);
	} else if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("L4 connected");
		k_sem_give(&net_ok);
	} else if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		LOG_INF("L4 disconnected");
	}
}

/* MQTT event handler */
static void mqtt_event_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	int rc;

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed %d", evt->result);
			break;
		}
		mqtt_connected_flag = true;
		LOG_INF("MQTT connected");
		/* Subscribe */
		{
			struct mqtt_topic topic = {.topic = {.utf8 = (uint8_t *)MQTT_SUB_TOPIC,
							     .size = strlen(MQTT_SUB_TOPIC)},
						   .qos = MQTT_QOS_0_AT_MOST_ONCE};
			const struct mqtt_subscription_list sub_list = {
				.list = &topic, .list_count = 1, .message_id = 1};
			rc = mqtt_subscribe(c, &sub_list);
			if (rc) {
				LOG_ERR("Subscribe failed %d", rc);
			} else {
				LOG_INF("Subscribe sent for topic: %s", MQTT_SUB_TOPIC);
			}
		}
		break;
	case MQTT_EVT_SUBACK:
		LOG_INF("SUBACK received, message_id=%u", evt->param.suback.message_id);
		break;
	case MQTT_EVT_DISCONNECT:
		mqtt_connected_flag = false;
		LOG_INF("MQTT disconnected");
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;
		uint32_t len = MIN(p->message.payload.len, sizeof(payload_buf) - 1);

		memset(payload_buf, 0, sizeof(payload_buf));
		mqtt_read_publish_payload(c, payload_buf, len);
		payload_buf[len] = '\0';
		LOG_INF("RX %.*s => %s", p->message.topic.topic.size, p->message.topic.topic.utf8,
			payload_buf);
		/* Commands: red green blue off OR R,G,B */
		if (!strcmp((char *)payload_buf, "red")) {
			set_led_color(0x40, 0, 0);
		} else if (!strcmp((char *)payload_buf, "green")) {
			set_led_color(0, 0x40, 0);
		} else if (!strcmp((char *)payload_buf, "blue")) {
			set_led_color(0, 0, 0x40);
		} else if (!strcmp((char *)payload_buf, "off")) {
			set_led_color(0, 0, 0);
		} else {
			unsigned int r, g, b;

			if (sscanf((char *)payload_buf, "%u,%u,%u", &r, &g, &b) == 3) {
				set_led_color(r & 0xFF, g & 0xFF, b & 0xFF);
			} else {
				LOG_WRN("Unknown command");
			}
		}
		break;
	}
	default:
		break;
	}
}

/* Resolve broker: try IPv4 literal first; if not, attempt DNS (AF_INET). */
static int resolve_broker(void)
{
	struct sockaddr_in *addr4 = (struct sockaddr_in *)&broker;

	memset(addr4, 0, sizeof(*addr4));
	addr4->sin_family = AF_INET;
	addr4->sin_port = htons(CONFIG_APP_MQTT_BROKER_PORT);

	/* 1) Try treat as IPv4 literal */
	if (inet_pton(AF_INET, CONFIG_APP_MQTT_BROKER_HOST, &addr4->sin_addr) == 1) {
		LOG_INF("Broker IPv4 literal %s:%d", CONFIG_APP_MQTT_BROKER_HOST,
			CONFIG_APP_MQTT_BROKER_PORT);
		return 0;
	}

#if defined(CONFIG_DNS_RESOLVER)
	/* 2) DNS lookup */
	struct addrinfo hints = {0};
	struct addrinfo *res = NULL;
	int err;
	char port_str[8];

	snprintk(port_str, sizeof(port_str), "%d", CONFIG_APP_MQTT_BROKER_PORT);
	hints.ai_family = AF_INET; /* Only IPv4 for now */
	hints.ai_socktype = SOCK_STREAM;
	err = getaddrinfo(CONFIG_APP_MQTT_BROKER_HOST, port_str, &hints, &res);

	if (err || !res) {
		LOG_ERR("DNS resolve failed (%d) for %s", err, CONFIG_APP_MQTT_BROKER_HOST);
		if (res) {
			freeaddrinfo(res);
		}
		return -EHOSTUNREACH;
	}
	struct sockaddr_in *resolved = (struct sockaddr_in *)res->ai_addr;

	addr4->sin_addr = resolved->sin_addr;
	freeaddrinfo(res);
	char ipbuf[NET_IPV4_ADDR_LEN];

	net_addr_ntop(AF_INET, &addr4->sin_addr, ipbuf, sizeof(ipbuf));
	LOG_INF("Broker DNS %s -> %s:%d", CONFIG_APP_MQTT_BROKER_HOST, ipbuf,
		CONFIG_APP_MQTT_BROKER_PORT);
	return 0;
#else
	LOG_ERR("CONFIG_APP_MQTT_BROKER_HOST must be IPv4 literal or enable CONFIG_DNS_RESOLVER");
	return -EINVAL;
#endif
}

/* MQTT client setup */
static void app_mqtt_client_prepare(void)
{
	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_event_handler;
	client.client_id.utf8 = (uint8_t *)"esp32s3-led";
	client.client_id.size = strlen((char *)client.client_id.utf8);
	client.user_name = NULL;
	client.password = NULL;
	client.protocol_version = MQTT_VERSION_3_1_0;
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.keepalive = CONFIG_MQTT_KEEPALIVE;
}

/* MQTT client connect */
static int mqtt_connect_blocking(void)
{
	int rc = mqtt_connect(&client);

	if (rc) {
		LOG_ERR("mqtt_connect rc=%d", rc);
		return rc;
	}
	int attempts = 100; /* 100 * 50ms ~= 5s */

	while (!mqtt_connected_flag && attempts--) {
		mqtt_input(&client);
		mqtt_live(&client);
		k_msleep(50);
	}
	return mqtt_connected_flag ? 0 : -ETIMEDOUT;
}

static int mqtt_publish_status(void)
{
	char msg[64];

	snprintk(msg, sizeof(msg), "{\"r\":%u,\"g\":%u,\"b\":%u}", pixels[0].r, pixels[0].g,
		 pixels[0].b);
	struct mqtt_publish_param param = {0};

	param.message.topic.topic.utf8 = (uint8_t *)MQTT_PUB_TOPIC;
	param.message.topic.topic.size = strlen(MQTT_PUB_TOPIC);
	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.payload.data = msg;
	param.message.payload.len = strlen(msg);
	param.message_id = sys_rand16_get();
	param.dup_flag = 0;
	param.retain_flag = 0;
	return mqtt_publish(&client, &param);
}

/* Register callbacks for L4 events */
static void l4_register(void)
{
	net_mgmt_init_event_callback(&l4_cb, wifi_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);
}

/* Auto connect to Wi-Fi */
static int wifi_connect_auto(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params wifi_params = {0};

	wifi_params.ssid = (uint8_t *)WIFI_SSID;
	wifi_params.ssid_length = strlen(WIFI_SSID);
	wifi_params.security = WIFI_SECURITY_TYPE_PSK; /* WPA/WPA2-PSK: need PSK */
	wifi_params.psk = (uint8_t *)WIFI_PSK;
	wifi_params.psk_length = strlen(WIFI_PSK);
	wifi_params.channel = WIFI_CHANNEL_ANY;    /* auto select channel */
	wifi_params.band = WIFI_FREQ_BAND_2_4_GHZ; /* 2.4 GHz band */
	wifi_params.mfp = WIFI_MFP_OPTIONAL;       /* Management Frame Protection */

	LOG_INF("Connecting to Wi-Fi SSID: %s", WIFI_SSID);

	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &wifi_params, sizeof(wifi_params));
}

int main(void)
{
	int rc;
	LOG_INF("MQTT LED sample start");

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip not ready");
		return 0;
	}
	set_led_color(0, 0, 0);

	/* Register callbacks for L4 events */
	l4_register();

	/* get default network interface */
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No default net_if");
		return 0;
	}
#if defined(CONFIG_NET_DHCPV4)
	net_dhcpv4_start(iface);
#endif

	/* Auto connect to Wi-Fi */
	int wifi_rc = wifi_connect_auto();

	if (wifi_rc) {
		LOG_ERR("Wi-Fi connect failed: %d", wifi_rc);
		return 0;
	}

	/* Hybrid wait: event semaphore OR polling IPv4 acquisition (timeout 60s) */
	int waited_ms = 0;

	while (true) {
		if (k_sem_take(&net_ok, K_NO_WAIT) == 0) {
			LOG_INF("Network ready (event)");
			break;
		}

		/* Get IPv4 address */
		const struct in_addr *a = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		if (!a) {
			a = net_if_ipv4_get_global_addr(iface, NET_ADDR_ANY);
		}
		if (a) {
			char ipbuf[NET_IPV4_ADDR_LEN];

			net_addr_ntop(AF_INET, a, ipbuf, sizeof(ipbuf));

			LOG_INF("Network ready (poll) IPv4=%s", ipbuf);
			break;
		}
		if (waited_ms >= 60000) {
			LOG_WRN("Timeout waiting network, continue anyway");
			break;
		}
		LOG_INF("Waiting for network...");
		k_msleep(1000);
		waited_ms += 1000;
	}

	if (resolve_broker()) {
		return 0;
	}

	app_mqtt_client_prepare();

retry_connect:
	LOG_INF("Connecting MQTT broker %s:%d", CONFIG_APP_MQTT_BROKER_HOST,
		CONFIG_APP_MQTT_BROKER_PORT);
	rc = mqtt_connect_blocking();
	if (rc) {
		LOG_ERR("MQTT connect failed rc=%d, retry in 3s", rc);
		k_msleep(3000);
		goto retry_connect;
	}

	uint32_t publish_counter = 0;

	for (;;) {
		int irc = mqtt_input(&client);

		if (irc && irc != -EAGAIN) {
			LOG_WRN("mqtt_input rc=%d", irc);
		}
		mqtt_live(&client);

		if (!mqtt_connected_flag) {
			LOG_WRN("Lost MQTT, reconnecting");
			mqtt_abort(&client);
			k_msleep(1000);
			goto retry_connect;
		}

		if ((publish_counter++ % 100) == 0) {
			rc = mqtt_publish_status();
			if (rc) {
				LOG_WRN("Status publish failed %d", rc);
			} else {
				LOG_DBG("Status published");
			}
		}
		k_msleep(50);
	}

	return 0;
}
