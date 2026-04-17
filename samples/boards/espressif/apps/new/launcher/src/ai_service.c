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

/* Qwen-Omni Realtime WebSocket API */
#define RT_MODEL  "qwen3.5-omni-flash-realtime"
#define RT_WS_PATH "/api-ws/v1/realtime?model=" RT_MODEL
#define RT_VOICE  "Serena"
#define RT_MAX_TOKENS 48
#define RT_TEMPERATURE_JSON "0.2"

/* Plan-A pipeline step 1: Qwen3-ASR-Flash one-shot recognition over the
 * native DashScope multimodal-generation endpoint (NOT OpenAI compat).
 * Set AI_DEBUG_PIPELINE_ASR to 1 to run ASR before Realtime and log the
 * recognized text. Existing Realtime flow is unchanged.
 */
#define ASR_MODEL "qwen3-asr-flash"
#define ASR_URL   "/api/v1/services/aigc/multimodal-generation/generation"
#define AI_DEBUG_PIPELINE_ASR 1

/* Plan-A pipeline step 2-A: qwen-turbo LLM probe over OpenAI compat SSE.
 * When AI_DEBUG_PIPELINE_LLM is 1 and ASR returned non-empty text, post
 * that text to chat/completions and log the streamed reply.
 */
#define LLM_MODEL "qwen-turbo"
#define AI_DEBUG_PIPELINE_LLM 1

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
#define RT_STREAM_PREBUFFER_SAMPLES  (PLAYBACK_SAMPLE_RATE * 2)
#define RT_STREAM_FEED_SAMPLES       (PLAYBACK_SAMPLE_RATE / 10)

/* ================================================================
 *  PSRAM large buffers (.ext_ram.bss)
 * ================================================================ */

static int16_t record_pcm[MAX_RECORD_SAMPLES]
		__attribute__((section(".ext_ram.bss")));

static int16_t response_pcm[MAX_PLAYBACK_SAMPLES]
		__attribute__((section(".ext_ram.bss")));

static char b64_wav_buf[MAX_B64_WAV_SIZE]
		__attribute__((section(".ext_ram.bss")));

static char rsp_audio_b64[MAX_RSP_AUDIO_B64]
		__attribute__((section(".ext_ram.bss")));

static int16_t rsp_pcm_24k[MAX_RSP_PCM_24K]
		__attribute__((section(".ext_ram.bss")));

static char sse_line_buf[SSE_LINE_BUF_SIZE]
		__attribute__((section(".ext_ram.bss")));

/* ================================================================
 *  SRAM buffers & state
 * ================================================================ */

/* These need fast access but move to PSRAM to save DRAM */
static uint8_t http_recv_buf[HTTP_RECV_BUF_SIZE]
		__attribute__((section(".ext_ram.bss")));
static uint8_t ws_tx_mask_buf[4096]
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

struct rt_playback_state {
	bool session_active;
	bool producer_done;
	bool playback_started;
	bool stream_failed;
	int available_samples;
	int consumed_samples;
	int64_t first_delta_ms;
};

static struct rt_playback_state rt_playback;
static K_MUTEX_DEFINE(rt_playback_lock);
static K_SEM_DEFINE(rt_playback_data_sem, 0, 1);
static K_SEM_DEFINE(rt_playback_done_sem, 0, 1);

#define RT_PLAYBACK_THREAD_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(rt_playback_thread_stack,
		      RT_PLAYBACK_THREAD_STACK_SIZE);
static struct k_thread rt_playback_thread_data;

static void rt_playback_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sem_take(&rt_playback_data_sem, K_FOREVER);

		while (1) {
			bool should_start = false;
			bool should_stop = false;
			int chunk_offset = 0;
			int chunk_samples = 0;
			int buffered_samples = 0;
			int64_t start_delay_ms = 0;

			k_mutex_lock(&rt_playback_lock, K_FOREVER);

			if (!rt_playback.session_active) {
				k_mutex_unlock(&rt_playback_lock);
				break;
			}

			buffered_samples = rt_playback.available_samples -
						  rt_playback.consumed_samples;

			if (!rt_playback.playback_started) {
				if (buffered_samples >= RT_STREAM_PREBUFFER_SAMPLES ||
				    (rt_playback.producer_done &&
				     buffered_samples > 0)) {
					rt_playback.playback_started = true;
					should_start = true;
					if (rt_playback.first_delta_ms > 0)
						start_delay_ms =
							k_uptime_get() -
							rt_playback.first_delta_ms;
				} else if (rt_playback.producer_done &&
					   buffered_samples == 0) {
					rt_playback.session_active = false;
					k_mutex_unlock(&rt_playback_lock);
					k_sem_give(&rt_playback_done_sem);
					break;
				} else {
					k_mutex_unlock(&rt_playback_lock);
					break;
				}
			}

			buffered_samples = rt_playback.available_samples -
						  rt_playback.consumed_samples;
			if (buffered_samples > 0 && !rt_playback.stream_failed) {
				chunk_offset = rt_playback.consumed_samples;
				chunk_samples = buffered_samples > RT_STREAM_FEED_SAMPLES
					? RT_STREAM_FEED_SAMPLES
					: buffered_samples;
				rt_playback.consumed_samples += chunk_samples;
			} else if (rt_playback.producer_done) {
				should_stop = rt_playback.playback_started &&
					      !rt_playback.stream_failed;
				rt_playback.session_active = false;
			}

			k_mutex_unlock(&rt_playback_lock);

			if (should_start) {
				int ret = audio_stream_start();

				if (ret) {
					LOG_ERR("Realtime: buffered playback start failed: %d",
						ret);
					k_mutex_lock(&rt_playback_lock, K_FOREVER);
					rt_playback.stream_failed = true;
					rt_playback.session_active = false;
					k_mutex_unlock(&rt_playback_lock);
					k_sem_give(&rt_playback_done_sem);
					break;
				}

				current_state = AI_STATE_PLAYING;
				LOG_INF("Realtime: buffered playback started "
					"(%d samples buffered = %.2fs, %lld ms after first delta)",
					buffered_samples,
					(float)buffered_samples /
					PLAYBACK_SAMPLE_RATE,
					(long long)start_delay_ms);
			}

			if (chunk_samples > 0) {
				int fed = audio_stream_feed(response_pcm + chunk_offset,
							  chunk_samples);

				if (fed < chunk_samples) {
					LOG_WRN("Realtime: buffered playback short feed "
						"(%d/%d samples)",
						fed, chunk_samples);
				}

				k_mutex_lock(&rt_playback_lock, K_FOREVER);
				buffered_samples = rt_playback.available_samples -
						  rt_playback.consumed_samples;
				should_stop = rt_playback.producer_done &&
					      buffered_samples == 0;
				if (should_stop)
					rt_playback.session_active = false;
				k_mutex_unlock(&rt_playback_lock);
			}

			if (should_stop) {
				audio_stream_stop();
				LOG_INF("Realtime: buffered playback complete");
				k_sem_give(&rt_playback_done_sem);
				break;
			}

			if (chunk_samples == 0) {
				break;
			}
		}
	}
}

