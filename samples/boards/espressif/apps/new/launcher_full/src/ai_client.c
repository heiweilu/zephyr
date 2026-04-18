/*
 * ai_client.c — POST JPEG to Aliyun bailian (qwen-vl-plus) via TLS.
 *
 * Phase 1b-2: build OpenAI-compatible chat-completions request with
 * data:image/jpeg;base64 payload, send via mbedTLS sockets, dump the
 * raw HTTP response to UART. JSON parsing is deferred to phase 1b-3.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/dns_resolve.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai_client.h"
#include "ca_cert.h"
#include "secrets.h"

LOG_MODULE_REGISTER(ai_client, LOG_LEVEL_INF);

#define AI_HOST       "dashscope.aliyuncs.com"
#define AI_PORT       "443"
#define AI_PATH       "/compatible-mode/v1/chat/completions"
#define AI_MODEL      "qwen-vl-plus"
#define AI_PROMPT     "请用一句中文描述图片里的人脸或主要内容。"

#define CA_TAG        1

/* PSRAM-backed scratch buffers (avoid blowing up DRAM). */
#define REQ_CAP       (24 * 1024)   /* HTTP headers + JSON + base64 jpeg */
#define RESP_CAP      (4 * 1024)    /* full HTTP response */
#define B64_CAP       (16 * 1024)   /* base64-encoded jpeg, ~4/3 of jpeg_len */

static uint8_t req_buf[REQ_CAP]   __attribute__((section(".ext_ram.bss")));
static uint8_t resp_buf[RESP_CAP] __attribute__((section(".ext_ram.bss")));
static uint8_t b64_buf[B64_CAP]   __attribute__((section(".ext_ram.bss")));

/* ── base64 ─────────────────────────────────────────────────────────── */
static const char b64_tbl[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
	size_t need = ((in_len + 2) / 3) * 4 + 1; /* + NUL */
	if (need > out_cap) {
		return -ENOSPC;
	}

	size_t i, o = 0;
	for (i = 0; i + 3 <= in_len; i += 3) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = b64_tbl[(v >> 18) & 0x3F];
		out[o++] = b64_tbl[(v >> 12) & 0x3F];
		out[o++] = b64_tbl[(v >> 6) & 0x3F];
		out[o++] = b64_tbl[v & 0x3F];
	}
	size_t rem = in_len - i;
	if (rem == 1) {
		uint32_t v = (uint32_t)in[i] << 16;
		out[o++] = b64_tbl[(v >> 18) & 0x3F];
		out[o++] = b64_tbl[(v >> 12) & 0x3F];
		out[o++] = '=';
		out[o++] = '=';
	} else if (rem == 2) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
		out[o++] = b64_tbl[(v >> 18) & 0x3F];
		out[o++] = b64_tbl[(v >> 12) & 0x3F];
		out[o++] = b64_tbl[(v >> 6) & 0x3F];
		out[o++] = '=';
	}
	out[o] = '\0';
	return (int)o;
}

/* ── TLS credential init ────────────────────────────────────────────── */
int ai_client_init(void)
{
	int ret = tls_credential_add(CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				     ca_cert_globalsign_r3,
				     sizeof(ca_cert_globalsign_r3));
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("tls_credential_add failed: %d", ret);
		return ret;
	}
	LOG_INF("CA cert registered (tag=%d, %u bytes)",
		CA_TAG, (unsigned)sizeof(ca_cert_globalsign_r3));
	return 0;
}

/* ── DNS + TCP connect ──────────────────────────────────────────────── */
static int connect_tls(void)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;

	int ret = zsock_getaddrinfo(AI_HOST, AI_PORT, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_ERR("getaddrinfo(%s) failed: %d", AI_HOST, ret);
		return -EHOSTUNREACH;
	}

	int sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	sec_tag_t sec_tag_list[] = { CA_TAG };
	if (zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
			     sec_tag_list, sizeof(sec_tag_list)) < 0) {
		LOG_ERR("TLS_SEC_TAG_LIST failed: %d", -errno);
		zsock_close(sock);
		zsock_freeaddrinfo(res);
		return -errno;
	}
	if (zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
			     AI_HOST, strlen(AI_HOST)) < 0) {
		LOG_ERR("TLS_HOSTNAME failed: %d", -errno);
		zsock_close(sock);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	int verify = TLS_PEER_VERIFY_REQUIRED;
	if (zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
			     &verify, sizeof(verify)) < 0) {
		LOG_WRN("TLS_PEER_VERIFY failed: %d", -errno);
	}

	LOG_INF("connecting to %s:%s ...", AI_HOST, AI_PORT);
	if (zsock_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
		LOG_ERR("connect() failed: %d", -errno);
		zsock_close(sock);
		zsock_freeaddrinfo(res);
		return -errno;
	}
	zsock_freeaddrinfo(res);
	LOG_INF("TLS connected");
	return sock;
}

