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

#include <stdlib.h>   /* malloc / free / strdup */
#include <string.h>   /* strdup */

/* nofrendo housekeeping no-ops (defined in osd.c if ever called). */
static inline void mem_cleanup(void)     { }
static inline void mem_checkblocks(void) { }
static inline void mem_checkleaks(void)  { }

#endif /* _MEMGUARD_H_ */