static void rt_playback_begin(void)
{
	k_sem_reset(&rt_playback_data_sem);
	k_sem_reset(&rt_playback_done_sem);

	k_mutex_lock(&rt_playback_lock, K_FOREVER);
	memset(&rt_playback, 0, sizeof(rt_playback));
	rt_playback.session_active = true;
	k_mutex_unlock(&rt_playback_lock);
}

static void rt_playback_note_first_delta(void)
{
	k_mutex_lock(&rt_playback_lock, K_FOREVER);
	if (rt_playback.first_delta_ms == 0)
		rt_playback.first_delta_ms = k_uptime_get();
	k_mutex_unlock(&rt_playback_lock);
}

static int rt_playback_append(const int16_t *samples, int count)
{
	int copy = 0;

	k_mutex_lock(&rt_playback_lock, K_FOREVER);
	if (rt_playback.available_samples < MAX_PLAYBACK_SAMPLES) {
		int space = MAX_PLAYBACK_SAMPLES - rt_playback.available_samples;

		copy = count < space ? count : space;
		if (copy > 0) {
			memcpy(response_pcm + rt_playback.available_samples,
			       samples, copy * sizeof(int16_t));
			rt_playback.available_samples += copy;
		}
	}
	k_mutex_unlock(&rt_playback_lock);

	if (copy > 0)
		k_sem_give(&rt_playback_data_sem);

	return copy;
}

static void rt_playback_finish(bool *stream_failed)
{
	k_mutex_lock(&rt_playback_lock, K_FOREVER);
	rt_playback.producer_done = true;
	k_mutex_unlock(&rt_playback_lock);

	k_sem_give(&rt_playback_data_sem);
	k_sem_take(&rt_playback_done_sem, K_FOREVER);

	if (stream_failed != NULL) {
		k_mutex_lock(&rt_playback_lock, K_FOREVER);
		*stream_failed = rt_playback.stream_failed;
		k_mutex_unlock(&rt_playback_lock);
	}
}

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

