#ifndef _MATH_ASINACOS_H_
#define _MATH_ASINACOS_H_

#include <ultra64.h>

/* These are the game's fixed-point arc functions: they take a s16 in the
 * engine's angle units and return one, and have nothing to do with libc's
 * double-precision acos/asin beyond the name. On the N64 that was fine,
 * because the repository's own math.h was the one in scope and declares
 * neither. The PC build resolves <math.h> to glibc's (the repository's
 * include/ comes after the system directories, deliberately -- see
 * port/include/platform.h), so both declarations land in the same
 * translation unit and collide.
 *
 * Nothing in the game calls either one: they are defined here and declared
 * here, and that is all. So the PC build renames them rather than contorting
 * the include order for two functions with no callers. If something starts
 * calling them, it will pick up the rename through this header. */
#ifdef GEPC
#    define acos gameAcosFixed
#    define asin gameAsinFixed
#endif

u16 acos(s16 arg0);
s16 asin(s16 arg0);

#endif
