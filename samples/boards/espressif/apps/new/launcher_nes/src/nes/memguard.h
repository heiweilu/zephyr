/*
 * memguard.h — Zephyr port shadow
 *
 * Original nofrendo/memguard.h does:
 *     #define malloc(s) _my_malloc((s))
 *     #define free(d)   _my_free((void **) &(d))
 * Those macro definitions corrupt picolibc's <stdlib.h> declarations
 * (e.g. `aligned_alloc` is annotated with `__free_like(free)` which gets
 * macro-substituted into garbage), causing build failures whenever any
 * translation unit transitively pulls <stdlib.h> after <memguard.h>.
 *
 * In Zephyr we let nofrendo use picolibc malloc/free directly (the heap
 * is sized via CONFIG_HEAP_MEM_POOL_SIZE), so no wrapping is needed.
 * This shadow header simply blocks the original macro tricks.
 *
 * Picked up first because CMakeLists adds  src/nes  as a BEFORE-PRIVATE
 * include path.
 */
#ifndef _MEMGUARD_H_
#define _MEMGUARD_H_

#include <stdlib.h>   /* must be FIRST so picolibc declares aligned_alloc
			before we redirect malloc/free below */
#include <string.h>

/* nofrendo housekeeping no-ops. */
static inline void mem_cleanup(void)     { }
static inline void mem_checkblocks(void) { }
static inline void mem_checkleaks(void)  { }

/* Route nofrendo allocations to a 256 KB PSRAM sys_heap (defined in osd.c).
 * picolibc's 16 KB DRAM heap is way too small for the 60 KB GUI framebuffer
 * that bmp_create(256, 240, 0) wants. osd.c does not include this header
 * so its own malloc/free calls remain on the picolibc heap. */
extern void *nes_malloc(size_t size);
extern void  nes_free(void *p);
extern char *nes_strdup(const char *s);

#define malloc(s) nes_malloc((s))
#define free(p)   nes_free((p))
#define strdup(s) nes_strdup((s))

#endif /* _MEMGUARD_H_ */

