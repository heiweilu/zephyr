/*
 * Voice Assistant — Phase 10: AI 语音助手
 *
 * Push-to-Talk (BOOT button IO0) → Record mic (ES7210) →
 * base64 WAV → SSE HTTP POST to qwen3.5-omni-flash →
 * Parse text + audio response → Play through speaker (ES8311)
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

#include "audio.h"
#include "base64.h"
#include "secrets.h"
#include "ca_certificate.h"

LOG_MODULE_REGISTER(voice_assistant, LOG_LEVEL_INF);

/* ── API config ── */
#define AI_HOST   "dashscope.aliyuncs.com"
#define AI_PORT   443
#define AI_URL    "/compatible-mode/v1/chat/completions"
#define AI_MODEL  "qwen3.5-omni-flash"

/* ── Buffer sizes ── */
#define HTTP_RECV_BUF_SIZE    4096
#define SSE_LINE_BUF_SIZE     65536  /* 64KB in PSRAM — audio SSE lines can be very long */
#define WAV_HEADER_SIZE       44

/* Maximum base64-encoded WAV size: (160000 + 44) * 4/3 + overhead */
#define MAX_B64_WAV_SIZE      220000
/* Maximum accumulated response audio base64 */
#define MAX_RSP_AUDIO_B64     512000
/* Maximum response audio PCM at 24kHz (before resample) */
#define MAX_RSP_PCM_24K       (15 * 24000)  /* 15s at 24kHz = 360000 samples */

/* ── PSRAM large buffers (.ext_ram.bss) ── */
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

/* ── SRAM buffers ── */
static uint8_t http_recv_buf[HTTP_RECV_BUF_SIZE];
static size_t  sse_line_len;

/* ── State ── */
static volatile bool button_pressed;
static volatile bool recording_stop;
static size_t rsp_audio_b64_len;
static char   rsp_text[1024];
static size_t rsp_text_len;

/* ── BOOT button GPIO ── */
static const struct gpio_dt_spec boot_btn =
	GPIO_DT_SPEC_GET(DT_NODELABEL(boot_button), gpios);
static struct gpio_callback btn_cb_data;

/* ── WiFi state ── */
static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static K_SEM_DEFINE(wifi_connected_sem, 0, 1);
static K_SEM_DEFINE(ipv4_ready_sem, 0, 1);
static K_SEM_DEFINE(scan_done_sem, 0, 1);

/* ================================================================
 *  Button handler
 * ================================================================ */

