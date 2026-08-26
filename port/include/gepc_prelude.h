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

/* Pull libc's math.h and float.h in first, so the guards below see whatever
 * they define. The repository's math.h carries FLT_MAX and FLT_EPSILON too,
 * and being shadowed it takes those with it. */
#include <math.h>
#include <float.h>

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
#ifndef M_THREE_HALF_PI
#define M_THREE_HALF_PI  (3 * M_HALF_PI)
#endif

/* The SDK compiler folded this constant slightly off exact 0.1f, and the game
 * compares against it, so the value matters rather than just the name. */
#ifndef IDO_POINT_ONE
#define IDO_POINT_ONE    0.10000001f
#endif

/* Scale factors for converting normalised integers to float. The names say
 * "MAX_VALUE" but the values are the range, i.e. one past the maximum. */
#ifndef M_U16_MAX_VALUE_F
#define M_U16_MAX_VALUE_F 65536.0f
#endif
#ifndef M_U32_MAX_VALUE_F
#define M_U32_MAX_VALUE_F 4294967296.0f
#endif


/* The repository's own include/math.h carries these, but the PC build resolves
 * <math.h> to glibc's -- deliberately, since the repository's include/ comes
 * after the system directories. Without them the game's uses look like calls
 * to functions nobody defines, and the failure surfaces at link time as
 * "undefined reference to SQR", which is a long way from the cause.
 * Guarded individually so a build that does see the repository header is
 * unaffected. */
#ifndef SQR
#  define SQR(x)    ((x) * (x))
#endif
#ifndef ABS
#  define ABS(x)    ((x) < 0 ? -(x) : (x))
#endif
#ifndef SGN
#  define SGN(x)    ((x) < 0 ? -1 : (x) > 0 ? 1 : 0)
#endif
#ifndef MIN
#  define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#  define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif
#ifndef DegToRad
#  define DegToRad(DEG)      (float)((DEG) * M_TAU_F / 360.0f)
#endif
#ifndef DegToRad1Fact
/* One multiply rather than two, which is why the game has both. */
#  define DegToRad1Fact(DEG) (float)((DEG) * (float)(M_TAU / 360.0))
#endif
#ifndef RadToDeg
#  define RadToDeg(RAD)      (float)((RAD) * (360.0f / M_TAU_F))
#endif
#ifndef mDegToHalfRad
#  define mDegToHalfRad(x)   ((x * M_PI_F) / 360.0f)
#endif
#ifndef DEG2BYTE
#  define DEG2BYTE(DEG)      (char)(256.0f / 360.0f * (DEG))
#endif
#ifndef RAD2BYTE
#  define RAD2BYTE(RAD)      (char)(256.0f / M_TAU_F * (RAD))
#endif
#ifndef ByteToRadian
#  define ByteToRadian(Byte) ((Byte * M_TAU_F) * (1.0f / 256.0f))
#endif

#endif /* GEPC_PRELUDE_H */
