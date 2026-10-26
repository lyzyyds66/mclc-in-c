#ifndef MCLC_HTTP_H
#define MCLC_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/*
 * Download a file over HTTP(S). The file is written atomically through a
 * ".part" file. If expected_size is greater than zero the byte count is
 * checked. Existing files are overwritten.
 */
int http_download(const wchar_t *url, const wchar_t *destination,
                  int show_progress, int64_t expected_size, char **error);

/* Fetch a small text document (version manifest, version JSON, ...). */
int http_get_string(const wchar_t *url, char **data, size_t *size,
                    size_t max_size, char **error);

#endif /* MCLC_HTTP_H */
