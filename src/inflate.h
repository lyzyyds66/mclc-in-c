#ifndef MCLC_INFLATE_H
#define MCLC_INFLATE_H

#include <stddef.h>

/*
 * Decode a raw DEFLATE stream (RFC 1951, no zlib wrapper).
 * When expected_size is non-zero the output buffer is sized exactly and the
 * decoded length must match; otherwise the buffer grows as needed.
 */
int mclc_inflate_raw(const unsigned char *input, size_t input_size,
                     size_t expected_size, unsigned char **output,
                     size_t *output_size, char **error);

#endif /* MCLC_INFLATE_H */
