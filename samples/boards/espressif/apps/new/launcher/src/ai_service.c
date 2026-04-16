/*
 * ai_service.c — AI voice assistant backend service
 *
 * Background thread: WiFi → Audio codec → TLS cert → READY.
 * Then waits for recording trigger (BOOT button or UI), records mic,
 * encodes WAV+base64, sends SSE POST to DashScope qwen3.5-omni-flash,
 * parses streaming response (text + audio), plays back via ES8311.
 *
 * All PSRAM-heavy buffers live here. UI layer (app_ai_assistant.c) polls
 * state and reads chat messages — no direct LVGL calls from this module.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <mbedtls/x509_crt.h>

#include "ai_service.h"
#include "audio.h"
#include "base64.h"
#include "secrets.h"
#include "ca_certificate.h"

LOG_MODULE_REGISTER(ai_service, LOG_LEVEL_INF);

/* ================================================================
 *  API / TLS config
 * ================================================================ */

#define AI_HOST   "dashscope.aliyuncs.com"
#define AI_PORT   443
#define AI_URL    "/compatible-mode/v1/chat/completions"
#define AI_MODEL  "qwen3.5-omni-flash"

/* Match the known-good OpenSSL probe as closely as the socket API allows. */
#define AI_TLS_CIPHERSUITE_ECDHE_RSA_AES128_GCM_SHA256 0xC02F

static const int ai_tls_ciphersuites[] = {
	AI_TLS_CIPHERSUITE_ECDHE_RSA_AES128_GCM_SHA256,
};

/* ================================================================
 *  Buffer sizes
 * ================================================================ */

#define HTTP_RECV_BUF_SIZE    4096
#define SSE_LINE_BUF_SIZE     65536   /* 64KB in PSRAM */
#define WAV_HEADER_SIZE       44

#define MAX_B64_WAV_SIZE      220000
#define MAX_RSP_AUDIO_B64     512000
#define MAX_RSP_PCM_24K       (15 * 24000)  /* 15s at 24kHz */

/* ================================================================
 *  PSRAM large buffers (.ext_ram.bss)
 * ================================================================ */

static int16_t record_pcm[MAX_RECORD_SAMPLES]
		__attribute__((section(".ext_ram.bss")));

static char b64_wav_buf[MAX_B64_WAV_SIZE]
		__attribute__((section(".ext_ram.bss")));

static char rsp_audio_b64[MAX_RSP_AUDIO_B64]
		__attribute__((section(".ext_ram.bss")));

static int16_t rsp_pcm_24k[MAX_RSP_PCM_24K]
		__attribute__((section(".ext_ram.bss")));

int16_t playback_pcm[MAX_PLAYBACK_SAMPLES]
		__attribute__((section(".ext_ram.bss")));

static char sse_line_buf[SSE_LINE_BUF_SIZE]
		__attribute__((section(".ext_ram.bss")));

/* ================================================================
 *  SRAM buffers & state
 * ================================================================ */

/* These need fast access but move to PSRAM to save DRAM */
static uint8_t http_recv_buf[HTTP_RECV_BUF_SIZE]
		__attribute__((section(".ext_ram.bss")));
static size_t  sse_line_len;

static volatile enum ai_state current_state = AI_STATE_INIT;
static volatile bool button_pressed;
static volatile bool recording_stop;
static size_t rsp_audio_b64_len;
static char   rsp_text[1024]
		__attribute__((section(".ext_ram.bss")));
static size_t rsp_text_len;

/* Chat history (in PSRAM — 20 × 512 = 10KB) */
static struct ai_chat_msg messages[AI_MAX_MESSAGES]
		__attribute__((section(".ext_ram.bss")));
static volatile int msg_count;

/* WiFi connected flag */
static volatile bool wifi_ok;

/* ================================================================
 *  BOOT button
 * ================================================================ */

static const struct gpio_dt_spec boot_btn =
	GPIO_DT_SPEC_GET(DT_NODELABEL(boot_button), gpios);
static struct gpio_callback btn_cb_data;

static void button_isr(const struct device *dev, struct gpio_callback *cb,
		       uint32_t pins)
{
	int val = gpio_pin_get_dt(&boot_btn);
	if (val == 1) {
		button_pressed = true;
	} else {
		recording_stop = true;
	}
}

