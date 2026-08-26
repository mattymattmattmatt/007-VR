#ifndef _IDO_COMPAT_H_
#define _IDO_COMPAT_H_

/*
 * Places where the game relies on something the SGI compiler accepted and
 * standard C does not, spelled out once so the PC build does not carry five
 * copies of the same workaround.
 */

/*
 * IDO accepted
 *
 *     T dst[N] = src;
 *
 * where src is another array of the same type, and copied its contents. C has
 * no array assignment at all, let alone in an initialiser, so GCC rejects it
 * outright as "invalid initializer". Five places rely on it: the ricochet
 * sound table in chraction.c, the custom reverb parameters in audi.c, and the
 * three stack-pointer tables in crash.c.
 *
 * The copy is what the N64 code actually did, so the PC build just writes it
 * out. Used as:
 *
 *     IDO_ARRAY_INIT(s16 mrs[3], mrs, metal_ricochet_SFX);
 *
 * The size comes from the destination, so a source array that is declared
 * shorter than the destination is a compile error on the ROM build and a
 * short read here -- keep the two the same length, as all five sites do.
 */
#ifdef GEPC
#    include <string.h>
#    define IDO_ARRAY_INIT(decl, name, src)                                 \
         decl;                                                              \
         memcpy((name), (src), sizeof(name))
#else
#    define IDO_ARRAY_INIT(decl, name, src) decl = src
#endif

#endif /* _IDO_COMPAT_H_ */