static int trim_recorded_pcm(int16_t *pcm, int num_samples)
{
	const int threshold = 600;
	const int margin = RECORD_SAMPLE_RATE / 8;
	int first = -1;
	int last = -1;

	for (int i = 0; i < num_samples; i++) {
		int sample = pcm[i];
		if (sample < 0) {
			sample = -sample;
		}
		if (sample >= threshold) {
			first = i;
			break;
		}
	}

	if (first < 0) {
		return num_samples;
	}

	for (int i = num_samples - 1; i >= 0; i--) {
		int sample = pcm[i];
		if (sample < 0) {
			sample = -sample;
		}
		if (sample >= threshold) {
			last = i;
			break;
		}
	}

	if (last < first) {
		return num_samples;
	}

	if (first > margin) {
		first -= margin;
	} else {
		first = 0;
	}
	if (last + margin < num_samples) {
		last += margin;
	} else {
		last = num_samples - 1;
	}

	int trimmed = last - first + 1;
	if (trimmed <= 0 || trimmed >= num_samples) {
		return num_samples;
	}

	memmove(pcm, pcm + first, trimmed * sizeof(*pcm));
	LOG_INF("Trimmed audio: %d -> %d samples (%.1fs -> %.1fs)",
		num_samples, trimmed,
		(float)num_samples / RECORD_SAMPLE_RATE,
		(float)trimmed / RECORD_SAMPLE_RATE);

	return trimmed;
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
static bool sse_done;
static int audio_chunk_count;

static void process_sse_event(const char *json_str)
{
	if (strncmp(json_str, "[DONE]", 6) == 0) {
		LOG_INF("SSE stream complete");
		sse_done = true;
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
			audio_chunk_count = 0;
			LOG_INF("Streaming playback initiated");
		}

		int decoded = base64_decode(dp, b64_len,
					    (uint8_t *)rsp_pcm_24k,
					    MAX_RSP_PCM_24K * sizeof(int16_t));
		if (decoded <= 0) return;

		audio_chunk_count++;

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

/* Debug-only: when set, dump every "data:" SSE line raw before parsing. */
static bool dbg_dump_sse;

/* Plan-A pipeline step 1: when set, parse DashScope native ASR SSE
 * (cumulative "content":[{"text":"..."}] format) instead of the OpenAI
 * compat "content":"..." format used by chat/completions.
 */
static bool asr_mode;

static void asr_extract_text(const char *json_str)
{
	static const char pat[] = "\"content\":[{\"text\":\"";
	const char *p = strstr(json_str, pat);
	if (!p) return;

	p += sizeof(pat) - 1;
	const char *e = p;
	while (*e && *e != '"') {
		if (*e == '\\' && *(e + 1)) {
			e += 2;
			continue;
		}
		e++;
	}
	size_t len = e - p;
	if (len >= sizeof(rsp_text)) len = sizeof(rsp_text) - 1;
	memcpy(rsp_text, p, len);
	rsp_text[len] = '\0';
	rsp_text_len = len;  /* cumulative: reset, do not append */
}

static void sse_feed(const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char c = data[i];
		if (c == '\n') {
			if (sse_line_len > 0) {
				sse_line_buf[sse_line_len] = '\0';
				/* Tolerate both "data: " (OpenAI) and
				 * "data:" (DashScope native).
				 */
				char *payload = NULL;
				if (strncmp(sse_line_buf, "data:", 5) == 0) {
					payload = sse_line_buf + 5;
					if (*payload == ' ') payload++;
				}
				if (payload) {
					if (dbg_dump_sse) {
						LOG_INF("SSE raw[%zu]: %.480s",
							strlen(payload),
							payload);
					}
					if (asr_mode) {
						asr_extract_text(payload);
					} else {
						process_sse_event(payload);
					}
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
				if (dbg_dump_sse) {
					size_t dump = to_read < 480 ?
						      to_read : 480;
					http_recv_buf[dump] = '\0';
					LOG_INF("CHUNK raw[%zu]: %s",
						to_read,
						(char *)http_recv_buf);
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
	/* Drain any remaining HTTP chunked data so socket is clean for reuse */
	if (is_chunked) {
		char drain[64];
		struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLIN };
		while (zsock_poll(&pfd, 1, 200) > 0) {
			int r = zsock_recv(sock, drain, sizeof(drain), 0);
			if (r <= 0) break;
		}
	}
	/* Reset buffered reader state */
	sock_buf_len = 0;
	sock_buf_pos = 0;

	LOG_INF("SSE complete: text=%zu bytes, audio_b64=%zu bytes, chunks=%d",
		rsp_text_len, rsp_audio_b64_len, audio_chunk_count);
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
 *  WebSocket minimal client (for CosyVoice TTS)
 * ================================================================ */

#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

#define WS_URL   "/api-ws/v1/inference"

struct ws_frame {
	uint8_t opcode;
	size_t  payload_len;
};

static int ws_send_frame(int sock, uint8_t opcode,
			 const uint8_t *data, size_t len)
{
	uint8_t header[14];
	int hlen = 0;
	uint32_t mask_val = k_cycle_get_32();
	uint8_t mask[4];

	memcpy(mask, &mask_val, 4);

	header[0] = 0x80 | opcode;  /* FIN + opcode */
	if (len < 126) {
		header[1] = 0x80 | (uint8_t)len;
		hlen = 2;
	} else if (len < 65536) {
		header[1] = 0x80 | 126;
		header[2] = (len >> 8) & 0xFF;
		header[3] = len & 0xFF;
		hlen = 4;
	} else {
		header[1] = 0x80 | 127;
		memset(header + 2, 0, 4);
		header[6] = (len >> 24) & 0xFF;
		header[7] = (len >> 16) & 0xFF;
		header[8] = (len >> 8) & 0xFF;
		header[9] = len & 0xFF;
		hlen = 10;
	}
	memcpy(header + hlen, mask, 4);
	hlen += 4;

	int ret = sock_send_all(sock, (const char *)header, hlen);
	if (ret) return ret;

	/* Send masked payload in chunks */
	size_t sent = 0;
	while (sent < len) {
		size_t chunk = len - sent;
		if (chunk > sizeof(ws_tx_mask_buf))
			chunk = sizeof(ws_tx_mask_buf);
		for (size_t i = 0; i < chunk; i++) {
			ws_tx_mask_buf[i] = data[sent + i] ^ mask[(sent + i) & 3];
		}
		ret = sock_send_all(sock, (const char *)ws_tx_mask_buf, chunk);
		if (ret) return ret;
		sent += chunk;
	}
	return 0;
}

static int ws_recv_frame_header(int sock, struct ws_frame *frame)
{
	uint8_t hdr[2];
	if (sock_read_n(sock, hdr, 2) < 0) return -1;

	frame->opcode = hdr[0] & 0x0F;
	bool has_mask = (hdr[1] & 0x80) != 0;
	size_t len = hdr[1] & 0x7F;

	if (len == 126) {
		uint8_t ext[2];
		if (sock_read_n(sock, ext, 2) < 0) return -1;
		len = ((size_t)ext[0] << 8) | ext[1];
	} else if (len == 127) {
		uint8_t ext[8];
		if (sock_read_n(sock, ext, 8) < 0) return -1;
		len = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16) |
		      ((size_t)ext[6] << 8) | ext[7];
	}

	if (has_mask) {
		uint8_t mask_key[4];
		if (sock_read_n(sock, mask_key, 4) < 0) return -1;
	}

	frame->payload_len = len;
	return 0;
}

static int ws_skip_payload(int sock, size_t len)
{
	while (len > 0) {
		size_t s = len > sizeof(http_recv_buf) ? sizeof(http_recv_buf) : len;
		if (sock_read_n(sock, http_recv_buf, s) < 0) return -1;
		len -= s;
	}
	return 0;
}

static void ws_log_close_frame(int sock, size_t payload_len,
			      const char *context)
{
	uint8_t payload[126];
	size_t rd = payload_len;

	if (rd > sizeof(payload) - 1) {
		rd = sizeof(payload) - 1;
	}

	if (rd > 0) {
		if (sock_read_n(sock, payload, rd) < 0) {
			LOG_WRN("%s: server closed connection (failed to read close payload)",
				context);
			return;
		}
	}

	if (payload_len > rd) {
		if (ws_skip_payload(sock, payload_len - rd) < 0) {
			LOG_WRN("%s: server closed connection (failed to skip close payload)",
				context);
			return;
		}
	}

	payload[rd] = '\0';

	if (rd >= 2) {
		uint16_t code = ((uint16_t)payload[0] << 8) | payload[1];
		const char *reason = (rd > 2) ? (const char *)(payload + 2) : "";

		LOG_WRN("%s: server closed connection (code=%u reason=%s)",
			context, code, reason[0] ? reason : "<empty>");
	} else if (rd > 0) {
		LOG_WRN("%s: server closed connection (%u payload bytes)",
			context, (unsigned int)payload_len);
	} else {
		LOG_WRN("%s: server closed connection", context);
	}
}

static int ws_upgrade(int sock)
{
	char req[512];
	int rlen = snprintk(req, sizeof(req),
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"Authorization: bearer %s\r\n"
		"\r\n",
		WS_URL, AI_HOST, AI_API_KEY);

	sock_buf_len = 0;
	sock_buf_pos = 0;

	int ret = sock_send_all(sock, req, rlen);
	if (ret) return ret;

	char line[256];
	if (sock_read_line(sock, line, sizeof(line)) < 0) return -EIO;

	if (strncmp(line, "HTTP/1.1 101", 12) != 0) {
		LOG_ERR("WebSocket upgrade failed: %s", line);
		return -EIO;
	}

	/* Skip remaining response headers */
	while (1) {
		int len = sock_read_line(sock, line, sizeof(line));
		if (len < 0) return -EIO;
		if (len == 0) break;
	}

	LOG_INF("WebSocket upgraded");
	return 0;
}

/* ================================================================
 *  CosyVoice TTS
 * ================================================================ */

#define TTS_MODEL   "cosyvoice-v3-flash"
#define TTS_VOICE   "longanyang"

static size_t json_escape_str(const char *src, char *dst, size_t dst_max)
{
	size_t si = 0, di = 0;
	while (src[si] && di < dst_max - 2) {
		char c = src[si];
		if (c == '"' || c == '\\') {
			if (di + 2 > dst_max - 1) break;
			dst[di++] = '\\';
			dst[di++] = c;
		} else if (c == '\n') {
			if (di + 2 > dst_max - 1) break;
			dst[di++] = '\\';
			dst[di++] = 'n';
		} else if (c == '\r') {
			/* skip CR */
		} else if (c == '\t') {
			if (di + 2 > dst_max - 1) break;
			dst[di++] = '\\';
			dst[di++] = 't';
		} else {
			dst[di++] = c;
		}
		si++;
	}
	dst[di] = '\0';
	return di;
}

static int cosyvoice_tts(int sock, const char *text)
{
	int ret;

	LOG_INF("TTS: starting CosyVoice for %zu bytes of text", strlen(text));

	/* 1. WebSocket upgrade on existing TLS socket */
	ret = ws_upgrade(sock);
	if (ret) {
		return ret;
	}

	/* 3. Generate task_id */
	uint32_t t1 = k_cycle_get_32();
	uint32_t t2 = t1 ^ 0xDEADBEEF;
	char task_id[40];
	snprintk(task_id, sizeof(task_id), "%08x-0000-4000-8000-%08x%04x",
		 t1, t2, (t1 >> 16) & 0xFFFF);

	/* 4. Send run-task (use sse_line_buf as temp) */
	int clen = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
		"{\"header\":{\"action\":\"run-task\","
		"\"task_id\":\"%s\","
		"\"streaming\":\"duplex\"},"
		"\"payload\":{"
		"\"task_group\":\"audio\","
		"\"task\":\"tts\","
		"\"function\":\"SpeechSynthesizer\","
		"\"model\":\"" TTS_MODEL "\","
		"\"parameters\":{"
		"\"text_type\":\"PlainText\","
		"\"voice\":\"" TTS_VOICE "\","
		"\"format\":\"pcm\","
		"\"sample_rate\":16000,"
		"\"volume\":50,"
		"\"rate\":1.0,"
		"\"pitch\":1.0},"
		"\"input\":{}}}", task_id);

	ret = ws_send_frame(sock, WS_OP_TEXT,
			    (const uint8_t *)sse_line_buf, clen);
	if (ret) goto cleanup;
	LOG_INF("TTS: run-task sent");

	/* 5. Wait for task-started */
	{
		struct ws_frame frame;
		char json_buf[1024];
		bool started = false;

		while (!started) {
			if (ws_recv_frame_header(sock, &frame) < 0) {
				ret = -EIO;
				goto cleanup;
			}
			if (frame.opcode == WS_OP_TEXT) {
				size_t rd = frame.payload_len;
				if (rd >= sizeof(json_buf)) rd = sizeof(json_buf) - 1;
				if (sock_read_n(sock, (uint8_t *)json_buf, rd) < 0) {
					ret = -EIO;
					goto cleanup;
				}
				json_buf[rd] = '\0';
				if (frame.payload_len > rd)
					ws_skip_payload(sock, frame.payload_len - rd);

				if (strstr(json_buf, "task-started")) {
					started = true;
					LOG_INF("TTS: task started");
				} else if (strstr(json_buf, "task-failed")) {
					LOG_ERR("TTS: task failed: %s", json_buf);
					ret = -EIO;
					goto cleanup;
				}
			} else {
				ws_skip_payload(sock, frame.payload_len);
			}
		}
	}

	/* 6. Send continue-task with text */
	{
		/* Escape text for JSON */
		char *escaped = (char *)rsp_audio_b64;  /* reuse 512KB buffer */
		json_escape_str(text, escaped, MAX_RSP_AUDIO_B64 / 2);

		clen = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
			"{\"header\":{\"action\":\"continue-task\","
			"\"task_id\":\"%s\","
			"\"streaming\":\"duplex\"},"
			"\"payload\":{\"input\":{\"text\":\"%s\"}}}",
			task_id, escaped);

		ret = ws_send_frame(sock, WS_OP_TEXT,
				    (const uint8_t *)sse_line_buf, clen);
		if (ret) goto cleanup;
		LOG_INF("TTS: continue-task sent (%d bytes)", clen);
	}

	/* 7. Send finish-task */
	clen = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
		"{\"header\":{\"action\":\"finish-task\","
		"\"task_id\":\"%s\","
		"\"streaming\":\"duplex\"},"
		"\"payload\":{\"input\":{}}}", task_id);

	ret = ws_send_frame(sock, WS_OP_TEXT,
			    (const uint8_t *)sse_line_buf, clen);
	if (ret) goto cleanup;
	LOG_INF("TTS: finish-task sent");

	/* 8. Receive binary PCM frames until task-finished */
	{
		struct ws_frame frame;
		char json_buf[1024];
		int16_t *pcm_out = rsp_pcm_24k;  /* reuse as PCM collect buffer */
		int total_samples = 0;
		int max_samples = MAX_RSP_PCM_24K;
		int pcm_chunks = 0;
		bool finished = false;

		while (!finished) {
			if (ws_recv_frame_header(sock, &frame) < 0) break;

			if (frame.opcode == WS_OP_BINARY) {
				/* PCM audio data */
				size_t remaining = frame.payload_len;
				while (remaining > 0) {
					size_t avail = (max_samples - total_samples)
						       * sizeof(int16_t);
					size_t to_read = remaining;
					if (to_read > avail) to_read = avail;
					if (to_read == 0) {
						ws_skip_payload(sock, remaining);
						break;
					}
					if (sock_read_n(sock,
							(uint8_t *)&pcm_out[total_samples],
							to_read) < 0) {
						finished = true;
						break;
					}
					total_samples += to_read / sizeof(int16_t);
					remaining -= to_read;
				}
				pcm_chunks++;
			} else if (frame.opcode == WS_OP_TEXT) {
				size_t rd = frame.payload_len;
				if (rd >= sizeof(json_buf))
					rd = sizeof(json_buf) - 1;
				if (sock_read_n(sock, (uint8_t *)json_buf,
						rd) < 0)
					break;
				json_buf[rd] = '\0';
				if (frame.payload_len > rd)
					ws_skip_payload(sock,
							frame.payload_len - rd);

				if (strstr(json_buf, "task-finished")) {
					finished = true;
					LOG_INF("TTS: task finished, %d chunks, "
						"%d samples (%.1fs)",
						pcm_chunks, total_samples,
						(float)total_samples / 16000.0f);
				} else if (strstr(json_buf, "task-failed")) {
					LOG_ERR("TTS: task failed: %s",
						json_buf);
					finished = true;
				}
			} else if (frame.opcode == WS_OP_PING) {
				/* Reply with pong */
				ws_skip_payload(sock, frame.payload_len);
				ws_send_frame(sock, WS_OP_PONG, NULL, 0);
			} else if (frame.opcode == WS_OP_CLOSE) {
				LOG_WRN("TTS: server closed");
				finished = true;
			} else {
				ws_skip_payload(sock, frame.payload_len);
			}
		}

		/* 9. Play collected PCM */
		if (total_samples > 0) {
			LOG_INF("TTS: playing %d samples (%.1fs)",
				total_samples,
				(float)total_samples / 16000.0f);
			audio_play(pcm_out, total_samples);
		}
	}

cleanup:
	return ret;
}

/* ================================================================
 *  Qwen-Omni Realtime WebSocket session
 * ================================================================ */

static int rt_ws_upgrade(int sock)
{
	char req[512];
	int rlen = snprintk(req, sizeof(req),
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"Authorization: bearer %s\r\n"
		"\r\n",
		RT_WS_PATH, AI_HOST, AI_API_KEY);

	sock_buf_len = 0;
	sock_buf_pos = 0;

	int ret = sock_send_all(sock, req, rlen);
	if (ret) return ret;

	char line[256];
	if (sock_read_line(sock, line, sizeof(line)) < 0) return -EIO;

	if (strncmp(line, "HTTP/1.1 101", 12) != 0) {
		LOG_ERR("Realtime WS upgrade failed: %s", line);
		return -EIO;
	}

	while (1) {
		int len = sock_read_line(sock, line, sizeof(line));
		if (len < 0) return -EIO;
		if (len == 0) break;
	}

	LOG_INF("Realtime WebSocket upgraded");
	return 0;
}

static int rt_send_session_update(int sock)
{
	int len = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
		"{\"type\":\"session.update\","
		"\"session\":{"
		"\"modalities\":[\"text\",\"audio\"],"
		"\"voice\":\"" RT_VOICE "\","
		"\"instructions\":\"You are a helpful voice assistant. "
		"Reply in concise spoken Chinese. "
		"Prefer one short sentence.\","
		"\"input_audio_format\":\"pcm\","
		"\"output_audio_format\":\"pcm\","
		"\"temperature\":" RT_TEMPERATURE_JSON ","
		"\"max_tokens\":%d,"
		"\"turn_detection\":null}}",
		RT_MAX_TOKENS);

	LOG_INF("Realtime: session.update payload=%s", sse_line_buf);

	return ws_send_frame(sock, WS_OP_TEXT,
			     (const uint8_t *)sse_line_buf, len);
}

static int rt_send_audio_chunks(int sock, const int16_t *pcm,
				int num_samples)
{
	/* Send audio in 1s chunks to reduce WebSocket/TLS framing overhead. */
	const int CHUNK_SAMPLES = 16000;
	int offset = 0;
	int ret;
	int64_t send_start_ms = k_uptime_get();
	int total_chunks = (num_samples + CHUNK_SAMPLES - 1) / CHUNK_SAMPLES;
	int chunk_index = 0;

	while (offset < num_samples) {
		int chunk = num_samples - offset;
		if (chunk > CHUNK_SAMPLES) chunk = CHUNK_SAMPLES;
		int64_t chunk_start_ms = k_uptime_get();

		int pcm_bytes = chunk * sizeof(int16_t);
		size_t b64_len = base64_encode(
			(const uint8_t *)(pcm + offset), pcm_bytes,
			b64_wav_buf, MAX_B64_WAV_SIZE);
		int64_t b64_done_ms = k_uptime_get();

		int jlen = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
			"{\"type\":\"input_audio_buffer.append\","
			"\"audio\":\"%.*s\"}",
			(int)b64_len, b64_wav_buf);
		int64_t json_done_ms = k_uptime_get();

		ret = ws_send_frame(sock, WS_OP_TEXT,
				    (const uint8_t *)sse_line_buf, jlen);
		if (ret) return ret;
		chunk_index++;
		LOG_INF("Realtime: chunk %d/%d sent (%d samples, b64=%u, encode=%lld ms, json=%lld ms, send=%lld ms)",
			chunk_index, total_chunks, chunk, (unsigned int)b64_len,
			(long long)(b64_done_ms - chunk_start_ms),
			(long long)(json_done_ms - b64_done_ms),
			(long long)(k_uptime_get() - json_done_ms));

		offset += chunk;
	}

	LOG_INF("Realtime: sent %d audio chunks (%d samples) in %lld ms",
		(num_samples + CHUNK_SAMPLES - 1) / CHUNK_SAMPLES,
		num_samples,
		(long long)(k_uptime_get() - send_start_ms));

	/* Send commit to trigger server processing */
	static const char commit_json[] =
		"{\"type\":\"input_audio_buffer.commit\"}";
	ret = ws_send_frame(sock, WS_OP_TEXT,
			    (const uint8_t *)commit_json,
			    strlen(commit_json));
	if (ret) return ret;

	/* Manual mode: explicitly request response after commit */
	static const char create_json[] =
		"{\"type\":\"response.create\"}";
	return ws_send_frame(sock, WS_OP_TEXT,
			     (const uint8_t *)create_json,
			     strlen(create_json));
}