static int button_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&boot_btn)) {
		LOG_ERR("BOOT button GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&boot_btn, GPIO_INPUT);
	if (ret) return ret;

	ret = gpio_pin_interrupt_configure_dt(&boot_btn, GPIO_INT_EDGE_BOTH);
	if (ret) return ret;

	gpio_init_callback(&btn_cb_data, button_isr, BIT(boot_btn.pin));
	gpio_add_callback(boot_btn.port, &btn_cb_data);

	LOG_INF("BOOT button ready (push-to-talk)");
	return 0;
}

/* ================================================================
 *  WiFi
 * ================================================================ */

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
	}
}

static void handle_ipv4_event(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb); ARG_UNUSED(iface);
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 address acquired");
		k_sem_give(&ipv4_ready_sem);
	}
}

static int wifi_connect(void)
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

	LOG_INF("Scanning WiFi ...");
	for (int attempt = 1; attempt <= 10; attempt++) {
		ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
		if (ret == 0) break;
		k_msleep(500);
	}
	if (ret) return ret;
	if (k_sem_take(&scan_done_sem, K_SECONDS(15))) return -ETIMEDOUT;

	params.ssid = (const uint8_t *)WIFI_SSID;
	params.ssid_length = strlen(WIFI_SSID);
	params.psk = (const uint8_t *)WIFI_PSK;
	params.psk_length = strlen(WIFI_PSK);
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.channel = WIFI_CHANNEL_ANY;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = SYS_FOREVER_MS;

	LOG_INF("Connecting to WiFi: %s ...", WIFI_SSID);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params,
		       sizeof(params));
	if (ret) return ret;
	if (k_sem_take(&wifi_connected_sem, K_SECONDS(30))) return -ETIMEDOUT;

	LOG_INF("Waiting for IPv4 ...");
	if (k_sem_take(&ipv4_ready_sem, K_SECONDS(30))) return -ETIMEDOUT;

	return 0;
}

/* ================================================================
 *  TLS
 * ================================================================ */

static int tls_setup(void)
{
	int ret;
	mbedtls_x509_crt cert;
	int parse_ret;
	int parsed_cert_count = 0;
	const mbedtls_x509_crt *node;

	mbedtls_x509_crt_init(&cert);
	parse_ret = mbedtls_x509_crt_parse(&cert, ca_certificate,
					 sizeof(ca_certificate));
	for (node = &cert; node != NULL; node = node->next) {
		if (node->raw.p == NULL || node->raw.len == 0U) {
			continue;
		}
		parsed_cert_count++;
	}
	LOG_INF("Local CA parse self-test: ret=%d parsed=%d bytes=%zu",
		parse_ret, parsed_cert_count, sizeof(ca_certificate));
	mbedtls_x509_crt_free(&cert);

	ret = tls_credential_add(CA_CERTIFICATE_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate, sizeof(ca_certificate));
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to register CA certificate: %d", ret);
		return ret;
	}

	LOG_INF("CA certificate ready (tag %d, %zu bytes)",
		CA_CERTIFICATE_TAG, sizeof(ca_certificate));
	return 0;
}

static bool tls_verify_dn_contains(const char *dn, const char *needle)
{
	return strstr(dn, needle) != NULL;
}

static bool tls_verify_is_known_dashscope_chain(const char *subject,
						const char *issuer,
						int depth)
{
	switch (depth) {
	case 0:
		return tls_verify_dn_contains(subject, "CN=*.aliyuncs.com") &&
			tls_verify_dn_contains(issuer,
				"CN=GlobalSign GCC R3 OV TLS CA 2024");
	case 1:
		return tls_verify_dn_contains(subject,
				"CN=GlobalSign GCC R3 OV TLS CA 2024") &&
			tls_verify_dn_contains(issuer,
				"OU=GlobalSign Root CA - R3");
	case 2:
		return tls_verify_dn_contains(subject,
				"OU=GlobalSign Root CA - R3") &&
			tls_verify_dn_contains(issuer,
				"CN=GlobalSign Root CA");
	default:
		return false;
	}
}

static int tls_cert_verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth,
				      uint32_t *flags)
{
	char subject[160] = { 0 };
	char issuer[160] = { 0 };
	uint32_t original_flags = *flags;

	ARG_UNUSED(ctx);

	mbedtls_x509_dn_gets(subject, sizeof(subject), &crt->subject);
	mbedtls_x509_dn_gets(issuer, sizeof(issuer), &crt->issuer);

	if (original_flags != 0U) {
		LOG_WRN("TLS verify depth=%d flags=%08x subject=%s issuer=%s",
			depth, (unsigned int)original_flags, subject, issuer);
	}

	if ((original_flags & ~MBEDTLS_X509_BADCERT_NOT_TRUSTED) == 0U &&
	    tls_verify_is_known_dashscope_chain(subject, issuer, depth)) {
		LOG_WRN("Allowing known DashScope certificate chain element at depth %d",
			depth);
		*flags &= ~MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	}

	return (*flags == 0U) ? 0 : MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
}

