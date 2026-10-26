#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t x)
{
    p[0] = (unsigned char)(x >> 24);
    p[1] = (unsigned char)(x >> 16);
    p[2] = (unsigned char)(x >> 8);
    p[3] = (unsigned char)x;
}

static uint32_t load_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(unsigned char *p, uint32_t x)
{
    p[0] = (unsigned char)x;
    p[1] = (unsigned char)(x >> 8);
    p[2] = (unsigned char)(x >> 16);
    p[3] = (unsigned char)(x >> 24);
}

static void sha1_transform(Sha1Context *ctx, const unsigned char block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be32(block + i * 4);
    }
    for (i = 16; i < 80; i++) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        uint32_t temp;
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
        temp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

void sha1_init(Sha1Context *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->total = 0;
    ctx->used = 0;
}

void sha1_update(Sha1Context *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    ctx->total += (uint64_t)len;
    if (ctx->used) {
        size_t take = 64 - ctx->used;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buffer + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == 64) {
            sha1_transform(ctx, ctx->buffer);
            ctx->used = 0;
        }
    }
    while (len >= 64) {
        sha1_transform(ctx, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(ctx->buffer + ctx->used, p, len);
        ctx->used += len;
    }
}

void sha1_final(Sha1Context *ctx, unsigned char digest[20])
{
    uint64_t bits = ctx->total * 8;
    unsigned char pad[72];
    size_t pad_len;
    int i;

    pad[0] = 0x80;
    memset(pad + 1, 0, sizeof(pad) - 1);
    pad_len = (ctx->used < 56) ? (56 - ctx->used) : (120 - ctx->used);
    sha1_update(ctx, pad, pad_len);
    for (i = 7; i >= 0; i--) {
        pad[7 - i] = (unsigned char)(bits >> (i * 8));
    }
    sha1_update(ctx, pad, 8);
    for (i = 0; i < 5; i++) {
        store_be32(digest + i * 4, ctx->h[i]);
    }
}

void sha1_hex(const unsigned char digest[20], char out[41])
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 20; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[40] = '\0';
}

int sha1_file(const wchar_t *path, unsigned char digest[20])
{
    FILE *f = _wfopen(path, L"rb");
    unsigned char buffer[65536];
    size_t got;
    Sha1Context ctx;

    if (!f) {
        return -1;
    }
    sha1_init(&ctx);
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        sha1_update(&ctx, buffer, got);
    }
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    sha1_final(&ctx, digest);
    return 0;
}

int sha1_file_hex(const wchar_t *path, char out[41])
{
    unsigned char digest[20];
    if (sha1_file(path, digest) != 0) {
        return -1;
    }
    sha1_hex(digest, out);
    return 0;
}

/* MD5, used only to derive the vanilla offline UUID. */
static const uint32_t MD5_K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const int MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_transform(uint32_t state[4], const unsigned char block[64])
{
    uint32_t m[16];
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    int i;

    for (i = 0; i < 16; i++) {
        m[i] = load_le32(block + i * 4);
    }
    for (i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | (~d));
            g = (7 * i) % 16;
        }
        {
            uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl32(a + f + MD5_K[i] + m[g], MD5_S[i]);
            a = temp;
        }
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_final(uint32_t state[4], uint64_t total,
                      const unsigned char *buffer, size_t used,
                      unsigned char digest[16])
{
    unsigned char pad[72];
    uint64_t bits = total * 8;
    size_t pad_len;
    int i;

    pad[0] = 0x80;
    memset(pad + 1, 0, sizeof(pad) - 1);
    pad_len = (used < 56) ? (56 - used) : (120 - used);

    /* Re-create a compact context by feeding the tail directly. */
    {
        unsigned char block[128];
        size_t pos = 0;
        memcpy(block, buffer, used);
        pos = used;
        memcpy(block + pos, pad, pad_len);
        pos += pad_len;
        for (i = 0; i < 8; i++) {
            block[pos + (size_t)i] = (unsigned char)(bits >> (i * 8));
        }
        pos += 8;
        md5_transform(state, block);
        if (pos > 64) {
            md5_transform(state, block + 64);
        }
    }
    for (i = 0; i < 4; i++) {
        store_le32(digest + i * 4, state[i]);
    }
}

void md5_bytes(const void *data, size_t len, unsigned char digest[16])
{
    uint32_t state[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu,
                          0x10325476u };
    const unsigned char *p = (const unsigned char *)data;
    size_t remaining = len;
    uint64_t total = len;

    while (remaining >= 64) {
        md5_transform(state, p);
        p += 64;
        remaining -= 64;
    }
    md5_final(state, total, p, remaining, digest);
}

int offline_uuid_for_name(const char *name, char out[37])
{
    static const char hex[] = "0123456789abcdef";
    char seed[512];
    unsigned char digest[16];
    int n;
    int i;

    n = snprintf(seed, sizeof(seed), "OfflinePlayer:%s", name ? name : "");
    if (n < 0 || (size_t)n >= sizeof(seed)) {
        return -1;
    }
    md5_bytes(seed, (size_t)n, digest);
    digest[6] = (unsigned char)((digest[6] & 0x0F) | 0x30);
    digest[8] = (unsigned char)((digest[8] & 0x3F) | 0x80);
    for (i = 0; i < 16; i++) {
        int pos = i * 2 + (i >= 4 ? 1 : 0) + (i >= 6 ? 1 : 0) +
                  (i >= 8 ? 1 : 0) + (i >= 10 ? 1 : 0);
        out[pos] = hex[digest[i] >> 4];
        out[pos + 1] = hex[digest[i] & 0x0F];
    }
    out[36] = '\0';
    out[8] = '-';
    out[13] = '-';
    out[18] = '-';
    out[23] = '-';
    return 0;
}