static int rt_receive_response(int sock)
{
	struct ws_frame frame;
	bool done = false;
	bool audio_truncated = false;
	bool stream_failed = false;
	int audio_chunks = 0;
	int accum_samples = 0;

	rsp_text_len = 0;
	rsp_text[0] = '\0';
	current_state = AI_STATE_PROCESSING;
	rt_playback_begin();

	while (!done) {
		if (ws_recv_frame_header(sock, &frame) < 0) break;

		if (frame.opcode == WS_OP_TEXT) {
			size_t rd = frame.payload_len;
			if (rd >= SSE_LINE_BUF_SIZE)
				rd = SSE_LINE_BUF_SIZE - 1;
			if (sock_read_n(sock, (uint8_t *)sse_line_buf,
					rd) < 0)
				break;
			sse_line_buf[rd] = '\0';
			if (frame.payload_len > rd)
				ws_skip_payload(sock,
						frame.payload_len - rd);

			char event_type[64];
			if (json_extract_string(sse_line_buf, "type",
						event_type,
						sizeof(event_type)) <= 0)
				continue;

			if (strcmp(event_type,
				   "response.audio.delta") == 0) {
				int64_t now_ms = k_uptime_get();
				/* Extract base64 audio from "delta" */
				const char *dp = strstr(sse_line_buf,
							"\"delta\":\"");
				if (!dp) {
					dp = strstr(sse_line_buf,
						    "\"delta\": \"");
					if (!dp) continue;
					dp += 10;
				} else {
					dp += 9;
				}
				const char *end = strchr(dp, '"');
				if (!end) continue;
				size_t b64_len = end - dp;

				if (audio_chunks == 0) {
					rt_playback_note_first_delta();
					LOG_INF("Realtime: first audio delta");
				}

				int decoded = base64_decode(
					dp, b64_len,
					(uint8_t *)rsp_pcm_24k,
					MAX_RSP_PCM_24K *
					sizeof(int16_t));
				if (decoded <= 0) continue;

				int samples_24k =
					decoded / sizeof(int16_t);
				int16_t *temp_16k =
					(int16_t *)rsp_audio_b64;
				int max_16k = MAX_RSP_AUDIO_B64 /
					sizeof(int16_t);
				int samples_16k =
					audio_resample_24k_to_16k(
						rsp_pcm_24k, samples_24k,
						temp_16k, max_16k);

				int copy = rt_playback_append(temp_16k,
							 samples_16k);
				accum_samples += copy;
				if (copy < samples_16k)
					audio_truncated = true;
				audio_chunks++;
				LOG_INF("Realtime: audio delta %d (%d samples = %.2fs, total=%d samples, t=+%lld ms)",
					audio_chunks,
					copy,
					(float)copy / PLAYBACK_SAMPLE_RATE,
					accum_samples,
					(long long)(now_ms -
						(rt_playback.first_delta_ms > 0
						 ? rt_playback.first_delta_ms
						 : now_ms)));

			} else if (strcmp(event_type,
				"response.audio_transcript.delta") == 0) {
				/* Audio-only mode: ignore transcript text. */

			} else if (strcmp(event_type,
				  "response.created") == 0) {
				LOG_INF("Realtime: response.created");

			} else if (strcmp(event_type,
				  "response.audio.done") == 0) {
				LOG_INF("Realtime: response.audio.done");

			} else if (strcmp(event_type,
					  "response.done") == 0) {
				LOG_INF("Realtime: response done "
					"(audio=%d chunks, %d samples "
					"= %.1fs, text=%zu bytes)",
					audio_chunks, accum_samples,
					(float)accum_samples /
					PLAYBACK_SAMPLE_RATE,
					rsp_text_len);
				done = true;

			} else if (strcmp(event_type, "error") == 0) {
				LOG_ERR("Realtime error: %s", sse_line_buf);
				done = true;

			} else if (strcmp(event_type,
					  "session.created") == 0 ||
				   strcmp(event_type,
					  "session.updated") == 0) {
				LOG_INF("Realtime: %s", event_type);
			}

		} else if (frame.opcode == WS_OP_PING) {
			ws_skip_payload(sock, frame.payload_len);
			ws_send_frame(sock, WS_OP_PONG, NULL, 0);

		} else if (frame.opcode == WS_OP_CLOSE) {
			LOG_WRN("Realtime: server closed connection");
			done = true;

		} else {
			ws_skip_payload(sock, frame.payload_len);
		}
	}

	rt_playback_finish(&stream_failed);

	if (audio_truncated) {
		LOG_WRN("Realtime: reply audio truncated at %.1fs",
			(float)accum_samples / PLAYBACK_SAMPLE_RATE);
	}

	if (stream_failed && accum_samples > 0) {
		current_state = AI_STATE_PLAYING;
		LOG_WRN("Realtime: falling back to one-shot playback (%d samples = %.1fs)",
			accum_samples,
			(float)accum_samples / PLAYBACK_SAMPLE_RATE);
		audio_play(response_pcm, accum_samples);
	}

	return 0;
}

