/*
 * Cloud AI Chat — Phase 10: 云端 AI 对话
 *
 * WiFi → DNS → TLS → HTTP POST to Alibaba Bailian (qwen3.5-omni-flash)
 * Serial text interaction.
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
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/dns_resolve.h>

#include "secrets.h"
#include "ca_certificate.h"

LOG_MODULE_REGISTER(cloud_ai, LOG_LEVEL_INF);

/* ── API config ── */
#define AI_HOST        "dashscope.aliyuncs.com"
#define AI_PORT        443
#define AI_URL         "/compatible-mode/v1/chat/completions"
#define AI_MODEL       "qwen3.5-omni-flash"

/* ── Buffers ── */
#define HTTP_RECV_BUF_SIZE  4096
#define HTTP_TIMEOUT_MS     30000
#define JSON_REQ_BUF_SIZE   1024
#define JSON_RSP_BUF_SIZE   4096

static uint8_t http_recv_buf[HTTP_RECV_BUF_SIZE];
static char    json_req_buf[JSON_REQ_BUF_SIZE];
static char    json_rsp_buf[JSON_RSP_BUF_SIZE];
static size_t  json_rsp_len;

/* ── WiFi state ── */
static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static K_SEM_DEFINE(wifi_connected_sem, 0, 1);
static K_SEM_DEFINE(ipv4_ready_sem, 0, 1);
static K_SEM_DEFINE(scan_done_sem, 0, 1);

/* ================================================================
 *  WiFi helpers (simplified from wifi_test)
 * ================================================================ */

static void handle_wifi_event(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event,
			      struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
		LOG_INF("WiFi scan done");
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
			      uint64_t mgmt_event,
			      struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 address acquired");
		k_sem_give(&ipv4_ready_sem);
	}
}

