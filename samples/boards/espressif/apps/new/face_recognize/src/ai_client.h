#ifndef AI_CLIENT_H_
#define AI_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

/* Initialize TLS credential (register CA cert). Call once after wifi_up(). */
int ai_client_init(void);

/*
 * POST a JPEG image to Aliyun bailian (qwen-vl-plus, OpenAI-compatible).
 * The full HTTP response (status line + headers + body) is dumped to UART
 * via printk between AI_RESP_BEGIN / AI_RESP_END markers.
 *
 * Returns 0 on success (got an HTTP response), negative errno on failure.
 */
int ai_client_post_jpeg(const uint8_t *jpeg, size_t jpeg_len);

/*
 * Extract the qwen-vl-plus caption text from the most recent response
 * (i.e. the value of choices[0].message.content). Returns the number of
 * bytes written to `out` (excluding NUL terminator), 0 if not found.
 * Performs minimal JSON unescape of \", \\ and \n.
 */
int ai_client_get_caption(char *out, size_t out_max);

#endif /* AI_CLIENT_H_ */
