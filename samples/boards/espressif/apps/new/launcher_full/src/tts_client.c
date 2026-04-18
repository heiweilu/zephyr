/*
 * tts_client.c — CosyVoice WebSocket TTS over TLS for face_recognize
 *
 * Adapted from launcher/ai_service.c (cosyvoice_tts + ws_* helpers).
 *
 * Path : wss://dashscope.aliyuncs.com/api-ws/v1/inference
 * Model: cosyvoice-v3-flash
 * Voice: longanyang
 * PCM  : 16-bit 16 kHz mono → audio_play()
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

#include "tts_client.h"
#include "audio.h"
#include "secrets.h"

LOG_MODULE_REGISTER(tts_client, LOG_LEVEL_INF);

#define AI_HOST   "dashscope.aliyuncs.com"
#define AI_PORT   "443"
#define WS_PATH   "/api-ws/v1/inference"
#define CA_TAG    1

#define TTS_MODEL "cosyvoice-v3-flash"
#define TTS_VOICE "longanyang"

#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

#define SSE_LINE_BUF_SIZE  (32 * 1024)
#define MAX_RSP_AUDIO_B64  (256 * 1024)
#define MAX_RSP_PCM_16K    (15 * 16000)   /* 15 s @ 16 kHz mono int16 */
#define HTTP_RECV_BUF_SIZE 4096

/* PSRAM buffers */
static char    sse_line_buf[SSE_LINE_BUF_SIZE]    __attribute__((section(".ext_ram.bss")));
static char    rsp_text_esc[MAX_RSP_AUDIO_B64]    __attribute__((section(".ext_ram.bss")));
static int16_t rsp_pcm_16k[MAX_RSP_PCM_16K]       __attribute__((section(".ext_ram.bss")));
static uint8_t http_recv_buf[HTTP_RECV_BUF_SIZE]  __attribute__((section(".ext_ram.bss")));
static uint8_t ws_tx_mask_buf[4096]               __attribute__((section(".ext_ram.bss")));
static uint8_t sock_buf[4096]                     __attribute__((section(".ext_ram.bss")));
static size_t  sock_buf_len, sock_buf_pos;

struct ws_frame { uint8_t opcode; size_t payload_len; };

/* ── socket helpers ─────────────────────────────────────────────── */
static int sock_send_all(int sock, const char *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		int ret = zsock_send(sock, data + sent, len - sent, 0);
		if (ret < 0) {
			LOG_ERR("send failed: %d", -errno);
			return -errno;
		}
		sent += ret;
	}
	return 0;
}

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

/* ── TLS connect (mirrors ai_client.c) ──────────────────────────── */
static int tls_connect(void)
{
	struct zsock_addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_family = AF_INET };
	struct zsock_addrinfo *res = NULL;
	int ret = zsock_getaddrinfo(AI_HOST, AI_PORT, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_ERR("getaddrinfo failed: %d", ret);
		return -EHOSTUNREACH;
	}

	int sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (sock < 0) { zsock_freeaddrinfo(res); LOG_ERR("socket: %d", -errno); return -errno; }

	sec_tag_t tags[] = { CA_TAG };
	if (zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags)) < 0) {
		LOG_ERR("TLS_SEC_TAG_LIST: %d", -errno); goto err;
	}
	if (zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, AI_HOST, sizeof(AI_HOST)) < 0) {
		LOG_ERR("TLS_HOSTNAME: %d", -errno); goto err;
	}
	int verify = TLS_PEER_VERIFY_REQUIRED;
	if (zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify)) < 0) {
		LOG_WRN("TLS_PEER_VERIFY: %d", -errno);
	}

	if (zsock_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
		LOG_ERR("connect: %d", -errno); goto err;
	}
	zsock_freeaddrinfo(res);
	LOG_INF("TLS connected (TTS)");
	return sock;
err:
	zsock_close(sock);
	zsock_freeaddrinfo(res);
	return -EIO;
}

