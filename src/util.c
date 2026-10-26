#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>

void sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

int sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need;
    size_t cap;
    char *p;

    if (extra > SIZE_MAX - sb->len - 1) {
        return -1;
    }
    need = sb->len + extra + 1;
    if (need <= sb->cap) {
        return 0;
    }
    cap = sb->cap ? sb->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    p = (char *)realloc(sb->data, cap);
    if (!p) {
        return -1;
    }
    sb->data = p;
    sb->cap = cap;
    return 0;
}

int sb_append_n(StrBuf *sb, const char *text, size_t len)
{
    if (!text || !len) {
        return 0;
    }
    if (sb_reserve(sb, len) != 0) {
        return -1;
    }
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_append(StrBuf *sb, const char *text)
{
    return text ? sb_append_n(sb, text, strlen(text)) : 0;
}

int sb_append_char(StrBuf *sb, char c)
{
    return sb_append_n(sb, &c, 1);
}

int sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    size_t cap = 256;
    char *tmp;

    for (;;) {
        int n;
        tmp = (char *)malloc(cap);
        if (!tmp) {
            return -1;
        }
        va_start(ap, fmt);
        n = vsnprintf(tmp, cap, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < cap) {
            int rc = sb_append_n(sb, tmp, (size_t)n);
            free(tmp);
            return rc;
        }
        free(tmp);
        if (n >= 0) {
            cap = (size_t)n + 1;
        } else {
            if (cap > 1024 * 1024) {
                return -1;
            }
            cap *= 2;
        }
    }
}

int sb_append_utf8(StrBuf *sb, const wchar_t *wide)
{
    char *u;
    int rc;

    if (!wide) {
        return 0;
    }
    u = wide_to_utf8(wide);
    if (!u) {
        return -1;
    }
    rc = sb_append(sb, u);
    free(u);
    return rc;
}

char *sb_take(StrBuf *sb)
{
    char *p;

    if (!sb->data) {
        p = (char *)malloc(1);
        if (!p) {
            return NULL;
        }
        p[0] = '\0';
    } else {
        p = sb->data;
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return p;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "MCLC: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "MCLC: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);
    if (!p) {
        fprintf(stderr, "MCLC: out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrdup(const char *text)
{
    size_t len;
    char *p;

    if (!text) {
        return NULL;
    }
    len = strlen(text);
    p = (char *)xmalloc(len + 1);
    memcpy(p, text, len + 1);
    return p;
}

char *xstrndup(const char *text, size_t len)
{
    char *p;

    if (!text) {
        return NULL;
    }
    p = (char *)xmalloc(len + 1);
    memcpy(p, text, len);
    p[len] = '\0';
    return p;
}

wchar_t *xwcsdup(const wchar_t *text)
{
    size_t len;
    wchar_t *p;

    if (!text) {
        return NULL;
    }
    len = wcslen(text);
    p = (wchar_t *)xmalloc((len + 1) * sizeof(wchar_t));
    memcpy(p, text, (len + 1) * sizeof(wchar_t));
    return p;
}

char *wide_to_utf8(const wchar_t *text)
{
    int len;
    char *out;

    if (!text) {
        return NULL;
    }
    len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                              NULL, 0, NULL, NULL);
    if (len <= 0) {
        return NULL;
    }
    out = (char *)xmalloc((size_t)len);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                             out, len, NULL, NULL)) {
        free(out);
        return NULL;
    }
    return out;
}

wchar_t *utf8_to_wide(const char *text)
{
    int len;
    wchar_t *out;

    if (!text) {
        return NULL;
    }
    len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                              NULL, 0);
    if (len <= 0) {
        return NULL;
    }
    out = (wchar_t *)xmalloc((size_t)len * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                             out, len)) {
        free(out);
        return NULL;
    }
    return out;
}

