#ifndef SMART_CAMERA_WIFI_MGR_H_
#define SMART_CAMERA_WIFI_MGR_H_

#include <stdint.h>

/* Connect Wi-Fi STA, wait for IPv4 lease.
 *
 * On success, *ip_str_out (caller-provided buffer of at least 16 bytes)
 * is filled with the dotted IPv4 string. Returns 0 on success, negative errno.
 */
int wifi_mgr_connect_blocking(char *ip_str_out, size_t ip_str_size);

#endif /* SMART_CAMERA_WIFI_MGR_H_ */