static int realtime_voice_session(int sock, const int16_t *pcm,
				  int num_samples)
{
	int ret;

	LOG_INF("Realtime: starting session (%d samples, %.1fs)",
		num_samples, (float)num_samples / RECORD_SAMPLE_RATE);
	LOG_INF("Realtime: model=%s voice=%s max_tokens=%d",
		RT_MODEL, RT_VOICE, RT_MAX_TOKENS);

	/* 1. WebSocket upgrade */
	ret = rt_ws_upgrade(sock);
	if (ret) return ret;

	/* 2. Send session configuration */
	ret = rt_send_session_update(sock);
	if (ret) return ret;

	/* 3. Wait for session.created + session.updated before sending audio */
	{
		struct ws_frame frame;
		bool got_updated = false;
		int wait_events = 0;
		int64_t wait_deadline_ms = k_uptime_get() + 15000;

		while (!got_updated && k_uptime_get() < wait_deadline_ms) {
			if (ws_recv_frame_header(sock, &frame) < 0) {
				LOG_ERR("Realtime: lost connection waiting for session");
				return -EIO;
			}
			if (frame.opcode == WS_OP_TEXT) {
				size_t rd = frame.payload_len;
				if (rd >= SSE_LINE_BUF_SIZE)
					rd = SSE_LINE_BUF_SIZE - 1;
				if (sock_read_n(sock, (uint8_t *)sse_line_buf,
						rd) < 0)
					return -EIO;
				sse_line_buf[rd] = '\0';
				if (frame.payload_len > rd)
					ws_skip_payload(sock,
							frame.payload_len - rd);

				char etype[64];
				if (json_extract_string(sse_line_buf, "type",
							etype,
							sizeof(etype)) > 0) {
					LOG_INF("Realtime: %s", etype);
					if (strcmp(etype,
						   "session.updated") == 0)
						got_updated = true;
					else if (strcmp(etype, "error") == 0) {
						LOG_ERR("Realtime session error: %s",
							sse_line_buf);
						return -EIO;
					}
				} else {
					LOG_WRN("Realtime: session wait raw payload: %s",
						sse_line_buf);
				}
				wait_events++;
			} else if (frame.opcode == WS_OP_PING) {
				ws_skip_payload(sock, frame.payload_len);
				ws_send_frame(sock, WS_OP_PONG, NULL, 0);
			} else if (frame.opcode == WS_OP_CLOSE) {
				ws_log_close_frame(sock, frame.payload_len,
						   "Realtime session init");
				return -EIO;
			} else {
				ws_skip_payload(sock, frame.payload_len);
			}
		}

		if (!got_updated) {
			LOG_ERR("Realtime: no session.updated within timeout (%d events)",
				wait_events);
			return -EIO;
		}
	}

	/* 4. Send recorded audio as base64 PCM chunks */
	ret = rt_send_audio_chunks(sock, pcm, num_samples);
	if (ret) return ret;

	LOG_INF("Realtime: audio sent, receiving response...");

	/* 5. Receive and stream-play response */
	ret = rt_receive_response(sock);

	/* 6. Send close frame */
	ws_send_frame(sock, WS_OP_CLOSE, NULL, 0);

	return ret;
}

