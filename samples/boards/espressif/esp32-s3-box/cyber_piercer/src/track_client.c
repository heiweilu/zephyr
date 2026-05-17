  /*
 * smart_camera /track NDJSON client.
 *
 * Connects to http://<host>:<port>/track and parses one JSON object per line.
 * Each line looks like:    {"seq":N,"cx":X,"cy":Y}\n
 * cx==-1 means no target.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "track_client.h"

LOG_MODULE_REGISTER(track_client, LOG_LEVEL_INF);

#define TRACK_THREAD_STACK 3072
#define TRACK_THREAD_PRIO  7
#define LINE_BUF_SIZE      256
#define BACKOFF_MS         2000

static K_THREAD_STACK_DEFINE(track_stack, TRACK_THREAD_STACK);
static struct k_thread track_thread;

struct track_cfg {
	const char *host;
	uint16_t port;
	track_target_cb_t cb;
	void *user;
};

static struct track_cfg cfg;

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
		zsock_freeaddrinfo(res);
		return -errno;
	}
	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		zsock_close(sock);
		return -errno;
	}
	return sock;
}

static int send_get(int sock, const char *host, uint16_t port)
{
	char req[160];
	int len = snprintk(req, sizeof(req),
			   "GET /track HTTP/1.0\r\n"
			   "Host: %s:%u\r\n"
			   "Accept: */*\r\n"
			   "\r\n",
			   host, port);
	int sent = 0;
	while (sent < len) {
		int n = zsock_send(sock, req + sent, len - sent, 0);
		if (n <= 0) return -errno;
		sent += n;
	}
	return 0;
}

/* Read until "\r\n\r\n" (end of HTTP response headers). */
static int skip_headers(int sock)
{
	uint8_t window[4] = {0};
	while (1) {
		uint8_t b;
		int n = zsock_recv(sock, &b, 1, 0);
		if (n <= 0) return -EPIPE;
		window[0] = window[1];
		window[1] = window[2];
		window[2] = window[3];
		window[3] = b;
		if (window[0] == '\r' && window[1] == '\n' &&
		    window[2] == '\r' && window[3] == '\n') {
			return 0;
		}
	}
}

/* Naive JSON {"seq":N,"cx":X,"cy":Y} parser. Returns 0 and fills *cx/*cy
 * (or -1 for no target).
 */
static int parse_line(const char *line, int *cx, int *cy)
{
	const char *p = strstr(line, "\"cx\":");
	if (!p) return -EINVAL;
	*cx = atoi(p + 5);
	p = strstr(line, "\"cy\":");
	if (!p) return -EINVAL;
	*cy = atoi(p + 5);
	return 0;
}

static void session(int sock)
{
	if (send_get(sock, cfg.host, cfg.port) < 0) return;
	if (skip_headers(sock) < 0) return;

	char line[LINE_BUF_SIZE];
	size_t pos = 0;
	while (1) {
		uint8_t b;
		int n = zsock_recv(sock, &b, 1, 0);
		if (n <= 0) return;
		if (b == '\n') {
			line[pos] = '\0';
			int cx = -1, cy = -1;
			if (parse_line(line, &cx, &cy) == 0 && cfg.cb) {
				cfg.cb(cx, cy, cfg.user);
			}
			pos = 0;
		} else if (pos < sizeof(line) - 1) {
			line[pos++] = (char)b;
		} else {
			pos = 0; /* line too long, drop */
		}
	}
}

static void track_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	while (1) {
		int sock = tcp_connect(cfg.host, cfg.port);
		if (sock < 0) {
			k_msleep(BACKOFF_MS);
			continue;
		}
		LOG_INF("track connected %s:%u", cfg.host, cfg.port);
		session(sock);
		zsock_close(sock);
		LOG_WRN("track disconnected, retry in %d ms", BACKOFF_MS);
		k_msleep(BACKOFF_MS);
	}
}

int track_client_start(const char *host, uint16_t port,
		       track_target_cb_t cb, void *user)
{
	cfg.host = host;
	cfg.port = port;
	cfg.cb = cb;
	cfg.user = user;
	k_thread_create(&track_thread, track_stack,
			K_THREAD_STACK_SIZEOF(track_stack),
			track_task, NULL, NULL, NULL,
			TRACK_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&track_thread, "track");
	return 0;
}
