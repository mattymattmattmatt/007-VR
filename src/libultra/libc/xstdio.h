#ifndef _XSTDIO_H
#define _XSTDIO_H
#include <ultratypes.h>
#include <stdarg.h>
/*#include <stddef.h>*/

/**
 * xstdio.h aka printf.h
*/

typedef struct
{
    union {
        /* 00 */ s64 s64;
        u64 u64;
        f64 f64;
        u32 u32;
        u16 u16;
    } value;
    /* 08 */ u8 *buff;
    /* 0c */ s32 n0;
    /* 10 */ s32 num_leading_zeros;
    /* 14 */ s32 part2_len;
    /* 18 */ s32 num_mid_zeros;
    /* 1c */ s32 part3_len;
    /* 20 */ s32 num_trailing_zeros;
    /* 24 */ s32 precision;
    /* 28 */ s32 width;
    /* 2c */ u32 size;
    /* 30 */ u32 flags;
    /* 34 */ u8 length;
} printf_struct;

#define FLAGS_SPACE 1
#define FLAGS_PLUS 2
#define FLAGS_MINUS 4
#define FLAGS_HASH 8
#define FLAGS_ZERO 16

#ifdef GEPC
/* This header is the odd one out, and GCC will not let it pass. _Printf is
 * defined in src/libultrare/libc/xprintf.c over char *, does pointer
 * arithmetic on the format string as char *, and every caller is char * too:
 * sprintf(char *dst, const char *fmt, ...) hands both straight through, and
 * proutSprintf is declared char *(char *, const char *, size_t). Only this
 * declaration says u8. IDO did not care; C does, and a declaration that
 * disagrees with its definition is undefined behaviour rather than a warning
 * to route around.
 *
 * Following the definition and the callers rather than the declaration, since
 * they are what the code actually does. Three places mention outfun and they
 * are all in this header and that one file. */
typedef char *outfun(char *, const char *, size_t);

int _Printf(outfun prout, char *arg, const char *fmt, va_list args);
#else
typedef u8 *outfun(u8*,const u8*,size_t);

int _Printf(outfun prout, u8 *arg, const u8 *fmt, va_list args);
#endif
void _Litob(printf_struct *args, u8 type);
void _Ldtob(printf_struct *args, u8 type);

#endif
