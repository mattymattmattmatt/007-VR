/*
 * gepc_prelude.h - force-included ahead of every game translation unit.
 *
 * The repository ships its own include/math.h, which carries both the N64
 * SDK's float constants and a set of declarations that clash with libc. The
 * PC build puts the repository's include/ *after* the system directories (see
 * port/README.md), so libc's math.h wins -- which is what we want for the
 * functions, but it means the SDK's float constants disappear and around a
 * dozen translation units stop compiling.
 *
 * This restores just those constants. Every one is #ifndef-guarded, so if a
 * translation unit does reach the repository's math.h first, nothing is
 * redefined.
 */
#ifndef GEPC_PRELUDE_H
#define GEPC_PRELUDE_H

/* Pull libc's math.h in first, so the guards below see whatever it defines. */
#include <math.h>

#ifndef M_PI_F
#define M_PI_F           3.1415927f
#endif
#ifndef M_MINUS_PI_F
#define M_MINUS_PI_F     (-3.1415927f)
#endif
#ifndef M_PI_2F
#define M_PI_2F          1.5707964f
#endif
#ifndef M_TAU
#define M_TAU            6.28318530717958647692
#endif
#ifndef M_TAU_F
#define M_TAU_F          6.2831855f
#endif
#ifndef M_HALF_PI
#define M_HALF_PI        (M_PI_F / 2)
#endif
#ifndef M_LN2F
#define M_LN2F           0.69813174f
#endif

/* Scale factors for converting normalised integers to float. The names say
 * "MAX_VALUE" but the values are the range, i.e. one past the maximum. */
#ifndef M_U16_MAX_VALUE_F
#define M_U16_MAX_VALUE_F 65536.0f
#endif
#ifndef M_U32_MAX_VALUE_F
#define M_U32_MAX_VALUE_F 4294967296.0f
#endif

#endif /* GEPC_PRELUDE_H */