/* ================================================================
 *  Plan-A pipeline step 1: qwen3-asr-flash one-shot recognition
 * ================================================================ */

static int qwen_asr_recognize(int sock, const int16_t *pcm, int num_samples,
			      char *text_out, size_t text_max)
{
	const int pcm_bytes = num_samples * (int)sizeof(int16_t);
	const int wav_total = WAV_HEADER_SIZE + pcm_bytes;

	/* Reuse rsp_pcm_24k as scratch for raw WAV bytes (it is unused until
	 * the subsequent Realtime playback path runs).
	 */
	uint8_t *wav_buf = (uint8_t *)rsp_pcm_24k;
	const int wav_scratch_max = MAX_RSP_PCM_24K * (int)sizeof(int16_t);

	if (wav_total > wav_scratch_max) {
		LOG_ERR("ASR: WAV too large (%d > %d)", wav_total,
			wav_scratch_max);
		return -ENOMEM;
	}

	build_wav_header(wav_buf, RECORD_SAMPLE_RATE, 16, 1, pcm_bytes);
	memcpy(wav_buf + WAV_HEADER_SIZE, pcm, pcm_bytes);

	size_t b64_len = base64_encode(wav_buf, wav_total,
				       b64_wav_buf, MAX_B64_WAV_SIZE);
	if (b64_len == 0) {
		LOG_ERR("ASR: base64 encode failed (wav=%d)", wav_total);
		return -ENOMEM;
	}

	static const char json_prefix[] =
		"{\"model\":\"" ASR_MODEL "\","
		"\"input\":{\"messages\":["
		"{\"role\":\"system\",\"content\":[{\"text\":\"\"}]},"
		"{\"role\":\"user\",\"content\":["
		"{\"audio\":\"data:audio/wav;base64,";

	static const char json_suffix[] =
		"\"}"
		"]}"
		"]},"
		"\"parameters\":{\"asr_options\":{\"language\":\"zh\","
		"\"enable_lid\":false,\"enable_itn\":false}}}";

	size_t body_len = strlen(json_prefix) + b64_len + strlen(json_suffix);

	char http_header[512];
	int hlen = snprintk(http_header, sizeof(http_header),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Type: application/json\r\n"
		"Authorization: Bearer %s\r\n"
		"X-DashScope-SSE: enable\r\n"
		"Content-Length: %zu\r\n"
		"\r\n",
		ASR_URL, AI_HOST, AI_API_KEY, body_len);
	if (hlen <= 0) return -ENOMEM;

	LOG_INF("ASR request: model=%s body=%zu (b64=%zu)",
		ASR_MODEL, body_len, b64_len);

	bool saved_stream_mode = stream_mode;
	stream_mode = false;
	audio_chunk_count = 0;
	dbg_dump_sse = true;
	asr_mode = true;

	int64_t t0 = k_uptime_get();

	int ret = sock_send_all(sock, http_header, hlen);
	if (ret) goto out;

	ret = sock_send_all(sock, json_prefix, strlen(json_prefix));
	if (ret) goto out;

	size_t sent = 0;
	while (sent < b64_len) {
		size_t chunk = b64_len - sent;
		if (chunk > 4096) chunk = 4096;
		ret = sock_send_all(sock, b64_wav_buf + sent, chunk);
		if (ret) goto out;
		sent += chunk;
	}

	ret = sock_send_all(sock, json_suffix, strlen(json_suffix));
	if (ret) goto out;

	ret = receive_sse_stream(sock);
	if (ret == 0) {
		size_t copy = rsp_text_len;
		if (copy >= text_max) copy = text_max - 1;
		memcpy(text_out, rsp_text, copy);
		text_out[copy] = '\0';
		LOG_INF("ASR result (%lld ms, %zu bytes): %s",
			k_uptime_get() - t0, copy, text_out);
	} else {
		LOG_ERR("ASR: receive_sse_stream failed: %d", ret);
	}

out:
	stream_mode = saved_stream_mode;
	dbg_dump_sse = false;
	asr_mode = false;
	return ret;
}

