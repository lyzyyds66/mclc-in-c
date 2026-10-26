/*
 * MCLC - Minecraft C Launcher
 * Small, dependency-free helpers: UTF-8/UTF-16, dynamic strings and filesystem.
 */
#ifndef MCLC_UTIL_H
#define MCLC_UTIL_H

#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#ifdef __GNUC__
#define MCLC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define MCLC_PRINTF(a, b)
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
int sb_reserve(StrBuf *sb, size_t extra);
int sb_append_n(StrBuf *sb, const char *text, size_t len);
int sb_append(StrBuf *sb, const char *text);
int sb_append_char(StrBuf *sb, char c);
int sb_appendf(StrBuf *sb, const char *fmt, ...) MCLC_PRINTF(2, 3);
int sb_append_utf8(StrBuf *sb, const wchar_t *wide);
char *sb_take(StrBuf *sb);
void sb_free(StrBuf *sb);

void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *text);
char *xstrndup(const char *text, size_t len);
wchar_t *xwcsdup(const wchar_t *text);

char *wide_to_utf8(const wchar_t *text);
wchar_t *utf8_to_wide(const char *text);

wchar_t *path_join(const wchar_t *base, const wchar_t *child);
wchar_t *path_join3(const wchar_t *a, const wchar_t *b, const wchar_t *c);
int path_exists(const wchar_t *path);
int path_is_dir(const wchar_t *path);
int64_t path_size(const wchar_t *path);

/*
 * Validation for untrusted metadata. path_component_is_safe accepts one
 * filename/version-id component (no separators, drive colon, "." or "..").
 * path_relative_is_safe accepts a Maven-style forward-slash relative path.
 */
int path_component_is_safe(const char *text);
int path_relative_is_safe(const char *text);
int mkdir_p(const wchar_t *path);
int ensure_parent_dir(const wchar_t *file_path);

char *read_text_file(const wchar_t *path, size_t *size_out, char **error);
int write_file_all(const wchar_t *path, const void *data, size_t size);
int file_starts_with(const wchar_t *path, const char *magic, size_t magic_len);

int str_ieq(const char *a, const char *b);
int str_ends_with(const char *text, const char *suffix);
int str_ends_with_ci(const char *text, const char *suffix);
int str_starts_with(const char *text, const char *prefix);

char *format_utf8(const char *fmt, ...) MCLC_PRINTF(1, 2);

/* error, when non-NULL, must point to a NULL-initialized char* (or a string
 * previously returned by set_error). */
void set_error(char **error, const char *fmt, ...) MCLC_PRINTF(2, 3);

#endif /* MCLC_UTIL_H */