static bool is_benchmark_ipv4(uint32_t ipv4)
{
	return (ipv4 & 0xfffe0000U) == 0xc6120000U;
}

static int tls_open_socket(int *sock)
{
	sec_tag_t sec_tags[] = { CA_CERTIFICATE_TAG };
	struct tls_cert_verify_cb cert_verify = {
		.cb = tls_cert_verify_cb,
		.ctx = NULL,
	};
	int verify = TLS_PEER_VERIFY_REQUIRED;
	int ret;

	*sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (*sock < 0) {
		LOG_ERR("Failed to create TLS socket: %d", errno);
		return -errno;
	}

	ret = zsock_setsockopt(*sock, SOL_TLS, TLS_PEER_VERIFY,
			       &verify, sizeof(verify));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_PEER_VERIFY: %d", errno);
		zsock_close(*sock);
		*sock = -1;
		return -errno;
	}

	ret = zsock_setsockopt(*sock, SOL_TLS, TLS_SEC_TAG_LIST,
			       sec_tags, sizeof(sec_tags));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_SEC_TAG_LIST: %d", errno);
		zsock_close(*sock);
		*sock = -1;
		return -errno;
	}

	ret = zsock_setsockopt(*sock, SOL_TLS, TLS_HOSTNAME,
			       TLS_PEER_HOSTNAME, sizeof(TLS_PEER_HOSTNAME));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_HOSTNAME: %d", errno);
		zsock_close(*sock);
		*sock = -1;
		return -errno;
	}

	ret = zsock_setsockopt(*sock, SOL_TLS, TLS_CIPHERSUITE_LIST,
			       ai_tls_ciphersuites, sizeof(ai_tls_ciphersuites));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_CIPHERSUITE_LIST: %d", errno);
		zsock_close(*sock);
		*sock = -1;
		return -errno;
	}

	ret = zsock_setsockopt(*sock, SOL_TLS, TLS_CERT_VERIFY_CALLBACK,
			       &cert_verify, sizeof(cert_verify));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_CERT_VERIFY_CALLBACK: %d", errno);
		zsock_close(*sock);
		*sock = -1;
		return -errno;
	}

	return 0;
}

static int tls_connect_sockaddr(const struct sockaddr *addr, socklen_t addrlen,
				const char *label, int *sock)
{
	int ret;

	ret = tls_open_socket(sock);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("TLS connecting via %s ...", label);
	ret = zsock_connect(*sock, addr, addrlen);
	if (ret < 0) {
		int err = errno;

		LOG_ERR("Socket connect via %s failed: %d", label, err);
		zsock_close(*sock);
		*sock = -1;
		return -err;
	}

	return 0;
}

static void tcp_probe_sockaddr(const struct sockaddr *addr, socklen_t addrlen,
			       const char *label)
{
	int sock;
	int ret;

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_WRN("TCP probe socket create failed for %s: %d", label, errno);
		return;
	}

	ret = zsock_connect(sock, addr, addrlen);
	if (ret < 0) {
		LOG_WRN("TCP probe to %s failed: %d", label, errno);
	} else {
		LOG_INF("TCP probe to %s succeeded", label);
	}

	zsock_close(sock);
}