/* ================================================================
 *  Plan-A pipeline step 2-A: qwen-turbo LLM probe (OpenAI compat SSE)
 * ================================================================ */

static int qwen_llm_chat(int sock, const char *user_text,
			 char *reply_out, size_t reply_max)
{
	/* JSON-escape user_text (max ~256 bytes ASR text → escaped ≤ 512). */
	char escaped[512];
	json_escape_str(user_text, escaped, sizeof(escaped));

	/* Build full JSON body in sse_line_buf (PSRAM, 64KB). */
	int body_len = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
		"{\"model\":\"" LLM_MODEL "\","
		"\"stream\":true,"
		"\"enable_thinking\":false,"
		"\"messages\":["
		"{\"role\":\"system\",\"content\":\"You are a helpful voice "
		"assistant. Reply concisely in Chinese, one short sentence.\"},"
		"{\"role\":\"user\",\"content\":\"%s\"}"
		"]}",
		escaped);
	if (body_len <= 0 || body_len >= SSE_LINE_BUF_SIZE) return -ENOMEM;

	char http_header[512];
	int hlen = snprintk(http_header, sizeof(http_header),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Type: application/json\r\n"
		"Authorization: Bearer %s\r\n"
		"Content-Length: %d\r\n"
		"\r\n",
		AI_URL, AI_HOST, AI_API_KEY, body_len);
	if (hlen <= 0) return -ENOMEM;

	LOG_INF("LLM request: model=%s body=%d (input=%zu)",
		LLM_MODEL, body_len, strlen(user_text));

	bool saved_stream_mode = stream_mode;
	stream_mode = false;  /* collect text only, no audio playback */
	audio_chunk_count = 0;

	int64_t t0 = k_uptime_get();

	int ret = sock_send_all(sock, http_header, hlen);
	if (ret) goto out;

	ret = sock_send_all(sock, sse_line_buf, body_len);
	if (ret) goto out;

	ret = receive_sse_stream(sock);
	if (ret == 0) {
		size_t copy = rsp_text_len;
		if (copy >= reply_max) copy = reply_max - 1;
		memcpy(reply_out, rsp_text, copy);
		reply_out[copy] = '\0';
		LOG_INF("LLM reply (%lld ms, %zu bytes): %s",
			k_uptime_get() - t0, copy, reply_out);
	} else {
		LOG_ERR("LLM: receive_sse_stream failed: %d", ret);
	}