static void button_isr(const struct device *dev, struct gpio_callback *cb,
		       uint32_t pins)
{
	int val = gpio_pin_get_dt(&boot_btn);
	if (val == 1) {
		/* Button pressed (active-low, dt spec inverts) */
		button_pressed = true;
	} else {
		/* Button released */
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
	if (ret) {
		LOG_ERR("Button config failed: %d", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&boot_btn, GPIO_INT_EDGE_BOTH);
	if (ret) {
		LOG_ERR("Button interrupt config failed: %d", ret);
		return ret;
	}

	gpio_init_callback(&btn_cb_data, button_isr, BIT(boot_btn.pin));
	gpio_add_callback(boot_btn.port, &btn_cb_data);

	LOG_INF("BOOT button ready (IO0, push-to-talk)");
	return 0;
}

/* ================================================================
 *  WiFi (from cloud_ai)
 * ================================================================ */

static void handle_wifi_event(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
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

	/* Scan first */
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
 *  TLS socket
 * ================================================================ */

static int tls_setup(void)
{
	return tls_credential_add(CA_CERTIFICATE_TAG,
				  TLS_CREDENTIAL_CA_CERTIFICATE,
				  ca_certificate, sizeof(ca_certificate));
}

static int tls_connect(int *sock)
{
	struct zsock_addrinfo hints = {0}, *res = NULL;
	int ret;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	LOG_INF("Resolving %s ...", AI_HOST);
	ret = zsock_getaddrinfo(AI_HOST, "443", &hints, &res);
	if (ret || !res) {
		LOG_ERR("DNS resolve failed: %d", ret);
		return -EHOSTUNREACH;
	}

	*sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (*sock < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}

	sec_tag_t sec_tags[] = { CA_CERTIFICATE_TAG };
	zsock_setsockopt(*sock, SOL_TLS, TLS_SEC_TAG_LIST,
			 sec_tags, sizeof(sec_tags));
	zsock_setsockopt(*sock, SOL_TLS, TLS_HOSTNAME,
			 TLS_PEER_HOSTNAME, sizeof(TLS_PEER_HOSTNAME));

	LOG_INF("TLS connecting ...");
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
 *  WAV header builder
 * ================================================================ */

static void build_wav_header(uint8_t *hdr, int sample_rate,
			     int bits, int channels, int data_size)
{
	int byte_rate = sample_rate * channels * bits / 8;
	int block_align = channels * bits / 8;
	int file_size = data_size + 36;  /* total - 8 bytes for RIFF header */

	memcpy(hdr, "RIFF", 4);
	hdr[4] = file_size & 0xFF;
	hdr[5] = (file_size >> 8) & 0xFF;
	hdr[6] = (file_size >> 16) & 0xFF;
	hdr[7] = (file_size >> 24) & 0xFF;
	memcpy(hdr + 8, "WAVE", 4);
	memcpy(hdr + 12, "fmt ", 4);
	hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;  /* subchunk size */
	hdr[20] = 1; hdr[21] = 0;  /* PCM format */
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
 *  SSE streaming HTTP request via raw TLS socket
 * ================================================================ */

/* Send all bytes through TLS socket */
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

/* Extract a JSON string value after a key.
 * Finds "key":"value" and copies value to out (up to max_len).
 * Returns length of value, or -1 if not found. */
static int json_extract_string(const char *json, const char *key,
			       char *out, size_t max_len)
{
	/* Build pattern: "key":" */
	char pattern[64];
	int plen = snprintk(pattern, sizeof(pattern), "\"%s\":\"", key);
	if (plen <= 0) return -1;

	const char *p = strstr(json, pattern);
	if (!p) {
		/* Try with space: "key": " */
		plen = snprintk(pattern, sizeof(pattern), "\"%s\": \"", key);
		p = strstr(json, pattern);
		if (!p) return -1;
	}

	p += plen;
	size_t i = 0;
	while (*p && *p != '"' && i < max_len - 1) {
		if (*p == '\\' && *(p + 1)) {
			p++;  /* skip escape */
		}
		out[i++] = *p++;
	}
	out[i] = '\0';
	return (int)i;
}

/* ── Streaming playback mode ── */
static bool stream_mode;          /* true: decode+play audio per-chunk */
static bool stream_started;       /* audio_stream_start() called */

/* Process one SSE data line (after "data: " prefix) */
static void process_sse_event(const char *json_str)
{
	if (strncmp(json_str, "[DONE]", 6) == 0) {
		LOG_INF("SSE stream complete");
		return;
	}

	/* Extract text content: "content":"..." */
	char text_chunk[256];
	int tlen = json_extract_string(json_str, "content", text_chunk, sizeof(text_chunk));
	if (tlen > 0) {
		if (tlen > 0 && rsp_text_len + tlen < sizeof(rsp_text) - 1) {
			memcpy(rsp_text + rsp_text_len, text_chunk, tlen);
			rsp_text_len += tlen;
			rsp_text[rsp_text_len] = '\0';
		}
	}

	/* Extract audio data: look for "data":"<base64>" within audio context */
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

	/* Find end of base64 string */
	const char *end = dp;
	while (*end && *end != '"') end++;
	size_t b64_len = end - dp;
	if (b64_len == 0) return;

	if (stream_mode) {
		/* Streaming: decode this chunk immediately and feed to player */

		/* Start stream on first audio chunk */
		if (!stream_started) {
			audio_stream_start();
			stream_started = true;
			LOG_INF("Streaming playback initiated");
		}

		/* Decode base64 → 24kHz PCM (into rsp_pcm_24k temp buffer) */
		int decoded = base64_decode(dp, b64_len,
					    (uint8_t *)rsp_pcm_24k,
					    MAX_RSP_PCM_24K * sizeof(int16_t));
		if (decoded <= 0) return;

		int samples_24k = decoded / sizeof(int16_t);

		/* Resample 24kHz → 16kHz (output to rsp_audio_b64 as temp) */
		int16_t *temp_16k = (int16_t *)rsp_audio_b64;
		int max_16k = MAX_RSP_AUDIO_B64 / sizeof(int16_t);
		int samples_16k = audio_resample_24k_to_16k(
			rsp_pcm_24k, samples_24k, temp_16k, max_16k);

		/* Feed to streaming player */
		audio_stream_feed(temp_16k, samples_16k);
		rsp_audio_b64_len += b64_len;  /* track total for logging */
	} else {
		/* Non-streaming: accumulate base64 for batch decode later */
		while (dp < end && rsp_audio_b64_len < MAX_RSP_AUDIO_B64 - 1) {
			rsp_audio_b64[rsp_audio_b64_len++] = *dp++;
		}
	}
}

/* Feed raw bytes from HTTP response into SSE line parser */
static void sse_feed(const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char c = data[i];

		if (c == '\n') {
			/* End of line — process if non-empty */
			if (sse_line_len > 0) {
				sse_line_buf[sse_line_len] = '\0';

				if (strncmp(sse_line_buf, "data: ", 6) == 0) {
					process_sse_event(sse_line_buf + 6);
				}
				/* Ignore other SSE fields (id:, event:, etc.) */

				sse_line_len = 0;
			}
			/* Empty line = SSE event boundary (already processed) */
		} else if (c == '\r') {
			/* Skip CR */
		} else {
			if (sse_line_len < SSE_LINE_BUF_SIZE - 1) {
				sse_line_buf[sse_line_len++] = c;
			}
			/* Silently drop chars beyond buffer — should not happen with 64KB */
		}
	}
}

/* Parse HTTP chunked transfer encoding.
 * Returns pointer to body start and sets body_len.
 * For simplicity, we skip chunk headers and just feed body data to SSE. */
/* Receive exactly n bytes from socket. Returns 0 on success. */
static int sock_recv_exact(int sock, uint8_t *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		int ret = zsock_recv(sock, buf + got, n - got, 0);
		if (ret <= 0) return -EIO;
		got += ret;
	}
	return 0;
}

/* ── Buffered socket reader for chunk decoding ── */
static uint8_t  sock_buf[4096];
static size_t   sock_buf_len;
static size_t   sock_buf_pos;
static bool     sse_done;

/* Read one byte from buffered socket. Returns -1 on EOF/error. */
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

/* Read a line (until \n) from buffered socket into buf. Returns length. */
static int sock_read_line(int sock, char *buf, size_t max)
{
	size_t i = 0;
	while (i < max - 1) {
		int c = sock_read_byte(sock);
		if (c < 0) return -1;
		if (c == '\n') break;
		if (c != '\r') {
			buf[i++] = (char)c;
		}
	}
	buf[i] = '\0';
	return (int)i;
}

/* Read exactly n bytes from buffered socket. */
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

static int receive_sse_stream(int sock)
{
	/* Initialize buffered reader */
	sock_buf_len = 0;
	sock_buf_pos = 0;
	sse_done = false;

	/* First, receive HTTP header line by line */
	char line[512];
	bool is_chunked = false;
	int http_status = 0;

	/* Status line */
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

	/* Read headers until empty line */
	while (1) {
		int len = sock_read_line(sock, line, sizeof(line));
		if (len < 0) {
			LOG_ERR("Failed to read header");
			return -EIO;
		}
		if (len == 0) break;  /* Empty line = end of headers */

		if (strstr(line, "chunked")) {
			is_chunked = true;
		}
	}

	LOG_INF("SSE stream started (chunked=%d)", is_chunked);

	/* Reset SSE state */
	sse_line_len = 0;
	rsp_audio_b64_len = 0;
	rsp_text_len = 0;
	rsp_text[0] = '\0';

	/* Read body: chunked transfer decoding */
	if (is_chunked) {
		while (!sse_done) {
			/* Read chunk size line (hex), skip empty lines */
			char chunk_hdr[32];
			int hlen;
			do {
				hlen = sock_read_line(sock, chunk_hdr, sizeof(chunk_hdr));
				if (hlen < 0) {
					LOG_ERR("Chunk header read failed");
					goto done;
				}
			} while (hlen == 0);

			LOG_INF("Chunk hdr [%d]: '%s'", hlen, chunk_hdr);

			/* Parse hex chunk size */
			char *endp;
			unsigned long chunk_size = strtoul(chunk_hdr, &endp, 16);
			if (endp == chunk_hdr) {
				/* Not a valid hex number — might be trailing data, skip */
				LOG_WRN("Invalid chunk header: '%.16s'", chunk_hdr);
				continue;
			}
			if (chunk_size == 0) {
				LOG_INF("Chunked transfer complete (0-length chunk)");
				break;
			}

			/* Read chunk data and feed to SSE parser */
			size_t remaining = chunk_size;
			while (remaining > 0) {
				size_t to_read = remaining;
				if (to_read > sizeof(http_recv_buf) - 1) {
					to_read = sizeof(http_recv_buf) - 1;
				}
				if (sock_read_n(sock, http_recv_buf, to_read) < 0) {
					LOG_ERR("Chunk data read failed");
					sse_done = true;
					break;
				}
				sse_feed((char *)http_recv_buf, to_read);

				/* Check for [DONE] */
				http_recv_buf[to_read] = '\0';
				if (strstr((char *)http_recv_buf, "[DONE]")) {
					sse_done = true;
				}

				remaining -= to_read;
			}

			/* Consume trailing \r\n after chunk data */
			char crlf[2];
			sock_read_n(sock, (uint8_t *)crlf, 2);
		}
	} else {
		/* Non-chunked: read until connection closes */
		while (!sse_done) {
			int c = sock_read_byte(sock);
			if (c < 0) break;
			char ch = (char)c;
			sse_feed(&ch, 1);
		}
	}

done:
	LOG_INF("SSE parse complete: text=%zu bytes, audio_b64=%zu bytes",
		rsp_text_len, rsp_audio_b64_len);

	return 0;
}

/* Build and send the streaming HTTP POST request with audio */
static int send_voice_request(int sock, const char *b64_audio, size_t b64_len)
{
	/* Build JSON body.
	 * Because the body is very large (200KB+), we send it in parts:
	 * 1. JSON prefix (before base64 audio)
	 * 2. base64 audio data
	 * 3. JSON suffix (after base64 audio)
	 */
	static const char json_prefix[] =
		"{\"model\":\"" AI_MODEL "\","
		"\"stream\":true,"
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

	/* Build HTTP request header */
	char http_header[512];
	int hlen = snprintk(http_header, sizeof(http_header),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Type: application/json\r\n"
		"Authorization: Bearer %s\r\n"
		"Content-Length: %zu\r\n"
		"\r\n",
		AI_URL, AI_HOST, AI_API_KEY, body_len);

	if (hlen <= 0) {
		LOG_ERR("HTTP header too large");
		return -ENOMEM;
	}

	LOG_INF("Sending request: header=%d body=%zu (audio_b64=%zu)",
		hlen, body_len, b64_len);

	/* Send HTTP header */
	int ret = sock_send_all(sock, http_header, hlen);
	if (ret) return ret;

	/* Send JSON prefix */
	ret = sock_send_all(sock, json_prefix, strlen(json_prefix));
	if (ret) return ret;

	/* Send base64 audio in chunks to avoid stack issues */
	size_t sent = 0;
	while (sent < b64_len) {
		size_t chunk = b64_len - sent;
		if (chunk > 4096) chunk = 4096;
		ret = sock_send_all(sock, b64_audio + sent, chunk);
		if (ret) return ret;
		sent += chunk;
	}

	/* Send JSON suffix */
	ret = sock_send_all(sock, json_suffix, strlen(json_suffix));
	if (ret) return ret;

	LOG_INF("Request sent, waiting for SSE response...");

	/* Receive and parse SSE stream */
	return receive_sse_stream(sock);
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(void)
{
	int ret;

	LOG_INF("=== Voice Assistant (qwen3.5-omni-flash) ===");

	/* Step 1: Init audio codecs */
	ret = audio_codec_init();
	if (ret) {
		LOG_ERR("Audio init failed: %d", ret);
		return ret;
	}

	/* Step 2: Init BOOT button */
	ret = button_init();
	if (ret) {
		LOG_ERR("Button init failed: %d", ret);
		return ret;
	}

	/* Step 3: Connect WiFi */
	ret = wifi_connect();
	if (ret) {
		LOG_ERR("WiFi failed: %d", ret);
		return ret;
	}

	/* Step 4: Register TLS CA cert */
	ret = tls_setup();
	if (ret) {
		LOG_ERR("TLS setup failed: %d", ret);
		return ret;
	}

	LOG_INF(">>> Testing SSE stream with text-only request... <<<");

	/* Text-only streaming test (no audio) to verify SSE parsing works */
	{
		int sock = -1;
		ret = tls_connect(&sock);
		if (ret) {
			LOG_ERR("TLS connect for test failed: %d", ret);
		} else {
			static const char test_body[] =
				"{\"model\":\"" AI_MODEL "\","
				"\"stream\":true,"
				"\"messages\":["
				"{\"role\":\"user\",\"content\":\"Say hello in one sentence.\"}"
				"]}";

			char hdr[512];
			int hlen = snprintk(hdr, sizeof(hdr),
				"POST %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Content-Type: application/json\r\n"
				"Authorization: Bearer %s\r\n"
				"Content-Length: %d\r\n"
				"\r\n",
				AI_URL, AI_HOST, AI_API_KEY, (int)strlen(test_body));

			sock_send_all(sock, hdr, hlen);
			sock_send_all(sock, test_body, strlen(test_body));
			LOG_INF("Test request sent, parsing SSE...");
			receive_sse_stream(sock);
			zsock_close(sock);

			if (rsp_text_len > 0) {
				printk("\n=== SSE Test Response ===\n%s\n=========================\n",
				       rsp_text);
			} else {
				LOG_WRN("SSE test: no text response received");
			}
		}
	}

	LOG_INF(">>> Ready! Press and hold BOOT button to speak <<<");

	/* Main loop: push-to-talk */
	while (1) {
		/* Wait for button press */
		button_pressed = false;
		recording_stop = false;

		while (!button_pressed) {
			k_msleep(50);
		}

		LOG_INF("--- Button pressed, recording... ---");

		/* Record audio */
		recording_stop = false;
		int num_samples = audio_record(record_pcm, MAX_RECORD_SAMPLES,
					       &recording_stop);
		if (num_samples <= 0) {
			LOG_ERR("Recording failed: %d", num_samples);
			continue;
		}

		LOG_INF("Recorded %d samples (%.1fs)",
			num_samples, (float)num_samples / RECORD_SAMPLE_RATE);

		/* Build WAV in temporary buffer (reuse part of PSRAM) */
		int pcm_bytes = num_samples * sizeof(int16_t);
		uint8_t wav_header[WAV_HEADER_SIZE];
		build_wav_header(wav_header, RECORD_SAMPLE_RATE, 16, 1, pcm_bytes);

		/* Base64 encode WAV (header + PCM) */
		/* We need a temp buffer for the complete WAV file.
		 * Use rsp_pcm_24k as temp since it's not needed yet. */
		uint8_t *wav_tmp = (uint8_t *)rsp_pcm_24k;
		int wav_total = WAV_HEADER_SIZE + pcm_bytes;
		if (wav_total > (int)sizeof(rsp_pcm_24k) * (int)sizeof(int16_t)) {
			LOG_ERR("WAV too large for temp buffer");
			continue;
		}
		memcpy(wav_tmp, wav_header, WAV_HEADER_SIZE);
		memcpy(wav_tmp + WAV_HEADER_SIZE, record_pcm, pcm_bytes);

		size_t b64_len = base64_encode(wav_tmp, wav_total,
					       b64_wav_buf, MAX_B64_WAV_SIZE);
		if (b64_len == 0) {
			LOG_ERR("Base64 encode failed");
			continue;
		}
		LOG_INF("WAV encoded: %d bytes → %zu base64 chars", wav_total, b64_len);

		/* TLS connect */
		int sock = -1;
		ret = tls_connect(&sock);
		if (ret) {
			LOG_ERR("TLS connect failed: %d", ret);
			continue;
		}

		/* Enable streaming playback mode */
		stream_mode = true;
		stream_started = false;

		/* Send voice request and receive SSE response
		 * Audio is decoded and played in real-time via streaming */
		ret = send_voice_request(sock, b64_wav_buf, b64_len);
		zsock_close(sock);

		/* Stop streaming playback (waits for remaining audio to finish) */
		if (stream_started) {
			audio_stream_stop();
		}
		stream_mode = false;

		if (ret) {
			LOG_ERR("Voice request failed: %d", ret);
			continue;
		}

		/* Print text response */
		if (rsp_text_len > 0) {
			printk("\n=== AI Response (text) ===\n%s\n=========================\n",
			       rsp_text);
		}

		if (rsp_audio_b64_len > 0) {
			LOG_INF("Audio streamed: %zu base64 chars total",
				rsp_audio_b64_len);
		} else {
			LOG_WRN("No audio in response");
		}

		LOG_INF(">>> Ready for next question <<<");
	}

	return 0;
}