/* ── WebSocket helpers ───────────────────────────────────────────── */
static int ws_send_frame(int sock, uint8_t opcode, const uint8_t *data, size_t len)
{
	uint8_t header[14];
	int hlen = 0;
	uint32_t mask_val = k_cycle_get_32();
	uint8_t mask[4];
	memcpy(mask, &mask_val, 4);

	header[0] = 0x80 | opcode;
	if (len < 126) {
		header[1] = 0x80 | (uint8_t)len; hlen = 2;
	} else if (len < 65536) {
		header[1] = 0x80 | 126;
		header[2] = (len >> 8) & 0xFF;
		header[3] = len & 0xFF; hlen = 4;
	} else {
		header[1] = 0x80 | 127;
		memset(header + 2, 0, 4);
		header[6] = (len >> 24) & 0xFF;
		header[7] = (len >> 16) & 0xFF;
		header[8] = (len >> 8) & 0xFF;
		header[9] = len & 0xFF; hlen = 10;
	}
	memcpy(header + hlen, mask, 4);
	hlen += 4;

	int ret = sock_send_all(sock, (const char *)header, hlen);
	if (ret) return ret;

	size_t sent = 0;
	while (sent < len) {
		size_t chunk = len - sent;
		if (chunk > sizeof(ws_tx_mask_buf)) chunk = sizeof(ws_tx_mask_buf);
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
		uint8_t mk[4];
		if (sock_read_n(sock, mk, 4) < 0) return -1;
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
		WS_PATH, AI_HOST, BAILIAN_API_KEY);

	sock_buf_len = sock_buf_pos = 0;

	int ret = sock_send_all(sock, req, rlen);
	if (ret) return ret;

	char line[256];
	if (sock_read_line(sock, line, sizeof(line)) < 0) return -EIO;
	if (strncmp(line, "HTTP/1.1 101", 12) != 0) {
		LOG_ERR("WS upgrade failed: %s", line);
		return -EIO;
	}
	while (1) {
		int len = sock_read_line(sock, line, sizeof(line));
		if (len < 0) return -EIO;
		if (len == 0) break;
	}
	LOG_INF("WS upgraded");
	return 0;
}

static size_t json_escape_str(const char *src, char *dst, size_t dst_max)
{
	size_t si = 0, di = 0;
	while (src[si] && di < dst_max - 2) {
		char c = src[si];
		if (c == '"' || c == '\\') {
			if (di + 2 > dst_max - 1) break;
			dst[di++] = '\\'; dst[di++] = c;
		} else if (c == '\n') {
			if (di + 2 > dst_max - 1) break;
			dst[di++] = '\\'; dst[di++] = 'n';
		} else if (c == '\r') {
			/* skip */
		} else if (c == '\t') {
			if (di + 2 > dst_max - 1) break;
			dst[di++] = '\\'; dst[di++] = 't';
		} else {
			dst[di++] = c;
		}
		si++;
	}
	dst[di] = '\0';
	return di;
}

/* ── public API ─────────────────────────────────────────────────── */
int tts_client_init(void) { return 0; }