static int tls_connect(int *sock)
{
	struct zsock_addrinfo hints = {0};
	struct zsock_addrinfo *res = NULL;
	static const char *const fallback_ips[] = {
		"8.140.217.18",
		"8.152.159.24",
		"39.96.213.166",
		"39.96.198.249",
	};
	int ret;
	bool try_fallback = false;

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
	uint32_t ipv4 = ntohl(addr4->sin_addr.s_addr);

	zsock_inet_ntop(AF_INET, &addr4->sin_addr, addr_str, sizeof(addr_str));
	LOG_INF("Resolved %s to %s", AI_HOST, addr_str);

	if (is_benchmark_ipv4(ipv4)) {
		LOG_WRN("DNS returned %s in 198.18.0.0/15 benchmark range; this often indicates a network-side routed VIP or unusable route on embedded devices",
			addr_str);
		try_fallback = true;
	}

	if (!try_fallback) {
		ret = tls_connect_sockaddr(res->ai_addr, res->ai_addrlen, addr_str, sock);
		if (ret == 0) {
			zsock_freeaddrinfo(res);
			LOG_INF("TLS connected!");
			return 0;
		}

		tcp_probe_sockaddr(res->ai_addr, res->ai_addrlen, addr_str);
		LOG_WRN("Primary DNS path failed; trying public fallback IPs");
	}

	zsock_freeaddrinfo(res);

	for (size_t i = 0; i < ARRAY_SIZE(fallback_ips); i++) {
		struct sockaddr_in fallback_addr = { 0 };

		fallback_addr.sin_family = AF_INET;
		fallback_addr.sin_port = htons(AI_PORT);
		ret = zsock_inet_pton(AF_INET, fallback_ips[i], &fallback_addr.sin_addr);
		if (ret != 1) {
			continue;
		}

		ret = tls_connect_sockaddr((const struct sockaddr *)&fallback_addr,
					  sizeof(fallback_addr), fallback_ips[i], sock);
		if (ret == 0) {
			LOG_INF("TLS connected via fallback IP %s", fallback_ips[i]);
			return 0;
		}

		tcp_probe_sockaddr((const struct sockaddr *)&fallback_addr,
				  sizeof(fallback_addr), fallback_ips[i]);
	}

	return -EHOSTUNREACH;
}

/* ================================================================
 *  WAV header builder
 * ================================================================ */

static void build_wav_header(uint8_t *hdr, int sample_rate,
			     int bits, int channels, int data_size)
{
	int byte_rate = sample_rate * channels * bits / 8;
	int block_align = channels * bits / 8;
	int file_size = data_size + 36;

	memcpy(hdr, "RIFF", 4);
	hdr[4] = file_size & 0xFF;
	hdr[5] = (file_size >> 8) & 0xFF;
	hdr[6] = (file_size >> 16) & 0xFF;
	hdr[7] = (file_size >> 24) & 0xFF;
	memcpy(hdr + 8, "WAVE", 4);
	memcpy(hdr + 12, "fmt ", 4);
	hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
	hdr[20] = 1; hdr[21] = 0;
	hdr[22] = channels & 0xFF; hdr[23] = 0;
	hdr[24] = sample_rate & 0xFF;
	hdr[25] = (sample_rate >> 8) & 0xFF;
	hdr[26] = (sample_rate >> 16) & 0xFF;
	hdr[27] = (sample_rate >> 24) & 0xFF;
	hdr[28] = byte_rate & 0xFF;
	hdr[29] = (byte_rate >> 8) & 0xFF;
	hdr[30] = (byte_rate >> 16) & 0xFF;
	hdr[31] = (byte_rate >> 24) & 0xFF;
	hdr[32] = block_align & 0xFF; hdr[33] = 0;
	hdr[34] = bits & 0xFF; hdr[35] = 0;
	memcpy(hdr + 36, "data", 4);
	hdr[40] = data_size & 0xFF;
	hdr[41] = (data_size >> 8) & 0xFF;
	hdr[42] = (data_size >> 16) & 0xFF;
	hdr[43] = (data_size >> 24) & 0xFF;
}

/* ================================================================
 *  Socket helpers
 * ================================================================ */

static int sock_send_all(int sock, const char *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		int ret = zsock_send(sock, data + sent, len - sent, 0);
		if (ret < 0) {
			LOG_ERR("send failed: %d", errno);
			return -errno;
		}
		sent += ret;
	}
	return 0;
}

/* ================================================================
 *  JSON helper
 * ================================================================ */

static int json_extract_string(const char *json, const char *key,
			       char *out, size_t max_len)
{
	char pattern[64];
	int plen = snprintk(pattern, sizeof(pattern), "\"%s\":\"", key);
	if (plen <= 0) return -1;

	const char *p = strstr(json, pattern);
	if (!p) {
		plen = snprintk(pattern, sizeof(pattern), "\"%s\": \"", key);
		p = strstr(json, pattern);
		if (!p) return -1;
	}

	p += plen;
	size_t i = 0;
	while (*p && *p != '"' && i < max_len - 1) {
		if (*p == '\\' && *(p + 1)) {
			p++;
		}
		out[i++] = *p++;
	}
	out[i] = '\0';
	return (int)i;
}

/* ================================================================
 *  SSE streaming
 * ================================================================ */

static bool stream_mode;
static bool stream_started;