/* ── send all ───────────────────────────────────────────────────────── */
static int send_all(int sock, const uint8_t *buf, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = zsock_send(sock, buf + sent, len - sent, 0);
		if (n < 0) {
			LOG_ERR("send() failed at %zu/%zu: %d", sent, len, -errno);
			return -errno;
		}
		sent += n;
	}
	return 0;
}

/* ── public API ─────────────────────────────────────────────────────── */
int ai_client_post_jpeg(const uint8_t *jpeg, size_t jpeg_len)
{
	int b64_len = b64_encode(jpeg, jpeg_len, (char *)b64_buf, B64_CAP);
	if (b64_len < 0) {
		LOG_ERR("base64 encode failed: %d", b64_len);
		return b64_len;
	}
	LOG_INF("base64: %d bytes (jpeg=%zu)", b64_len, jpeg_len);

	/* Build JSON body in tail of req_buf so we know its length first. */
	char *body = (char *)req_buf + 4096;
	size_t body_cap = REQ_CAP - 4096;
	int body_len = snprintf(body, body_cap,
		"{\"model\":\"%s\","
		"\"messages\":[{\"role\":\"user\",\"content\":["
		"{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,",
		AI_MODEL);
	if (body_len < 0 || (size_t)body_len >= body_cap) {
		return -ENOSPC;
	}
	if ((size_t)body_len + b64_len + 256 >= body_cap) {
		LOG_ERR("body buffer too small");
		return -ENOSPC;
	}
	memcpy(body + body_len, b64_buf, b64_len);
	body_len += b64_len;
	int tail = snprintf(body + body_len, body_cap - body_len,
		"\"}},{\"type\":\"text\",\"text\":\"%s\"}]}]}", AI_PROMPT);
	if (tail < 0) {
		return -ENOSPC;
	}
	body_len += tail;

	/* Build HTTP header at start of req_buf. */
	int hdr_len = snprintf((char *)req_buf, 4096,
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Authorization: Bearer %s\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n",
		AI_PATH, AI_HOST, BAILIAN_API_KEY, body_len);
	if (hdr_len < 0 || hdr_len >= 4096) {
		return -ENOSPC;
	}

	int sock = connect_tls();
	if (sock < 0) {
		return sock;
	}

	int ret = send_all(sock, req_buf, hdr_len);
	if (ret == 0) {
		ret = send_all(sock, (const uint8_t *)body, body_len);
	}
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}
	LOG_INF("request sent (header=%d, body=%d)", hdr_len, body_len);

	/* Read full response into resp_buf, then dump. */
	size_t total = 0;
	while (total < RESP_CAP - 1) {
		ssize_t n = zsock_recv(sock, resp_buf + total, RESP_CAP - 1 - total, 0);
		if (n == 0) {
			break; /* EOF */
		}
		if (n < 0) {
			LOG_ERR("recv() failed: %d", -errno);
			ret = -errno;
			break;
		}
		total += n;
	}
	zsock_close(sock);
	resp_buf[total] = '\0';

	printk("AI_RESP_BEGIN size=%zu\n", total);
	/* Dump as text in chunks; printk handles \r\n fine. */
	const size_t chunk = 256;
	for (size_t i = 0; i < total; i += chunk) {
		size_t n = (total - i < chunk) ? (total - i) : chunk;
		uint8_t save = resp_buf[i + n];
		resp_buf[i + n] = '\0';
		printk("%s", (char *)(resp_buf + i));
		resp_buf[i + n] = save;
	}
	printk("\nAI_RESP_END\n");

	return ret;
}

/* Extract choices[0].message.content from resp_buf (last response). */
int ai_client_get_caption(char *out, size_t out_max)
{
	if (!out || out_max == 0) return 0;
	out[0] = '\0';

	const char *p = strstr((const char *)resp_buf, "\"content\":\"");
	if (!p) return 0;
	p += 11; /* skip "content":" */

	size_t i = 0;
	while (*p && i + 1 < out_max) {
		char c = *p++;
		if (c == '\\' && *p) {
			char esc = *p++;
			switch (esc) {
			case 'n':  out[i++] = '\n'; break;
			case 't':  out[i++] = '\t'; break;
			case '"':  out[i++] = '"';  break;
			case '\\': out[i++] = '\\'; break;
			case '/':  out[i++] = '/';  break;
			default:   out[i++] = esc;  break;
			}
		} else if (c == '"') {
			break; /* end of string */
		} else {
			out[i++] = c;
		}
	}
	out[i] = '\0';
	return (int)i;
}