int tts_client_speak(const char *text)
{
	int ret;
	int64_t t0 = k_uptime_get();

	int sock = tls_connect();
	if (sock < 0) return sock;

	LOG_INF("TTS: speaking %zu bytes via cosyvoice", strlen(text));

	ret = ws_upgrade(sock);
	if (ret) goto cleanup;

	uint32_t t1 = k_cycle_get_32();
	uint32_t t2 = t1 ^ 0xDEADBEEF;
	char task_id[40];
	snprintk(task_id, sizeof(task_id), "%08x-0000-4000-8000-%08x%04x",
		 t1, t2, (t1 >> 16) & 0xFFFF);

	int clen = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
		"{\"header\":{\"action\":\"run-task\","
		"\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
		"\"payload\":{\"task_group\":\"audio\",\"task\":\"tts\","
		"\"function\":\"SpeechSynthesizer\",\"model\":\"" TTS_MODEL "\","
		"\"parameters\":{\"text_type\":\"PlainText\","
		"\"voice\":\"" TTS_VOICE "\",\"format\":\"pcm\","
		"\"sample_rate\":16000,\"volume\":50,\"rate\":1.0,\"pitch\":1.0},"
		"\"input\":{}}}", task_id);
	ret = ws_send_frame(sock, WS_OP_TEXT, (const uint8_t *)sse_line_buf, clen);
	if (ret) goto cleanup;
	LOG_INF("TTS: run-task sent");

	/* Wait for task-started */
	{
		struct ws_frame f; char jb[1024]; bool started = false;
		while (!started) {
			if (ws_recv_frame_header(sock, &f) < 0) { ret = -EIO; goto cleanup; }
			if (f.opcode == WS_OP_TEXT) {
				size_t rd = f.payload_len; if (rd >= sizeof(jb)) rd = sizeof(jb) - 1;
				if (sock_read_n(sock, (uint8_t *)jb, rd) < 0) { ret = -EIO; goto cleanup; }
				jb[rd] = '\0';
				if (f.payload_len > rd) ws_skip_payload(sock, f.payload_len - rd);
				if (strstr(jb, "task-started")) { started = true; LOG_INF("TTS: task started"); }
				else if (strstr(jb, "task-failed")) {
					LOG_ERR("TTS: task-failed: %s", jb); ret = -EIO; goto cleanup;
				}
			} else { ws_skip_payload(sock, f.payload_len); }
		}
	}

	/* continue-task with text */
	json_escape_str(text, rsp_text_esc, MAX_RSP_AUDIO_B64);
	clen = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
		"{\"header\":{\"action\":\"continue-task\","
		"\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
		"\"payload\":{\"input\":{\"text\":\"%s\"}}}",
		task_id, rsp_text_esc);
	ret = ws_send_frame(sock, WS_OP_TEXT, (const uint8_t *)sse_line_buf, clen);
	if (ret) goto cleanup;
	LOG_INF("TTS: continue-task sent (%d bytes)", clen);

	/* finish-task */
	clen = snprintk(sse_line_buf, SSE_LINE_BUF_SIZE,
		"{\"header\":{\"action\":\"finish-task\","
		"\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
		"\"payload\":{\"input\":{}}}", task_id);
	ret = ws_send_frame(sock, WS_OP_TEXT, (const uint8_t *)sse_line_buf, clen);
	if (ret) goto cleanup;
	LOG_INF("TTS: finish-task sent");

	/* Collect PCM (16 kHz mono int16) until task-finished, then play */
	{
		struct ws_frame f; char jb[1024];
		int total = 0, chunks = 0; bool done = false;

		while (!done) {
			if (ws_recv_frame_header(sock, &f) < 0) break;
			if (f.opcode == WS_OP_BINARY) {
				size_t remaining = f.payload_len;
				int chunk_samples = 0;
				while (remaining > 0) {
					size_t avail = (size_t)(MAX_RSP_PCM_16K - total) * sizeof(int16_t);
					size_t to_read = remaining;
					if (to_read > avail) to_read = avail;
					if (to_read == 0) { ws_skip_payload(sock, remaining); break; }
					if (sock_read_n(sock, (uint8_t *)&rsp_pcm_16k[total], to_read) < 0) {
						done = true; break;
					}
					int got = (int)(to_read / sizeof(int16_t));
					total += got;
					chunk_samples += got;
					remaining -= to_read;
				}
				if (chunk_samples > 0) {
					chunks++;
					LOG_INF("TTS: chunk %d +%d (total %d) @T+%lldms",
						chunks, chunk_samples, total, k_uptime_get() - t0);
				}
			} else if (f.opcode == WS_OP_TEXT) {
				size_t rd = f.payload_len; if (rd >= sizeof(jb)) rd = sizeof(jb) - 1;
				if (sock_read_n(sock, (uint8_t *)jb, rd) < 0) break;
				jb[rd] = '\0';
				if (f.payload_len > rd) ws_skip_payload(sock, f.payload_len - rd);
				if (strstr(jb, "task-finished")) {
					done = true;
					LOG_INF("TTS: finished, %d chunks, %d samples (%d ms) @T+%lldms",
						chunks, total, total * 1000 / 16000,
						k_uptime_get() - t0);
				} else if (strstr(jb, "task-failed")) {
					LOG_ERR("TTS: task-failed: %s", jb);
					done = true;
				}
			} else if (f.opcode == WS_OP_PING) {
				ws_skip_payload(sock, f.payload_len);
				ws_send_frame(sock, WS_OP_PONG, NULL, 0);
			} else if (f.opcode == WS_OP_CLOSE) {
				LOG_WRN("TTS: server closed"); done = true;
			} else { ws_skip_payload(sock, f.payload_len); }
		}

		if (total > 0) {
			LOG_INF("TTS: playing %d samples (%d ms)", total, total * 1000 / 16000);
			audio_play(rsp_pcm_16k, total);
		} else {
			LOG_WRN("TTS: no audio received");
			ret = -EIO;
		}
	}

cleanup:
	zsock_close(sock);
	return ret;
}
