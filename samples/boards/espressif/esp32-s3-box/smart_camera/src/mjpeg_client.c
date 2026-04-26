/*
 * smart_camera MJPEG client.
 *
 * Establishes a long-lived TCP connection to the PC server, issues
 *   GET <path> HTTP/1.0
 * and parses a multipart/x-mixed-replace response, invoking the user callback
 * for every full JPEG frame.
 *
 * We use a simple state machine:
 *   1. read until we have all response headers (\r\n\r\n)
 *   2. extract boundary token from Content-Type
 *   3. loop: search for boundary -> read part headers -> read Content-Length
 *      bytes of JPEG -> deliver frame -> repeat
 *
 * On any error we close the socket and reconnect after a short backoff.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <zephyr/net/socket.h>
#include <zephyr/net/dns_resolve.h>

#include "mjpeg_client.h"

LOG_MODULE_REGISTER(mjpeg_client, LOG_LEVEL_INF);

#define RX_BUF_SIZE        8192
#define MAX_FRAME_SIZE     65536  /* 320x240 JPEG @ Q=70 is well under 30 KB */
#define RECONNECT_BACKOFF_MS 2000

#define CLIENT_THREAD_STACK 4096
#define CLIENT_THREAD_PRIO  6

static K_THREAD_STACK_DEFINE(client_stack, CLIENT_THREAD_STACK);
static struct k_thread client_thread;

struct client_cfg {
	const char *host;
	uint16_t port;
	const char *path;
	mjpeg_frame_cb_t cb;
	void *user;
};

static struct client_cfg cfg;

/* Re-used frame buffer in PSRAM. */
static uint8_t frame_buf[MAX_FRAME_SIZE]
    __attribute__((section(".ext_ram.bss")));

/* ------- low-level helpers ------- */

static int tcp_connect(const char *host, uint16_t port)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	char port_str[8];
	int sock, ret;

	snprintk(port_str, sizeof(port_str), "%u", port);
	ret = zsock_getaddrinfo(host, port_str, &hints, &res);
	if (ret || !res) {
		LOG_ERR("DNS %s: %d", host, ret);
		return -EHOSTUNREACH;
	}

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket: %d", errno);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		LOG_ERR("connect %s:%u: %d", host, port, errno);
		zsock_close(sock);
		return -errno;
	}

	/* recv timeout so a silently dropped TCP connection does not hang us
	 * forever. On timeout recv() returns -1 with errno=EAGAIN; the caller
	 * treats it as an error and triggers reconnect.
	 */
	struct zsock_timeval tv = { .tv_sec = 5, .tv_usec = 0 };
	if (zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		LOG_WRN("SO_RCVTIMEO: %d", errno);
	}

	LOG_INF("connected %s:%u", host, port);
	return sock;
}

static int send_get(int sock, const char *host, uint16_t port, const char *path)
{
	char req[256];
	int len = snprintk(req, sizeof(req),
			   "GET %s HTTP/1.0\r\n"
			   "Host: %s:%u\r\n"
			   "User-Agent: smart_camera/1\r\n"
			   "Accept: */*\r\n"
			   "\r\n",
			   path, host, port);
	if (len < 0 || len >= (int)sizeof(req)) {
		return -ENOMEM;
	}
	int sent = 0;
	while (sent < len) {
		int n = zsock_send(sock, req + sent, len - sent, 0);
		if (n <= 0) {
			LOG_ERR("send: %d", errno);
			return -errno;
		}
		sent += n;
	}
	return 0;
}

/* ------- multipart parser ------- */

/* Extract boundary value from a Content-Type header value into out (size sz).
 * Returns 0 on success.
 */
static int extract_boundary(const char *headers, char *out, size_t sz)
{
	const char *p = strstr(headers, "boundary=");
	if (!p) {
		p = strstr(headers, "BOUNDARY=");
	}
	if (!p) {
		return -ENOENT;
	}
	p += strlen("boundary=");
	/* Optional surrounding quotes */
	if (*p == '"') {
		p++;
	}
	const char *end = p;
	while (*end && *end != '"' && *end != '\r' && *end != '\n' &&
	       *end != ';' && *end != ' ') {
		end++;
	}
	size_t blen = end - p;
	if (blen + 3 > sz) {
		return -ENOMEM;
	}
	/* Server sends "--<boundary>" between parts. */
	out[0] = '-';
	out[1] = '-';
	memcpy(out + 2, p, blen);
	out[2 + blen] = '\0';
	return 0;
}

/* Read into dst until len bytes received or socket dies.
 * Returns 0 on success, negative errno on failure.
 */
static int read_exact(int sock, uint8_t *dst, size_t len)
{
	size_t got = 0;
	while (got < len) {
		int n = zsock_recv(sock, dst + got, len - got, 0);
		if (n <= 0) {
			return n == 0 ? -EPIPE : -errno;
		}
		got += n;
	}
	return 0;
}

/* Drop bytes from socket until the bytes ending the buffer == needle. Returns
 * 0 on found, negative on error. The matched needle is consumed.
 */
static int read_until(int sock, const char *needle)
{
	size_t nlen = strlen(needle);
	uint8_t window[16];
	size_t wpos = 0;

	if (nlen >= sizeof(window)) {
		return -ENOMEM;
	}

	while (1) {
		uint8_t b;
		int n = zsock_recv(sock, &b, 1, 0);
		if (n <= 0) {
			return n == 0 ? -EPIPE : -errno;
		}
		if (wpos < nlen) {
			window[wpos++] = b;
		} else {
			memmove(window, window + 1, nlen - 1);
			window[nlen - 1] = b;
		}
		if (wpos == nlen && memcmp(window, needle, nlen) == 0) {
			return 0;
		}
	}
}

