#ifndef SMART_CAMERA_TRACK_CLIENT_H_
#define SMART_CAMERA_TRACK_CLIENT_H_

#include <stdint.h>

/* Callback invoked once per NDJSON line received from /track. cx<0 means
 * "no target" (hide the crosshair).
 */
typedef void (*track_target_cb_t)(int cx, int cy, void *user);

/* Start a background thread that connects to http://host:port/track and
 * calls cb(cx, cy, user) for every JSON line. Returns 0 on success.
 */
int track_client_start(const char *host, uint16_t port,
                       track_target_cb_t cb, void *user);

#endif /* SMART_CAMERA_TRACK_CLIENT_H_ */