static int path_is_absolute(const wchar_t *p)
{
    if (!p || !*p) {
        return 0;
    }
    if (p[0] == L'\\' || p[0] == L'/') {
        return 1;
    }
    if (((p[0] >= L'A' && p[0] <= L'Z') ||
         (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':') {
        return 1;
    }
    return 0;
}

wchar_t *path_join(const wchar_t *base, const wchar_t *child)
{
    size_t blen;
    size_t clen;
    int need_sep;
    wchar_t *out;

    if (!base || !*base) {
        return child ? xwcsdup(child) : NULL;
    }
    if (!child || !*child) {
        return xwcsdup(base);
    }
    if (path_is_absolute(child)) {
        return xwcsdup(child);
    }

    blen = wcslen(base);
    clen = wcslen(child);
    need_sep = !(base[blen - 1] == L'\\' || base[blen - 1] == L'/');
    out = (wchar_t *)xmalloc((blen + (size_t)need_sep + clen + 1) *
                             sizeof(wchar_t));
    memcpy(out, base, blen * sizeof(wchar_t));
    if (need_sep) {
        out[blen++] = L'\\';
    }
    memcpy(out + blen, child, (clen + 1) * sizeof(wchar_t));
    return out;
}

wchar_t *path_join3(const wchar_t *a, const wchar_t *b, const wchar_t *c)
{
    wchar_t *ab = path_join(a, b);
    wchar_t *abc;

    if (!ab) {
        return NULL;
    }
    abc = path_join(ab, c);
    free(ab);
    return abc;
}

int path_component_is_safe(const char *text)
{
    size_t len;

    if (!text || !*text) {
        return 0;
    }
    if (text[0] == '.') {
        return 0;
    }
    len = strlen(text);
    if (text[len - 1] == '.' || text[len - 1] == ' ') {
        return 0;
    }
    return strchr(text, '/') == NULL &&
           strchr(text, '\\') == NULL &&
           strchr(text, ':') == NULL;
}

int path_relative_is_safe(const char *text)
{
    const char *p;

    if (!text || !*text) {
        return 0;
    }
    if (text[0] == '/' || text[0] == '\\' || strchr(text, ':') ||
        strchr(text, '\\')) {
        return 0;
    }
    p = text;
    while (*p) {
        const char *start = p;
        size_t len;
        while (*p && *p != '/') {
            p++;
        }
        len = (size_t)(p - start);
        if (len == 0 || start[0] == '.') {
            return 0;
        }
        if (start[len - 1] == '.' || start[len - 1] == ' ') {
            return 0;
        }
        if (*p == '/') {
            p++;
            if (!*p) {
                return 0;
            }
        }
    }
    return 1;
}

int path_exists(const wchar_t *path)
{
    DWORD attr;

    if (!path || !*path) {
        return 0;
    }
    attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES;
}

int path_is_dir(const wchar_t *path)
{
    DWORD attr;

    if (!path || !*path) {
        return 0;
    }
    attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t path_size(const wchar_t *path)
{
    struct _stat64 st;

    if (!path || _wstat64(path, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_size;
}

static int create_one_dir(const wchar_t *path)
{
    if (CreateDirectoryW(path, NULL)) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS && path_is_dir(path)) {
        return 0;
    }
    return -1;
}

int mkdir_p(const wchar_t *path)
{
    wchar_t *tmp;
    wchar_t *p;
    size_t len;
    int rc = 0;

    if (!path || !*path) {
        return -1;
    }
    if (path_is_dir(path)) {
        return 0;
    }

    tmp = xwcsdup(path);
    len = wcslen(tmp);
    while (len > 1 && (tmp[len - 1] == L'\\' || tmp[len - 1] == L'/')) {
        tmp[--len] = L'\0';
    }

    p = tmp;
    if (len >= 2 && tmp[1] == L':') {
        p = tmp + 3;
    } else if (len >= 2 && tmp[0] == L'\\' && tmp[1] == L'\\') {
        int parts = 0;
        p = tmp + 2;
        while (*p && parts < 2) {
            if (*p == L'\\' || *p == L'/') {
                parts++;
            }
            p++;
        }
    }

    for (; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t saved = *p;
            *p = L'\0';
            if (*tmp && create_one_dir(tmp) != 0) {
                rc = -1;
                *p = saved;
                break;
            }
            *p = saved;
        }
    }
    if (rc == 0 && create_one_dir(tmp) != 0) {
        rc = -1;
    }
    free(tmp);
    return rc;
}

int ensure_parent_dir(const wchar_t *file_path)
{
    wchar_t *tmp;
    wchar_t *slash;
    int rc;

    if (!file_path) {
        return -1;
    }
    tmp = xwcsdup(file_path);
    slash = wcsrchr(tmp, L'\\');
    {
        wchar_t *alt = wcsrchr(tmp, L'/');
        if (!slash || (alt && alt > slash)) {
            slash = alt;
        }
    }
    if (!slash) {
        free(tmp);
        return 0;
    }
    while (slash > tmp && (slash[-1] == L'\\' || slash[-1] == L'/')) {
        slash--;
    }
    *slash = L'\0';
    rc = tmp[0] ? mkdir_p(tmp) : 0;
    free(tmp);
    return rc;
}

char *read_text_file(const wchar_t *path, size_t *size_out, char **error)
{
    FILE *f;
    int64_t size;
    char *data;
    size_t got;

    if (size_out) {
        *size_out = 0;
    }
    f = _wfopen(path, L"rb");
    if (!f) {
        set_error(error, "无法打开文件: %ls (errno=%d)", path, errno);
        return NULL;
    }
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_error(error, "无法读取文件大小: %ls", path);
        return NULL;
    }
    size = _ftelli64(f);
    if (size < 0 || (uint64_t)size > (uint64_t)SIZE_MAX - 1) {
        fclose(f);
        set_error(error, "文件过大: %ls", path);
        return NULL;
    }
    if (_fseeki64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_error(error, "无法定位文件: %ls", path);
        return NULL;
    }
    data = (char *)xmalloc((size_t)size + 1);
    got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        set_error(error, "读取文件失败: %ls", path);
        return NULL;
    }
    data[got] = '\0';
    if (size_out) {
        *size_out = got;
    }
    return data;
}

int write_file_all(const wchar_t *path, const void *data, size_t size)
{
    FILE *f;
    size_t written;

    if (ensure_parent_dir(path) != 0) {
        return -1;
    }
    f = _wfopen(path, L"wb");
    if (!f) {
        return -1;
    }
    written = size ? fwrite(data, 1, size, f) : 0;
    if (fclose(f) != 0 || written != size) {
        return -1;
    }
    return 0;
}

int file_starts_with(const wchar_t *path, const char *magic, size_t magic_len)
{
    FILE *f;
    char buf[16];
    size_t got;

    if (magic_len > sizeof(buf)) {
        return 0;
    }
    f = _wfopen(path, L"rb");
    if (!f) {
        return 0;
    }
    got = fread(buf, 1, magic_len, f);
    fclose(f);
    return got == magic_len && memcmp(buf, magic, magic_len) == 0;
}

int str_ieq(const char *a, const char *b)
{
    if (!a || !b) {
        return a == b;
    }
    return _stricmp(a, b) == 0;
}

int str_ends_with(const char *text, const char *suffix)
{
    size_t lt;
    size_t ls;

    if (!text || !suffix) {
        return 0;
    }
    lt = strlen(text);
    ls = strlen(suffix);
    return lt >= ls && memcmp(text + lt - ls, suffix, ls) == 0;
}

int str_ends_with_ci(const char *text, const char *suffix)
{
    size_t lt;
    size_t ls;

    if (!text || !suffix) {
        return 0;
    }
    lt = strlen(text);
    ls = strlen(suffix);
    return lt >= ls && _stricmp(text + lt - ls, suffix) == 0;
}

int str_starts_with(const char *text, const char *prefix)
{
    if (!text || !prefix) {
        return 0;
    }
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

char *format_utf8(const char *fmt, ...)
{
    StrBuf sb;
    va_list ap;
    char *out;

    sb_init(&sb);
    va_start(ap, fmt);
    {
        size_t cap = 256;
        for (;;) {
            int n;
            char *tmp = (char *)malloc(cap);
            if (!tmp) {
                va_end(ap);
                sb_free(&sb);
                return NULL;
            }
            n = vsnprintf(tmp, cap, fmt, ap);
            if (n >= 0 && (size_t)n < cap) {
                sb_append_n(&sb, tmp, (size_t)n);
                free(tmp);
                break;
            }
            free(tmp);
            va_end(ap);
            va_start(ap, fmt);
            if (n >= 0) {
                cap = (size_t)n + 1;
            } else {
                cap *= 2;
            }
        }
    }
    va_end(ap);
    out = sb_take(&sb);
    return out;
}

void set_error(char **error, const char *fmt, ...)
{
    va_list ap;
    char *msg = NULL;
    size_t cap = 256;

    if (!error) {
        return;
    }
    for (;;) {
        int n;
        char *tmp = (char *)malloc(cap);
        if (!tmp) {
            break;
        }
        va_start(ap, fmt);
        n = vsnprintf(tmp, cap, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < cap) {
            msg = tmp;
            break;
        }
        free(tmp);
        if (n >= 0) {
            cap = (size_t)n + 1;
        } else {
            cap *= 2;
        }
    }
    free(*error);
    *error = msg ? msg : xstrdup("未知错误");
}
