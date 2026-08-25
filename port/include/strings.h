/*
 * strings.h - deliberately not glibc's.
 *
 * PR/os.h declares bcopy, bcmp and bzero with int lengths, the way the N64 SDK
 * did. glibc's <strings.h> declares the same three with size_t, and glibc's
 * <string.h> pulls it in, so any translation unit that sees both headers fails
 * to compile on a conflicting-types error.
 *
 * Since port/include comes first on the search path, this file is what
 * <strings.h> resolves to, and the N64 declarations are left to win. The
 * functions that do NOT conflict are forwarded so port code can still use
 * them.
 *
 * Only affects the PC build. The ROM build never sees this directory.
 */
#ifndef GEPC_STRINGS_H
#define GEPC_STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
int ffs(int i);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_STRINGS_H */