static int wifi_connect(void)
{
	struct wifi_connect_req_params params = {0};

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

	/* Scan first to ensure WiFi subsystem is ready */
	int ret;
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

	LOG_INF("Waiting for IPv4 ...");
	if (k_sem_take(&ipv4_ready_sem, K_SECONDS(30))) {
		LOG_ERR("DHCPv4 timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

/* ================================================================
 *  TLS socket helpers
 * ================================================================ */

static int tls_setup(void)
{
	int ret;

	ret = tls_credential_add(CA_CERTIFICATE_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate,
				 sizeof(ca_certificate));
	if (ret < 0) {
		LOG_ERR("Failed to register CA certificate: %d", ret);
		return ret;
	}

	LOG_INF("CA certificate registered (tag %d, %zu bytes)",
		CA_CERTIFICATE_TAG, sizeof(ca_certificate));
	return 0;
}

static int tls_connect(int *sock)
{
	struct zsock_addrinfo hints = {0};
	struct zsock_addrinfo *res = NULL;
	int ret;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	LOG_INF("Resolving %s ...", AI_HOST);

	ret = zsock_getaddrinfo(AI_HOST, "443", &hints, &res);
	if (ret || !res) {
		LOG_ERR("DNS resolve failed: %d", ret);
		return -EHOSTUNREACH;
	}

	char addr_str[INET_ADDRSTRLEN];
	struct sockaddr_in *addr4 = (struct sockaddr_in *)res->ai_addr;
	zsock_inet_ntop(AF_INET, &addr4->sin_addr, addr_str, sizeof(addr_str));
	LOG_INF("Resolved to %s", addr_str);

	/* Create TLS socket */
	*sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (*sock < 0) {
		LOG_ERR("Failed to create TLS socket: %d", errno);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	/* Set TLS options */
	sec_tag_t sec_tag_list[] = { CA_CERTIFICATE_TAG };

	ret = zsock_setsockopt(*sock, SOL_TLS, TLS_SEC_TAG_LIST,
			       sec_tag_list, sizeof(sec_tag_list));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_SEC_TAG_LIST: %d", errno);
		zsock_close(*sock);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	ret = zsock_setsockopt(*sock, SOL_TLS, TLS_HOSTNAME,
			       TLS_PEER_HOSTNAME, sizeof(TLS_PEER_HOSTNAME));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_HOSTNAME: %d", errno);
		zsock_close(*sock);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	LOG_INF("TLS connecting to %s:%d ...", AI_HOST, AI_PORT);

	ret = zsock_connect(*sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);

	if (ret < 0) {
		LOG_ERR("TLS connect failed: %d", errno);
		zsock_close(*sock);
		return -errno;
	}

	LOG_INF("TLS connected!");
	return 0;
}

/* ================================================================
 *  HTTP + JSON helpers
 * ================================================================ */

static int build_json_request(const char *user_msg)
{
	int len = snprintk(json_req_buf, sizeof(json_req_buf),
		"{\"model\":\"%s\","
		"\"messages\":["
		"{\"role\":\"system\",\"content\":\"You are a helpful assistant. Reply in Chinese. Keep answers concise.\"},"
		"{\"role\":\"user\",\"content\":\"%s\"}"
		"],"
		"\"enable_thinking\":false"
		"}",
		AI_MODEL, user_msg);

	if (len < 0 || len >= (int)sizeof(json_req_buf)) {
		LOG_ERR("JSON request too large");
		return -ENOMEM;
	}

	return len;
}

/* Simple string search to extract "content":"..." from JSON response */
static const char *extract_content(const char *json, size_t len)
{
	/* Find the last "content":" in the response (the assistant's reply) */
	const char *needle = "\"content\":\"";
	const char *found = NULL;
	const char *p = json;

	while (p < json + len) {
		const char *hit = strstr(p, needle);
		if (!hit) {
			break;
		}
		found = hit;
		p = hit + 1;
	}

	if (!found) {
		/* Try alternative: "content": " (with space) */
		needle = "\"content\": \"";
		p = json;
		while (p < json + len) {
			const char *hit = strstr(p, needle);
			if (!hit) {
				break;
			}
			found = hit;
			p = hit + 1;
		}
	}

	if (!found) {
		return NULL;
	}

	return found + strlen(needle);
}

static void print_ai_response(const char *json, size_t len)
{
	const char *content = extract_content(json, len);

	if (!content) {
		LOG_ERR("Failed to extract content from response");
		LOG_INF("Raw response (%zu bytes): %.*s", len,
			(int)(len > 500 ? 500 : len), json);
		return;
	}

	/* Print until closing quote (handle escaped quotes) */
	printk("\n=== AI Response ===\n");
	for (const char *p = content; *p && p < json + len; p++) {
		if (*p == '\\' && *(p + 1)) {
			p++;
			switch (*p) {
			case 'n':
				printk("\n");
				break;
			case 't':
				printk("\t");
				break;
			case '"':
				printk("\"");
				break;
			case '\\':
				printk("\\");
				break;
			default:
				printk("\\%c", *p);
				break;
			}
		} else if (*p == '"') {
			break;
		} else {
			printk("%c", *p);
		}
	}
	printk("\n===================\n");
}

static int http_response_cb(struct http_response *rsp,
			    enum http_final_call final_data,
			    void *user_data)
{
	if (rsp->data_len > 0) {
		size_t copy_len = rsp->data_len;

		if (json_rsp_len + copy_len >= JSON_RSP_BUF_SIZE) {
			copy_len = JSON_RSP_BUF_SIZE - json_rsp_len - 1;
		}

		if (copy_len > 0) {
			memcpy(json_rsp_buf + json_rsp_len, rsp->recv_buf, copy_len);
			json_rsp_len += copy_len;
			json_rsp_buf[json_rsp_len] = '\0';
		}
	}

	if (final_data == HTTP_DATA_FINAL) {
		LOG_INF("HTTP response complete (%zu bytes)", json_rsp_len);
	}

	return 0;
}

static int send_ai_request(int sock, const char *question)
{
	struct http_request req = {0};
	int json_len;
	char content_len_str[16];
	int ret;

	json_len = build_json_request(question);
	if (json_len < 0) {
		return json_len;
	}

	snprintk(content_len_str, sizeof(content_len_str), "%d", json_len);

	/* Build authorization header */
	static char auth_header[128];
	snprintk(auth_header, sizeof(auth_header),
		 "Authorization: Bearer %s\r\n", AI_API_KEY);

	const char *headers[] = {
		"Content-Type: application/json\r\n",
		auth_header,
		NULL
	};

	/* Reset response buffer */
	json_rsp_len = 0;
	memset(json_rsp_buf, 0, sizeof(json_rsp_buf));

	req.method = HTTP_POST;
	req.url = AI_URL;
	req.host = AI_HOST;
	req.protocol = "HTTP/1.1";
	req.payload = json_req_buf;
	req.payload_len = json_len;
	req.header_fields = headers;
	req.response = http_response_cb;
	req.recv_buf = http_recv_buf;
	req.recv_buf_len = sizeof(http_recv_buf);

	LOG_INF("Sending question: \"%s\"", question);
	LOG_INF("Request payload (%d bytes): %s", json_len, json_req_buf);

	ret = http_client_req(sock, &req, HTTP_TIMEOUT_MS, "AI chat");
	if (ret < 0) {
		LOG_ERR("HTTP request failed: %d", ret);
		return ret;
	}

	LOG_INF("HTTP request sent, total bytes: %d", ret);

	/* Parse and print the response */
	if (json_rsp_len > 0) {
		print_ai_response(json_rsp_buf, json_rsp_len);
	} else {
		LOG_ERR("Empty response from server");
	}

	return 0;
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(void)
{
	int sock = -1;
	int ret;

	LOG_INF("=== Cloud AI Chat (qwen3.5-omni-flash) ===");

	/* Step 1: Connect WiFi */
	ret = wifi_connect();
	if (ret) {
		LOG_ERR("WiFi connection failed");
		return ret;
	}

	/* Step 2: Register TLS CA certificate */
	ret = tls_setup();
	if (ret) {
		return ret;
	}

	/* Step 3: TLS connect to API server */
	ret = tls_connect(&sock);
	if (ret) {
		return ret;
	}

	/* Step 4: Send a test question */
	ret = send_ai_request(sock, "你好，请用一句话介绍你自己。");
	if (ret) {
		LOG_ERR("AI request failed: %d", ret);
	}

	/* Close socket */
	if (sock >= 0) {
		zsock_close(sock);
		LOG_INF("Socket closed");
	}

	LOG_INF("Cloud AI test complete. Entering idle.");

	while (1) {
		k_sleep(K_SECONDS(60));
	}

	return 0;
}