static void process_sse_event(const char *json_str)
{
	if (strncmp(json_str, "[DONE]", 6) == 0) {
		LOG_INF("SSE stream complete");
		return;
	}

	/* Extract text */
	char text_chunk[256];
	int tlen = json_extract_string(json_str, "content", text_chunk,
				       sizeof(text_chunk));
	if (tlen > 0) {
		if (rsp_text_len + tlen < sizeof(rsp_text) - 1) {
			memcpy(rsp_text + rsp_text_len, text_chunk, tlen);
			rsp_text_len += tlen;
			rsp_text[rsp_text_len] = '\0';
		}
	}

	/* Extract audio */
	const char *audio_key = "\"audio\":{";
	const char *audio_pos = strstr(json_str, audio_key);
	if (!audio_pos) {
		audio_key = "\"audio\": {";
		audio_pos = strstr(json_str, audio_key);
	}
	if (!audio_pos) return;

	const char *data_key = "\"data\":\"";
	const char *dp = strstr(audio_pos, data_key);
	if (!dp) {
		data_key = "\"data\": \"";
		dp = strstr(audio_pos, data_key);
	}
	if (!dp) return;

	dp += strlen(data_key);
	const char *end = dp;
	while (*end && *end != '"') end++;
	size_t b64_len = end - dp;
	if (b64_len == 0) return;

	if (stream_mode) {
		if (!stream_started) {
			audio_stream_start();
			stream_started = true;
			LOG_INF("Streaming playback initiated");
		}

		int decoded = base64_decode(dp, b64_len,
					    (uint8_t *)rsp_pcm_24k,
					    MAX_RSP_PCM_24K * sizeof(int16_t));
		if (decoded <= 0) return;

		int samples_24k = decoded / sizeof(int16_t);
		int16_t *temp_16k = (int16_t *)rsp_audio_b64;
		int max_16k = MAX_RSP_AUDIO_B64 / sizeof(int16_t);
		int samples_16k = audio_resample_24k_to_16k(
			rsp_pcm_24k, samples_24k, temp_16k, max_16k);

		audio_stream_feed(temp_16k, samples_16k);
		rsp_audio_b64_len += b64_len;
	} else {
		while (dp < end && rsp_audio_b64_len < MAX_RSP_AUDIO_B64 - 1) {
			rsp_audio_b64[rsp_audio_b64_len++] = *dp++;
		}
	}
}

static void sse_feed(const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char c = data[i];
		if (c == '\n') {
			if (sse_line_len > 0) {
				sse_line_buf[sse_line_len] = '\0';
				if (strncmp(sse_line_buf, "data: ", 6) == 0) {
					process_sse_event(sse_line_buf + 6);
				}
				sse_line_len = 0;
			}
		} else if (c != '\r') {
			if (sse_line_len < SSE_LINE_BUF_SIZE - 1) {
				sse_line_buf[sse_line_len++] = c;
			}
		}
	}
}

/* ================================================================
 *  Buffered socket reader
 * ================================================================ */

static uint8_t  sock_buf[4096]
		__attribute__((section(".ext_ram.bss")));
static size_t   sock_buf_len;
static size_t   sock_buf_pos;
static bool     sse_done;

static int sock_read_byte(int sock)
{
	if (sock_buf_pos >= sock_buf_len) {
		struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLIN };
		int ret = zsock_poll(&pfd, 1, 60000);
		if (ret <= 0) return -1;
		ret = zsock_recv(sock, sock_buf, sizeof(sock_buf), 0);
		if (ret <= 0) return -1;
		sock_buf_len = ret;
		sock_buf_pos = 0;
	}
	return sock_buf[sock_buf_pos++];
}

static int sock_read_line(int sock, char *buf, size_t max)
{
	size_t i = 0;
	while (i < max - 1) {
		int c = sock_read_byte(sock);
		if (c < 0) return -1;
		if (c == '\n') break;
		if (c != '\r') buf[i++] = (char)c;
	}
	buf[i] = '\0';
	return (int)i;
}

static int sock_read_n(int sock, uint8_t *out, size_t n)
{
	size_t got = 0;
	while (got < n) {
		if (sock_buf_pos < sock_buf_len) {
			size_t avail = sock_buf_len - sock_buf_pos;
			size_t take = (n - got < avail) ? (n - got) : avail;
			memcpy(out + got, sock_buf + sock_buf_pos, take);
			sock_buf_pos += take;
			got += take;
		} else {
			struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLIN };
			int ret = zsock_poll(&pfd, 1, 60000);
			if (ret <= 0) return -1;
			ret = zsock_recv(sock, sock_buf, sizeof(sock_buf), 0);
			if (ret <= 0) return -1;
			sock_buf_len = ret;
			sock_buf_pos = 0;
		}
	}
	return 0;
}

