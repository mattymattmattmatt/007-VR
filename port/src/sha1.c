#include "sha1.h"

#include <string.h>

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block(gepc_sha1 *ctx, const unsigned char *block)
{
    unsigned int w[80];
    unsigned int a, b, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)block[i * 4 + 0] << 24)
             | ((unsigned int)block[i * 4 + 1] << 16)
             | ((unsigned int)block[i * 4 + 2] << 8)
             | ((unsigned int)block[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

void gepc_sha1_init(gepc_sha1 *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void gepc_sha1_update(gepc_sha1 *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t have = (size_t)((ctx->count / 8) % 64);
    size_t need;

    ctx->count += (unsigned long long)len * 8;

    if (have) {
        need = 64 - have;
        if (len < need) {
            memcpy(ctx->buffer + have, p, len);
            return;
        }
        memcpy(ctx->buffer + have, p, need);
        sha1_block(ctx, ctx->buffer);
        p += need;
        len -= need;
    }

    while (len >= 64) {
        sha1_block(ctx, p);
        p += 64;
        len -= 64;
    }

    if (len) {
        memcpy(ctx->buffer, p, len);
    }
}

void gepc_sha1_final(gepc_sha1 *ctx, unsigned char out[GEPC_SHA1_SIZE])
{
    unsigned long long bits = ctx->count;
    size_t have = (size_t)((ctx->count / 8) % 64);
    unsigned char pad = 0x80;
    unsigned char zero = 0x00;
    unsigned char lenbuf[8];
    int i;

    for (i = 0; i < 8; i++) {
        lenbuf[i] = (unsigned char)(bits >> (56 - i * 8));
    }

    gepc_sha1_update(ctx, &pad, 1);
    /* Pad with zeros until there is exactly room for the 8-byte length. */
    while ((size_t)((ctx->count / 8) % 64) != 56) {
        gepc_sha1_update(ctx, &zero, 1);
    }
    /* Appending the length must not itself change the counted length. */
    {
        unsigned long long saved = ctx->count;
        gepc_sha1_update(ctx, lenbuf, 8);
        ctx->count = saved;
    }
    (void)have;

    for (i = 0; i < 5; i++) {
        out[i * 4 + 0] = (unsigned char)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
    }
}

void gepc_sha1_hex(const void *data, size_t len, char hex[41])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char d[GEPC_SHA1_SIZE];
    gepc_sha1 ctx;
    int i;

    gepc_sha1_init(&ctx);
    gepc_sha1_update(&ctx, data, len);
    gepc_sha1_final(&ctx, d);

    for (i = 0; i < GEPC_SHA1_SIZE; i++) {
        hex[i * 2 + 0] = digits[(d[i] >> 4) & 0xF];
        hex[i * 2 + 1] = digits[d[i] & 0xF];
    }
    hex[40] = '\0';
}
