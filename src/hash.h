#ifndef MCLC_HASH_H
#define MCLC_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct {
    uint32_t h[5];
    uint64_t total;
    unsigned char buffer[64];
    size_t used;
} Sha1Context;

void sha1_init(Sha1Context *ctx);
void sha1_update(Sha1Context *ctx, const void *data, size_t len);
void sha1_final(Sha1Context *ctx, unsigned char digest[20]);
void sha1_hex(const unsigned char digest[20], char out[41]);
int sha1_file(const wchar_t *path, unsigned char digest[20]);
int sha1_file_hex(const wchar_t *path, char out[41]);

void md5_bytes(const void *data, size_t len, unsigned char digest[16]);
int offline_uuid_for_name(const char *name, char out[37]);

#endif /* MCLC_HASH_H */