/* ================================================================
 *  SSE stream receiver
 * ================================================================ */

static int receive_sse_stream(int sock)
{
	sock_buf_len = 0;
	sock_buf_pos = 0;
	sse_done = false;

	char line[512];
	bool is_chunked = false;
	int http_status = 0;

	if (sock_read_line(sock, line, sizeof(line)) < 0) {
		LOG_ERR("Failed to read HTTP status line");
		return -EIO;
	}
	if (strncmp(line, "HTTP/1.1 ", 9) == 0) {
		http_status = atoi(line + 9);
	}
	LOG_INF("HTTP status: %d", http_status);

	if (http_status != 200) {
		LOG_ERR("HTTP error: %s", line);
		return -EIO;
	}

	while (1) {
		int len = sock_read_line(sock, line, sizeof(line));
		if (len < 0) return -EIO;
		if (len == 0) break;
		if (strstr(line, "chunked")) is_chunked = true;
	}

	LOG_INF("SSE stream started (chunked=%d)", is_chunked);

	sse_line_len = 0;
	rsp_audio_b64_len = 0;
	rsp_text_len = 0;
	rsp_text[0] = '\0';

	/* Switch to STREAMING state so UI shows live text */
	current_state = AI_STATE_STREAMING;

	if (is_chunked) {
		while (!sse_done) {
			char chunk_hdr[32];
			int hlen;
			do {
				hlen = sock_read_line(sock, chunk_hdr,
						      sizeof(chunk_hdr));
				if (hlen < 0) goto done;
			} while (hlen == 0);

			char *endp;
			unsigned long chunk_size = strtoul(chunk_hdr, &endp, 16);
			if (endp == chunk_hdr) continue;
			if (chunk_size == 0) break;

			size_t remaining = chunk_size;
			while (remaining > 0) {
				size_t to_read = remaining;
				if (to_read > sizeof(http_recv_buf) - 1)
					to_read = sizeof(http_recv_buf) - 1;
				if (sock_read_n(sock, http_recv_buf, to_read) < 0) {
					sse_done = true;
					break;
				}
				sse_feed((char *)http_recv_buf, to_read);

				http_recv_buf[to_read] = '\0';
				if (strstr((char *)http_recv_buf, "[DONE]"))
					sse_done = true;

				remaining -= to_read;
			}

			char crlf[2];
			sock_read_n(sock, (uint8_t *)crlf, 2);
		}
	} else {
		while (!sse_done) {
			int c = sock_read_byte(sock);
			if (c < 0) break;
			char ch = (char)c;
			sse_feed(&ch, 1);
		}
	}

done:
	LOG_INF("SSE complete: text=%zu bytes, audio_b64=%zu bytes",
		rsp_text_len, rsp_audio_b64_len);
	return 0;
}

/* ================================================================
 *  Send voice request
 * ================================================================ */

static int send_voice_request(int sock, const char *b64_audio, size_t b64_len)
{
	static const char json_prefix[] =
		"{\"model\":\"" AI_MODEL "\","
		"\"stream\":true,"
		"\"enable_thinking\":false,"
		"\"modalities\":[\"text\",\"audio\"],"
		"\"audio\":{\"voice\":\"Tina\",\"format\":\"wav\"},"
		"\"messages\":["
		"{\"role\":\"system\",\"content\":\"You are a helpful voice assistant. "
		"Reply concisely in Chinese. Keep answers under 3 sentences.\"},"
		"{\"role\":\"user\",\"content\":["
		"{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";

	static const char json_suffix[] =
		"\",\"format\":\"wav\"}}"
		"]}"
		"],"
		"\"stream_options\":{\"include_usage\":true}"
		"}";

	size_t body_len = strlen(json_prefix) + b64_len + strlen(json_suffix);

	char http_header[512];
	int hlen = snprintk(http_header, sizeof(http_header),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Type: application/json\r\n"
		"Authorization: Bearer %s\r\n"
		"Content-Length: %zu\r\n"
		"\r\n",
		AI_URL, AI_HOST, AI_API_KEY, body_len);

	if (hlen <= 0) return -ENOMEM;

	LOG_INF("Sending request: header=%d body=%zu", hlen, body_len);

	int ret = sock_send_all(sock, http_header, hlen);
	if (ret) return ret;

	ret = sock_send_all(sock, json_prefix, strlen(json_prefix));
	if (ret) return ret;

	size_t sent = 0;
	while (sent < b64_len) {
		size_t chunk = b64_len - sent;
		if (chunk > 4096) chunk = 4096;
		ret = sock_send_all(sock, b64_audio + sent, chunk);
		if (ret) return ret;
		sent += chunk;
	}

	ret = sock_send_all(sock, json_suffix, strlen(json_suffix));
	if (ret) return ret;

	LOG_INF("Request sent, waiting for SSE response...");
	return receive_sse_stream(sock);
}