/* Parse a part header block (already past the boundary line) up to and
 * including the trailing \r\n\r\n. Returns Content-Length, or -1 if missing.
 */
static int read_part_headers(int sock)
{
	char buf[256];
	size_t pos = 0;
	int content_len = -1;

	/* Read line by line until empty line. */
	while (1) {
		size_t line_start = pos;
		while (1) {
			if (pos >= sizeof(buf) - 1) {
				return -ENOMEM;
			}
			int n = zsock_recv(sock, (uint8_t *)&buf[pos], 1, 0);
			if (n <= 0) {
				return n == 0 ? -EPIPE : -errno;
			}
			pos++;
			if (pos >= 2 && buf[pos - 2] == '\r' && buf[pos - 1] == '\n') {
				break;
			}
		}
		size_t line_len = pos - line_start - 2;
		buf[pos - 2] = '\0';
		if (line_len == 0) {
			break; /* end of part headers */
		}
		if (strncasecmp(&buf[line_start], "Content-Length:", 15) == 0) {
			content_len = atoi(&buf[line_start + 15]);
		}
		buf[pos - 2] = '\r';
	}
	return content_len;
}

/* Read and parse the HTTP response status + headers. Returns 0 on 200 OK and
 * fills boundary[]. */
static int read_response_headers(int sock, char *boundary, size_t bsz)
{
	char hbuf[1024];
	size_t pos = 0;

	while (pos < sizeof(hbuf) - 1) {
		int n = zsock_recv(sock, (uint8_t *)&hbuf[pos], 1, 0);
		if (n <= 0) {
			return n == 0 ? -EPIPE : -errno;
		}
		pos++;
		if (pos >= 4 && hbuf[pos - 4] == '\r' && hbuf[pos - 3] == '\n' &&
		    hbuf[pos - 2] == '\r' && hbuf[pos - 1] == '\n') {
			hbuf[pos] = '\0';
			break;
		}
	}

	/* Status line: "HTTP/1.x 200 ..." */
	int status = 0;
	const char *sp = strchr(hbuf, ' ');
	if (sp) {
		status = atoi(sp + 1);
	}
	if (status != 200) {
		LOG_ERR("server returned %d", status);
		return -EPROTO;
	}

	if (extract_boundary(hbuf, boundary, bsz) < 0) {
		LOG_ERR("no boundary in headers");
		return -EPROTO;
	}
	LOG_INF("boundary=%s", boundary);
	return 0;
}

/* ------- main client loop ------- */

static void client_session(int sock)
{
	char boundary[64];
	uint32_t seq = 0;
	int ret;

	ret = send_get(sock, cfg.host, cfg.port, cfg.path);
	if (ret) {
		return;
	}
	ret = read_response_headers(sock, boundary, sizeof(boundary));
	if (ret) {
		return;
	}

	while (1) {
		ret = read_until(sock, boundary);
		if (ret) {
			LOG_WRN("read_until boundary failed: %d", ret);
			return;
		}
		/* The boundary may be followed by \r\n or "--\r\n" (final). */
		uint8_t suffix[2];
		ret = read_exact(sock, suffix, 2);
		if (ret) {
			return;
		}
		if (suffix[0] == '-' && suffix[1] == '-') {
			LOG_INF("server closed stream");
			return;
		}
		/* expected \r\n already in suffix; loop expects part headers next */

		int clen = read_part_headers(sock);
		if (clen <= 0) {
			LOG_WRN("part missing Content-Length");
			return;
		}
		if (clen > MAX_FRAME_SIZE) {
			LOG_ERR("frame too big: %d > %d", clen, MAX_FRAME_SIZE);
			return;
		}

		ret = read_exact(sock, frame_buf, clen);
		if (ret) {
			LOG_WRN("read frame failed: %d", ret);
			return;
		}

		struct mjpeg_frame f = {
			.data = frame_buf,
			.len  = (size_t)clen,
			.seq  = seq++,
		};
		/* Latency optimisation: peek a full frame's worth (~4KB) - if so
		 * many bytes are already buffered then a complete newer frame is
		 * waiting and the current one is stale; drop it.
		 */
		static uint8_t peekbuf[4096];
		int avail = zsock_recv(sock, peekbuf, sizeof(peekbuf),
				       ZSOCK_MSG_PEEK | ZSOCK_MSG_DONTWAIT);
		bool stale = (avail >= (int)sizeof(peekbuf));
		if (cfg.cb && !stale) {
			cfg.cb(&f, cfg.user);
		}

		/* Trailing \r\n after JPEG body, swallow it.
		 * Use a separate buffer; do NOT reuse boundary[] (it is the
		 * needle for the next read_until and must stay intact).
		 */
		uint8_t crlf[2];
		ret = read_exact(sock, crlf, 2);
		if (ret) {
			return;
		}
	}
}

static void client_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (1) {
		int sock = tcp_connect(cfg.host, cfg.port);
		if (sock < 0) {
			k_msleep(RECONNECT_BACKOFF_MS);
			continue;
		}
		client_session(sock);
		zsock_close(sock);
		LOG_WRN("disconnected, retrying in %d ms", RECONNECT_BACKOFF_MS);
		k_msleep(RECONNECT_BACKOFF_MS);
	}
}

int mjpeg_client_start(const char *host, uint16_t port, const char *path,
		       mjpeg_frame_cb_t cb, void *user)
{
	cfg.host = host;
	cfg.port = port;
	cfg.path = path;
	cfg.cb = cb;
	cfg.user = user;

	k_thread_create(&client_thread, client_stack,
			K_THREAD_STACK_SIZEOF(client_stack),
			client_task, NULL, NULL, NULL,
			CLIENT_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&client_thread, "mjpeg_cli");
	return 0;
}
