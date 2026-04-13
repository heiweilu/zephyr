/*
 * compat.c — Zephyr compatibility shims for nofrendo
 *
 * nofrendo uses a handful of POSIX functions that may not exist in Zephyr's
 * minimal libc.  Provide them here so we avoid pulling in full newlib.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <stdlib.h>

/*
 * strdup — duplicate a string using the Zephyr system heap.
 * Used by nofrendo.c to copy filenames ("osd:").
 */
char *strdup(const char *s)
{
	if (!s) {
		return NULL;
	}
	size_t len = strlen(s) + 1u;
	char *dup = k_malloc(len);

	if (dup) {
		memcpy(dup, s, len);
	}
	return dup;
}
