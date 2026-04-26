#ifndef SMART_CAMERA_MJPEG_CLIENT_H_
#define SMART_CAMERA_MJPEG_CLIENT_H_

#include <stdint.h>
#include <stddef.h>

/* Frame received from PC server. The buffer is owned by the receiver thread
 * and is reused once display_ui has consumed it.
 */
struct mjpeg_frame {
	uint8_t *data;
	size_t len;
	uint32_t seq;
};

typedef void (*mjpeg_frame_cb_t)(const struct mjpeg_frame *frame, void *user);

/* Spawn the background MJPEG client thread.
 *
 *   host, port  PC server endpoint.
 *   path        URL path (typically "/mjpeg").
 *   cb          Called for every full JPEG frame; the data pointer is only
 *               valid during the callback. The callback runs on the client
 *               thread, so do quick work or copy/queue.
 *   user        Opaque pointer forwarded to cb.
 */
int mjpeg_client_start(const char *host, uint16_t port, const char *path,
		       mjpeg_frame_cb_t cb, void *user);

#endif /* SMART_CAMERA_MJPEG_CLIENT_H_ */
