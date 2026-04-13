/*
** noftypes.h — Zephyr port override
**
** Shadows nofrendo's original noftypes.h so it is found first via the
** "src/" BEFORE-PRIVATE include path added in CMakeLists.txt.
**
** Problem in the original:
**   1. noftypes.h defines: typedef enum { false=0, true=1 } bool;
**   2. It then does:        #include <log.h>
**   3. log.h → stdio.h → picolibc's stdbool.h → #define bool _Bool
**   4. After step 3, bool in source text maps to the C99 keyword _Bool.
**   5. Any function declared (with enum bool) before step 3 now conflicts
**      with its definition (with _Bool) after step 3.
**      → "error: conflicting types for 'ppu_displaysprites'"
**
** Fix: Use typedef _Bool bool instead of the enum typedef.
**   typedef _Bool bool  →  bool and _Bool are the same underlying type.
**   stdbool.h's "#define bool _Bool" just redefines bool to expand to
**   the same _Bool keyword. Function declarations and definitions always
**   resolve to the same type → no conflict.
**
** All other content is identical to the original noftypes.h.
*/

#ifndef _TYPES_H_
#define _TYPES_H_

/* Define this if running on little-endian (x86) systems */
#define  HOST_LITTLE_ENDIAN

#ifdef __GNUC__
#define  INLINE      static inline
#define  ZERO_LENGTH 0
#elif defined(WIN32)
#define  INLINE      static __inline
#define  ZERO_LENGTH 0
#else /* crapintosh? */
#define  INLINE      static
#define  ZERO_LENGTH 1
#endif

/* quell stupid compiler warnings */
#define  UNUSED(x)   ((x) = (x))

typedef  signed char    int8;
typedef  signed short   int16;
typedef  signed int     int32;
typedef  unsigned char  uint8;
typedef  unsigned short uint16;
typedef  unsigned int   uint32;

#ifndef __cplusplus

/*
 * CHANGED from original: use C99 _Bool as the underlying type for bool so
 * that including stdbool.h later (via stdio.h → picolibc) is harmless.
 * stdbool.h defines "#define bool _Bool" which is identical to this typedef
 * (both resolve to the same _Bool type), preventing any function-type
 * mismatch between declarations and definitions.
 */
#ifndef bool
typedef _Bool bool;
#endif
#ifndef false
#define false 0
#endif
#ifndef true
#define true  1
#endif

#ifndef  NULL
#define  NULL     ((void *) 0)
#endif

#endif /* !__cplusplus */

#include <memguard.h>
#include <log.h>

#ifdef NOFRENDO_DEBUG

#define  ASSERT(expr)      log_assert((int) (expr), __LINE__, __FILE__, NULL)
#define  ASSERT_MSG(msg)   log_assert(false, __LINE__, __FILE__, (msg))

#else /* !NOFRENDO_DEBUG */

#define  ASSERT(expr)
#define  ASSERT_MSG(msg)

#endif /* !NOFRENDO_DEBUG */

#endif /* _TYPES_H_ */
