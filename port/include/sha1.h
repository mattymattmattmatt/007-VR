/*
 * sha1.h - just enough SHA-1 to tell a player their ROM is the wrong one.
 *
 * Not for anything security-bearing; SHA-1 is only here because that is what
 * the repository's ge007.*.sha1 files record.
 */
#ifndef GEPC_SHA1_H
#define GEPC_SHA1_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEPC_SHA1_SIZE 20

typedef struct gepc_sha1 {
    unsigned int  state[5];
    unsigned long long count;   /* message length in bits */
    unsigned char buffer[64];
} gepc_sha1;

void gepc_sha1_init(gepc_sha1 *ctx);
void gepc_sha1_update(gepc_sha1 *ctx, const void *data, size_t len);
void gepc_sha1_final(gepc_sha1 *ctx, unsigned char out[GEPC_SHA1_SIZE]);

/* Convenience: hash a buffer and format it as lowercase hex. `hex` needs 41
 * bytes. */
void gepc_sha1_hex(const void *data, size_t len, char hex[41]);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_SHA1_H */