/* ================================================================
 *  Chat history
 * ================================================================ */

static void add_msg(bool is_user, const char *text)
{
	if (msg_count >= AI_MAX_MESSAGES) {
		/* Shift messages to make room */
		memmove(&messages[0], &messages[1],
			(AI_MAX_MESSAGES - 1) * sizeof(struct ai_chat_msg));
		msg_count = AI_MAX_MESSAGES - 1;
	}
	messages[msg_count].is_user = is_user;
	strncpy(messages[msg_count].text, text, AI_MSG_TEXT_SIZE - 1);
	messages[msg_count].text[AI_MSG_TEXT_SIZE - 1] = '\0';
	msg_count++;
}

/* ================================================================
 *  UI trigger semaphore
 * ================================================================ */

static K_SEM_DEFINE(ui_record_sem, 0, 1);

/* ================================================================
 *  Worker thread
 * ================================================================ */

#define AI_THREAD_STACK_SIZE 12288
K_THREAD_STACK_DEFINE(ai_thread_stack, AI_THREAD_STACK_SIZE);
static struct k_thread ai_thread_data;

static void ai_worker(void *p1, void *p2, void *p3)
{
	int ret;

	LOG_INF("AI worker starting...");

	/* 1. Init audio codecs */
	ret = audio_codec_init();
	if (ret) {
		LOG_ERR("Audio init failed: %d", ret);
		current_state = AI_STATE_ERROR;
		return;
	}

	/* 2. Init BOOT button */
	ret = button_init();
	if (ret) {
		LOG_WRN("BOOT button init failed: %d (UI mic still works)", ret);
	}

	/* 3. Connect WiFi */
	current_state = AI_STATE_WIFI_CONNECTING;
	ret = wifi_connect();
	if (ret) {
		LOG_ERR("WiFi failed: %d", ret);
		current_state = AI_STATE_ERROR;
		return;
	}
	wifi_ok = true;

	/* 4. Register TLS CA cert */
	ret = tls_setup();
	if (ret) {
		LOG_ERR("TLS setup failed: %d", ret);
		current_state = AI_STATE_ERROR;
		return;
	}

	current_state = AI_STATE_READY;
	LOG_INF("AI service ready — press BOOT or tap MIC");

#if defined(CONFIG_AI_AUTO_DEBUG)
	/* Auto-debug: immediately test TLS + AI without button press */
	LOG_INF("[AUTO-DEBUG] Auto-triggering TLS test in 2s ...");
	k_msleep(2000);
	{
		int sock = -1;
		ret = tls_connect(&sock);
		if (ret) {
			LOG_ERR("[AUTO-DEBUG] TLS connect FAILED: %d", ret);
		} else {
			LOG_INF("[AUTO-DEBUG] TLS connect SUCCESS!");
			/* Send a minimal text-only request to verify HTTP/SSE */
			static const char test_body[] =
				"{\"model\":\"" AI_MODEL "\","
				"\"stream\":false,"
				"\"enable_thinking\":false,"
				"\"messages\":["
				"{\"role\":\"user\",\"content\":\"hi\"}"
				"]}";
			char hdr[512];
			int hlen = snprintk(hdr, sizeof(hdr),
				"POST %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Content-Type: application/json\r\n"
				"Authorization: Bearer %s\r\n"
				"Content-Length: %zu\r\n"
				"\r\n",
				AI_URL, AI_HOST, AI_API_KEY, strlen(test_body));
			if (hlen > 0) {
				ret = sock_send_all(sock, hdr, hlen);
				if (ret == 0) {
					ret = sock_send_all(sock, test_body,
							    strlen(test_body));
				}
				if (ret == 0) {
					/* Read HTTP status line */
					char line[256];
					sock_buf_len = 0;
					sock_buf_pos = 0;
					if (sock_read_line(sock, line,
							   sizeof(line)) >= 0) {
						LOG_INF("[AUTO-DEBUG] Response: %s",
							line);
					}
				}
			}
			zsock_close(sock);
		}
		LOG_INF("[AUTO-DEBUG] Test complete.");
	}
#endif /* CONFIG_AI_AUTO_DEBUG */

	/* ── Main loop: wait for trigger → record → send → play ── */
	while (1) {
		button_pressed = false;
		recording_stop = false;

		/* Wait for BOOT button press or UI trigger */
		while (!button_pressed) {
			if (k_sem_take(&ui_record_sem, K_MSEC(50)) == 0) {
				break;
			}
		}

		/* ── Recording ── */
		current_state = AI_STATE_RECORDING;
		recording_stop = false;

		int num_samples = audio_record(record_pcm, MAX_RECORD_SAMPLES,
					       &recording_stop);
		if (num_samples <= 0) {
			LOG_ERR("Recording failed: %d", num_samples);
			current_state = AI_STATE_READY;
			continue;
		}

		LOG_INF("Recorded %d samples (%.1fs)",
			num_samples, (float)num_samples / RECORD_SAMPLE_RATE);

		/* Add user message */
		add_msg(true, "Voice message");

		/* ── Encode ── */
		current_state = AI_STATE_PROCESSING;

		int pcm_bytes = num_samples * sizeof(int16_t);
		uint8_t wav_header[WAV_HEADER_SIZE];
		build_wav_header(wav_header, RECORD_SAMPLE_RATE, 16, 1, pcm_bytes);

		uint8_t *wav_tmp = (uint8_t *)rsp_pcm_24k;
		int wav_total = WAV_HEADER_SIZE + pcm_bytes;
		memcpy(wav_tmp, wav_header, WAV_HEADER_SIZE);
		memcpy(wav_tmp + WAV_HEADER_SIZE, record_pcm, pcm_bytes);

		size_t b64_len = base64_encode(wav_tmp, wav_total,
					       b64_wav_buf, MAX_B64_WAV_SIZE);
		if (b64_len == 0) {
			LOG_ERR("Base64 encode failed");
			current_state = AI_STATE_READY;
			continue;
		}
		LOG_INF("WAV encoded: %d → %zu b64", wav_total, b64_len);

		/* ── TLS connect + send + SSE receive ── */
		int sock = -1;
		ret = tls_connect(&sock);
		if (ret) {
			LOG_ERR("TLS connect failed: %d", ret);
			add_msg(false, "[Connection error]");
			current_state = AI_STATE_READY;
			continue;
		}

		stream_mode = true;
		stream_started = false;

		ret = send_voice_request(sock, b64_wav_buf, b64_len);
		zsock_close(sock);

		/* ── Playback ── */
		if (stream_started) {
			current_state = AI_STATE_PLAYING;
			audio_stream_stop();
		}
		stream_mode = false;

		/* Add AI response to history */
		if (rsp_text_len > 0) {
			add_msg(false, rsp_text);
			printk("\n=== AI: %s ===\n", rsp_text);
		} else {
			add_msg(false, "[No response]");
		}

		current_state = AI_STATE_READY;
		LOG_INF("Ready for next question");
	}
}

/* ================================================================
 *  Public API
 * ================================================================ */

static bool inited;

int ai_service_init(void)
{
	if (inited) return 0;
	inited = true;

	k_thread_create(&ai_thread_data, ai_thread_stack,
			AI_THREAD_STACK_SIZE,
			ai_worker, NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_name_set(&ai_thread_data, "ai_worker");

	LOG_INF("AI service thread started");
	return 0;
}

enum ai_state ai_service_get_state(void)
{
	return current_state;
}

void ai_service_start_recording(void)
{
	if (current_state == AI_STATE_READY) {
		k_sem_give(&ui_record_sem);
	}
}

void ai_service_stop_recording(void)
{
	recording_stop = true;
}

int ai_service_get_msg_count(void)
{
	return msg_count;
}

const struct ai_chat_msg *ai_service_get_msg(int idx)
{
	if (idx < 0 || idx >= msg_count) return NULL;
	return &messages[idx];
}

const char *ai_service_get_live_text(void)
{
	return rsp_text;
}

size_t ai_service_get_live_text_len(void)
{
	return rsp_text_len;
}

bool ai_service_wifi_connected(void)
{
	return wifi_ok;
}
