#include "zip.h"
#include "inflate.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_LOCAL_SIGNATURE 0x04034B50u
#define ZIP_CENTRAL_SIGNATURE 0x02014B50u
#define ZIP_EOCD_SIGNATURE 0x06054B50u
#define ZIP_MAX_ENTRY_SIZE ((uint64_t)512 * 1024 * 1024)

static uint32_t g_crc_table[256];
static int g_crc_ready;

static void crc32_init(void)
{
    uint32_t i;
    uint32_t j;
    uint32_t c;

    if (g_crc_ready) {
        return;
    }
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc32_bytes(const unsigned char *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;

    crc32_init();
    for (i = 0; i < size; i++) {
        crc = g_crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Turn an archive path into a safe relative Windows path. Returns an
 * allocated UTF-8 string, or NULL if the entry must be rejected/skipped.
 */
static char *safe_entry_path(const char *name)
{
    StrBuf out;
    const char *p = name;

    if (!name || !*name) {
        return NULL;
    }
    if (name[0] == '/' || name[0] == '\\' || strchr(name, ':')) {
        return NULL;
    }
    sb_init(&out);
    while (*p) {
        const char *start = p;
        size_t len;
        while (*p && *p != '/' && *p != '\\') {
            p++;
        }
        len = (size_t)(p - start);
        if (len == 0) {
            /* Collapse duplicate separators. */
        } else if (start[0] == '.') {
            /* "." / ".." and hidden metadata are not needed for natives. */
            sb_free(&out);
            return NULL;
        } else if (start[len - 1] == '.' || start[len - 1] == ' ') {
            /* Windows strips these, which can turn a name into "..". */
            sb_free(&out);
            return NULL;
        } else {
            if (out.len) {
                sb_append_char(&out, '\\');
            }
            sb_append_n(&out, start, len);
        }
        if (*p) {
            p++;
        }
    }
    if (out.len == 0) {
        sb_free(&out);
        return NULL;
    }
    return sb_take(&out);
}

static int is_skipped_entry(const char *path)
{
    if (!path || !*path) {
        return 1;
    }
    if (_strnicmp(path, "META-INF\\", 9) == 0 ||
        _stricmp(path, "META-INF") == 0) {
        return 1;
    }
    return 0;
}

static int extract_one(const unsigned char *zip, size_t zip_size,
                       const unsigned char *central, const wchar_t *dest_dir,
                       char **error)
{
    uint16_t flags = rd16(central + 8);
    uint16_t method = rd16(central + 10);
    uint32_t expected_crc = rd32(central + 16);
    uint32_t compressed_size = rd32(central + 20);
    uint32_t uncompressed_size = rd32(central + 24);
    uint16_t name_len = rd16(central + 28);
    uint16_t extra_len = rd16(central + 30);
    uint16_t comment_len = rd16(central + 32);
    uint32_t local_offset = rd32(central + 42);
    char *name;
    char *safe;
    wchar_t *safe_wide;
    wchar_t *target;
    unsigned char *output = NULL;
    size_t output_size = 0;
    uint32_t crc;
    size_t local_data;

    if (flags & 0x0001) {
        set_error(error, "ZIP 条目已加密");
        return -1;
    }
    if (name_len == 0) {
        return 0;
    }
    if ((size_t)local_offset + 30 > zip_size) {
        set_error(error, "ZIP 本地文件头越界");
        return -1;
    }
    (void)extra_len;
    (void)comment_len;

    name = xstrndup((const char *)central + 46, name_len);
    safe = safe_entry_path(name);
    if (!safe) {
        free(name);
        return 0;
    }
    if (name_len && name[name_len - 1] == '/') {
        free(safe);
        free(name);
        return 0;
    }
    if (is_skipped_entry(safe)) {
        free(safe);
        free(name);
        return 0;
    }
    if (compressed_size == 0xFFFFFFFFu || uncompressed_size == 0xFFFFFFFFu) {
        set_error(error, "不支持 ZIP64 条目: %s", name);
        free(safe);
        free(name);
        return -1;
    }
    if (uncompressed_size > ZIP_MAX_ENTRY_SIZE) {
        set_error(error, "ZIP 条目过大: %s", name);
        free(safe);
        free(name);
        return -1;
    }
    if (rd32(zip + local_offset) != ZIP_LOCAL_SIGNATURE) {
        set_error(error, "ZIP 本地文件头签名错误: %s", name);
        free(safe);
        free(name);
        return -1;
    }
    local_data = (size_t)local_offset + 30 +
                 rd16(zip + local_offset + 26) +
                 rd16(zip + local_offset + 28);
    if (local_data + compressed_size > zip_size) {
        set_error(error, "ZIP 压缩数据越界: %s", name);
        free(safe);
        free(name);
        return -1;
    }

    if (method == 0) {
        if (compressed_size != uncompressed_size) {
            set_error(error, "ZIP 存储条目大小不一致: %s", name);
            free(safe);
            free(name);
            return -1;
        }
        output = (unsigned char *)xmalloc((size_t)uncompressed_size + 1);
        memcpy(output, zip + local_data, uncompressed_size);
        output_size = uncompressed_size;
    } else if (method == 8) {
        char *inflate_error = NULL;
        if (mclc_inflate_raw(zip + local_data, compressed_size,
                             uncompressed_size, &output, &output_size,
                             &inflate_error) != 0) {
            set_error(error, "解压 ZIP 条目 %s 失败: %s", name,
                      inflate_error ? inflate_error : "未知错误");
            free(inflate_error);
            free(safe);
            free(name);
            return -1;
        }
        if (output_size != uncompressed_size) {
            set_error(error, "ZIP 条目解压大小不符: %s", name);
            free(output);
            free(safe);
            free(name);
            return -1;
        }
    } else {
        set_error(error, "不支持的 ZIP 压缩方法 %u: %s",
                  (unsigned)method, name);
        free(safe);
        free(name);
        return -1;
    }

    crc = crc32_bytes(output, output_size);
    if (crc != expected_crc) {
        set_error(error, "ZIP 条目 CRC 校验失败: %s", name);
        free(output);
        free(safe);
        free(name);
        return -1;
    }

    safe_wide = utf8_to_wide(safe);
    if (!safe_wide) {
        set_error(error, "ZIP 条目文件名编码无效: %s", name);
        free(output);
        free(safe);
        free(name);
        return -1;
    }
    target = path_join(dest_dir, safe_wide);
    if (!target || write_file_all(target, output, output_size) != 0) {
        set_error(error, "无法写入 ZIP 条目: %ls", target ? target : L"(null)");
        free(target);
        free(safe_wide);
        free(output);
        free(safe);
        free(name);
        return -1;
    }
    free(target);
    free(safe_wide);
    free(output);
    free(safe);
    free(name);
    return 1;
}

int zip_extract_all(const wchar_t *zip_path, const wchar_t *dest_dir,
                    char **error)
{
    char *data;
    size_t size;
    size_t eocd = (size_t)-1;
    size_t i;
    size_t cd_offset;
    size_t cd_size;
    unsigned entries;
    size_t pos;
    int extracted = 0;
    unsigned n;

    data = read_text_file(zip_path, &size, error);
    if (!data) {
        return -1;
    }
    if (size < 22) {
        set_error(error, "ZIP 文件过小: %ls", zip_path);
        free(data);
        return -1;
    }

    i = size >= 65557 ? size - 65557 : 0;
    for (;;) {
        if (i + 22 <= size && rd32((unsigned char *)data + i) ==
                                   ZIP_EOCD_SIGNATURE) {
            uint16_t comment_len = rd16((unsigned char *)data + i + 20);
            size_t end = i + 22 + (size_t)comment_len;
            /*
             * The ZIP specification puts EOCD at the very end, but some
             * older JARs carry trailing data. Accept the last EOCD whose
             * declared comment is in bounds; the central directory bounds
             * are checked below.
             */
            if (end <= size) {
                eocd = i;
                break;
            }
        }
        if (i == 0) {
            break;
        }
        i--;
    }
    if (eocd == (size_t)-1) {
        set_error(error, "找不到 ZIP 中央目录: %ls", zip_path);
        free(data);
        return -1;
    }
    {
        unsigned char *e = (unsigned char *)data + eocd;
        uint16_t disk = rd16(e + 4);
        uint16_t cd_disk = rd16(e + 6);
        uint16_t disk_entries = rd16(e + 8);
        entries = rd16(e + 10);
        cd_size = rd32(e + 12);
        cd_offset = rd32(e + 16);
        if (disk != 0 || cd_disk != 0 || disk_entries != entries) {
            set_error(error, "不支持分卷 ZIP: %ls", zip_path);
            free(data);
            return -1;
        }
    }
    if (cd_offset > size || cd_size > size - cd_offset) {
        set_error(error, "ZIP 中央目录越界: %ls", zip_path);
        free(data);
        return -1;
    }

    if (mkdir_p(dest_dir) != 0) {
        set_error(error, "无法创建目录: %ls", dest_dir);
        free(data);
        return -1;
    }

    pos = cd_offset;
    for (n = 0; n < entries; n++) {
        unsigned char *central;
        uint16_t name_len;
        uint16_t extra_len;
        uint16_t comment_len;
        int rc;

        if (pos + 46 > size || rd32((unsigned char *)data + pos) !=
                                   ZIP_CENTRAL_SIGNATURE) {
            set_error(error, "ZIP 中央目录损坏: %ls", zip_path);
            free(data);
            return -1;
        }
        central = (unsigned char *)data + pos;
        name_len = rd16(central + 28);
        extra_len = rd16(central + 30);
        comment_len = rd16(central + 32);
        if (pos + 46u + name_len + extra_len + comment_len > size) {
            set_error(error, "ZIP 中央目录条目越界: %ls", zip_path);
            free(data);
            return -1;
        }
        rc = extract_one((unsigned char *)data, size, central, dest_dir,
                         error);
        if (rc < 0) {
            free(data);
            return -1;
        }
        extracted += rc;
        pos += 46u + name_len + extra_len + comment_len;
    }

    free(data);
    return extracted;
}