out:
	stream_mode = saved_stream_mode;
	return ret;
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

		num_samples = trim_recorded_pcm(record_pcm, num_samples);

		/* ── TLS connect + Realtime WebSocket session ── */
		current_state = AI_STATE_PROCESSING;

#if AI_DEBUG_PIPELINE_ASR
		/* Plan-A step 1: probe qwen3-asr-flash on a separate TLS
		 * socket. Result is logged only; main flow is unchanged.
		 */
		char asr_text[256] = {0};
		{
			int asr_sock = -1;
			int64_t asr_t0 = k_uptime_get();
			int ar = tls_connect(&asr_sock);
			if (ar == 0) {
				qwen_asr_recognize(asr_sock, record_pcm,
						   num_samples,
						   asr_text,
						   sizeof(asr_text));
				zsock_close(asr_sock);
				LOG_INF("ASR probe total %lld ms",
					k_uptime_get() - asr_t0);
			} else {
				LOG_WRN("ASR probe: tls_connect failed: %d",
					ar);
			}
		}

#if AI_DEBUG_PIPELINE_LLM
		/* Plan-A step 2-A: if ASR returned non-empty text, probe
		 * qwen-turbo LLM on a separate TLS socket. Reply is logged
		 * only; Realtime flow below is still unchanged.
		 */
		if (asr_text[0] != '\0') {
			int llm_sock = -1;
			char llm_reply[512];
			int64_t llm_t0 = k_uptime_get();
			int lr = tls_connect(&llm_sock);
			if (lr == 0) {
				qwen_llm_chat(llm_sock, asr_text,
					      llm_reply,
					      sizeof(llm_reply));
				zsock_close(llm_sock);
				LOG_INF("LLM probe total %lld ms",
					k_uptime_get() - llm_t0);
			} else {
				LOG_WRN("LLM probe: tls_connect failed: %d",
					lr);
			}
		} else {
			LOG_WRN("LLM probe skipped: ASR text empty");
		}
#endif
#endif

		int sock = -1;
		ret = tls_connect(&sock);
		if (ret) {
			LOG_ERR("TLS connect failed: %d", ret);
			current_state = AI_STATE_READY;
			continue;
		}

		ret = realtime_voice_session(sock, record_pcm, num_samples);

		zsock_close(sock);

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

	k_thread_create(&rt_playback_thread_data,
			rt_playback_thread_stack,
			RT_PLAYBACK_THREAD_STACK_SIZE,
			rt_playback_thread, NULL, NULL, NULL,
			6, 0, K_NO_WAIT);
	k_thread_name_set(&rt_playback_thread_data, "ai_rt_playback");

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
