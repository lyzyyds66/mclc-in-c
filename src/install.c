#include "install.h"
#include "hash.h"
#include "http.h"
#include "util.h"
#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wchar_t *join_utf8(const wchar_t *base, const char *relative)
{
    wchar_t *rel = utf8_to_wide(relative);
    wchar_t *out;

    if (!rel) {
        return NULL;
    }
    out = path_join(base, rel);
    free(rel);
    return out;
}

static void slash_to_backslash(wchar_t *path)
{
    if (!path) {
        return;
    }
    while (*path) {
        if (*path == L'/') {
            *path = L'\\';
        }
        path++;
    }
}

static int is_hex_string(const char *text, size_t length)
{
    size_t i;
    if (!text || strlen(text) != length) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static int sha1_matches(const wchar_t *path, const char *expected)
{
    char actual[41];
    if (!expected || !*expected) {
        return 1;
    }
    if (sha1_file_hex(path, actual) != 0) {
        return 0;
    }
    return _stricmp(actual, expected) == 0;
}

/*
 * Download a file when it is missing, has the wrong size, or (when verify is
 * requested) has the wrong SHA-1. New downloads are always checksum-verified.
 */
static int ensure_file(const wchar_t *path, const char *url, const char *sha1,
                       long long size, int verify, int show_progress,
                       int quiet, const char *label, char **error)
{
    int exists;
    int needs_download = 0;
    wchar_t *wide_url;
    char actual[41];

    if (!path) {
        set_error(error, "内部错误：文件路径为空（%s）", label);
        return -1;
    }
    exists = path_exists(path);
    if (exists && path_is_dir(path)) {
        exists = 0;
    }
    if (exists) {
        int64_t actual_size = path_size(path);
        if (size > 0 && actual_size != size) {
            needs_download = 1;
            if (!quiet) printf("  ! 大小不符，重新下载: %s\n", label);
        } else if (verify && sha1 && *sha1 && !sha1_matches(path, sha1)) {
            needs_download = 1;
            if (!quiet) printf("  ! SHA-1 不符，重新下载: %s\n", label);
        } else {
            return 0;
        }
    } else {
        needs_download = 1;
    }

    if (!needs_download) {
        return 0;
    }
    if (!url || !*url) {
        set_error(error, "缺少下载地址，无法获取: %s", label);
        return -1;
    }
    wide_url = utf8_to_wide(url);
    if (!wide_url) {
        set_error(error, "下载 URL 编码无效: %s", url);
        return -1;
    }
    if (!show_progress && !quiet) {
        printf("  + %s\n", label);
        fflush(stdout);
    }
    if (http_download(wide_url, path, show_progress, size, error) != 0) {
        char *detail = (error && *error) ? xstrdup(*error) : NULL;
        set_error(error, "下载 %s 失败%s%s", label, detail ? ": " : "",
                  detail ? detail : "");
        free(detail);
        free(wide_url);
        return -1;
    }
    free(wide_url);
    if (sha1 && *sha1) {
        if (sha1_file_hex(path, actual) != 0 ||
            _stricmp(actual, sha1) != 0) {
            DeleteFileW(path);
            set_error(error, "SHA-1 校验失败: %s（期望 %s）", label, sha1);
            return -1;
        }
    }
    return 0;
}

static int asset_name_is_safe(const char *name)
{
    const char *p = name;

    if (!name || !*name || name[0] == '/' || name[0] == '\\' ||
        strchr(name, ':') || strchr(name, '\\')) {
        return 0;
    }
    while (*p) {
        const char *start = p;
        size_t len;
        while (*p && *p != '/') {
            p++;
        }
        len = (size_t)(p - start);
        if (len == 0) {
            return 0;
        }
        if ((len == 1 && start[0] == '.') ||
            (len == 2 && start[0] == '.' && start[1] == '.')) {
            return 0;
        }
        if (start[len - 1] == '.' || start[len - 1] == ' ') {
            return 0;
        }
        if (*p) {
            p++;
        }
    }
    return 1;
}

static int copy_asset_to(const wchar_t *source, const wchar_t *dest,
                         char **error)
{
    int64_t source_size = path_size(source);
    if (path_exists(dest) && path_size(dest) == source_size) {
        return 0;
    }
    if (ensure_parent_dir(dest) != 0) {
        set_error(error, "无法创建资源目录: %ls", dest);
        return -1;
    }
    if (!CopyFileW(source, dest, FALSE)) {
        set_error(error, "无法复制资源到 %ls", dest);
        return -1;
    }
    return 0;
}

static int install_assets(Version *version, const wchar_t *game_dir,
                          int verify, char **error)
{
    wchar_t *assets_dir = path_join(game_dir, L"assets");
    wchar_t *indexes_dir = path_join(assets_dir, L"indexes");
    wchar_t *objects_dir = path_join(assets_dir, L"objects");
    wchar_t *index_path;
    JsonValue *index;
    JsonValue *objects;
    size_t total;
    size_t processed = 0;
    size_t downloaded = 0;
    int virtual_assets;
    int map_to_resources;
    int rc = -1;
    JsonMember *m;

    if (mkdir_p(indexes_dir) != 0 || mkdir_p(objects_dir) != 0) {
        set_error(error, "无法创建 assets 目录");
        goto done;
    }
    if (!version->asset_index_id ||
        !path_component_is_safe(version->asset_index_id)) {
        set_error(error, "版本缺少资源索引 ID 或 ID 不安全");
        goto done;
    }
    index_path = join_utf8(indexes_dir, version->asset_index_id);
    if (!index_path) {
        set_error(error, "资源索引 ID 无效: %s", version->asset_index_id);
        goto done;
    }
    {
        size_t len = wcslen(index_path);
        wchar_t *with_ext = (wchar_t *)xmalloc((len + 6) * sizeof(wchar_t));
        wcscpy(with_ext, index_path);
        wcscat(with_ext, L".json");
        free(index_path);
        index_path = with_ext;
    }
    if (!path_exists(index_path)) {
        if (!version->asset_index.url || !*version->asset_index.url) {
            set_error(error, "缺少资源索引且本地不存在: %ls", index_path);
            free(index_path);
            goto done;
        }
    }
    if (ensure_file(index_path, version->asset_index.url,
                    version->asset_index.sha1, version->asset_index.size,
                    verify, 0, 0, "资源索引", error) != 0) {
        free(index_path);
        goto done;
    }
    index = json_parse_file(index_path, error);
    free(index_path);
    if (!index) {
        goto done;
    }
    objects = json_get(index, "objects");
    if (!objects || objects->type != JSON_OBJECT) {
        set_error(error, "资源索引缺少 objects 对象");
        json_free(index);
        goto done;
    }

    total = json_object_size(objects);
    printf("  [资源] %zu 个对象\n", total);
    for (m = objects->u.object.head; m; m = m->next) {
        JsonValue *object = m->value;
        const char *hash;
        long long size;
        wchar_t *object_path;
        wchar_t *object_dir;
        char prefix[3];
        char *url;
        StrBuf ub;
        int exists;
        int64_t actual_size;

        processed++;
        if (object->type != JSON_OBJECT) {
            continue;
        }
        hash = json_get_string(object, "hash");
        size = json_get_i64(object, "size", 0);
        if (!is_hex_string(hash, 40)) {
            set_error(error, "资源对象哈希无效: %s", m->key);
            json_free(index);
            goto done;
        }
        if (!asset_name_is_safe(m->key)) {
            set_error(error, "资源名称不安全: %s", m->key);
            json_free(index);
            goto done;
        }
        prefix[0] = hash[0];
        prefix[1] = hash[1];
        prefix[2] = '\0';
        object_dir = join_utf8(objects_dir, prefix);
        object_path = join_utf8(object_dir, hash);
        free(object_dir);
        exists = path_exists(object_path);
        actual_size = exists ? path_size(object_path) : -1;
        if (!exists || (size > 0 && actual_size != size) ||
            (verify && !sha1_matches(object_path, hash))) {
            sb_init(&ub);
            sb_append(&ub, "https://resources.download.minecraft.net/");
            sb_append(&ub, prefix);
            sb_append_char(&ub, '/');
            sb_append(&ub, hash);
            url = sb_take(&ub);
            if (ensure_file(object_path, url, hash, size, verify, 0, 1,
                            m->key, error) != 0) {
                free(url);
                free(object_path);
                json_free(index);
                goto done;
            }
            free(url);
            downloaded++;
            if (downloaded % 25 == 0 || processed == total) {
                printf("\r  [资源] %zu/%zu（新下载 %zu）", processed, total,
                       downloaded);
                fflush(stdout);
            }
        }
        free(object_path);
    }
    if (downloaded) {
        printf("\r  [资源] %zu/%zu（新下载 %zu）\n", processed, total,
               downloaded);
    }

    virtual_assets = json_get_bool(index, "virtual", 0);
    map_to_resources = json_get_bool(index, "map_to_resources", 0);
    if (virtual_assets || map_to_resources) {
        wchar_t *virtual_root = NULL;
        wchar_t *resources_root = NULL;
        size_t copied = 0;

        if (virtual_assets) {
            wchar_t *base = path_join(assets_dir, L"virtual");
            wchar_t *id = utf8_to_wide(version->asset_index_id);
            virtual_root = id ? path_join(base, id) : NULL;
            free(base);
            free(id);
        }
        if (map_to_resources) {
            resources_root = path_join(game_dir, L"resources");
        }
        for (m = objects->u.object.head; m; m = m->next) {
            JsonValue *object = m->value;
            const char *hash;
            char prefix[3];
            wchar_t *object_path;
            wchar_t *object_dir;
            wchar_t *source;

            if (object->type != JSON_OBJECT) continue;
            hash = json_get_string(object, "hash");
            if (!is_hex_string(hash, 40) || !asset_name_is_safe(m->key)) {
                continue;
            }
            prefix[0] = hash[0];
            prefix[1] = hash[1];
            prefix[2] = '\0';
            object_dir = join_utf8(objects_dir, prefix);
            object_path = join_utf8(object_dir, hash);
            free(object_dir);
            source = object_path;
            if (virtual_root) {
                wchar_t *rel = utf8_to_wide(m->key);
                wchar_t *dest = path_join(virtual_root, rel);
                if (copy_asset_to(source, dest, error) != 0) {
                    free(dest);
                    free(rel);
                    free(source);
                    free(virtual_root);
                    free(resources_root);
                    json_free(index);
                    goto done;
                }
                free(dest);
                free(rel);
                copied++;
            }
            if (resources_root) {
                wchar_t *rel = utf8_to_wide(m->key);
                wchar_t *dest = path_join(resources_root, rel);
                if (copy_asset_to(source, dest, error) != 0) {
                    free(dest);
                    free(rel);
                    free(source);
                    free(virtual_root);
                    free(resources_root);
                    json_free(index);
                    goto done;
                }
                free(dest);
                free(rel);
                copied++;
            }
            free(source);
        }
        if (copied) {
            printf("  [资源] 已映射 %zu 个旧版资源文件\n", copied);
        }
        free(virtual_root);
        free(resources_root);
    }
    json_free(index);
    rc = 0;

done:
    free(assets_dir);
    free(indexes_dir);
    free(objects_dir);
    return rc;
}

int install_version(Version *version, const wchar_t *game_dir, int verify,
                    char **error)
{
    wchar_t *versions_dir;
    wchar_t *version_dir;
    wchar_t *natives_dir;
    wchar_t *libraries_dir;
    wchar_t *assets_dir;
    wchar_t *log_dir;
    wchar_t *client_path = NULL;
    wchar_t *client_id_wide;
    int i;
    size_t j;
    int native_count = 0;

    if (!version || !version->id) {
        set_error(error, "版本数据为空");
        return -1;
    }
    if (!path_component_is_safe(version->id)) {
        set_error(error, "版本 ID 不安全: %s", version->id);
        return -1;
    }
    if (version->logging_id &&
        !path_component_is_safe(version->logging_id)) {
        set_error(error, "日志配置 ID 不安全: %s", version->logging_id);
        return -1;
    }
    if (!path_component_is_safe(version->jar_id)) {
        set_error(error, "客户端 jar 名称不安全");
        return -1;
    }
    versions_dir = path_join(game_dir, L"versions");
    version_dir = join_utf8(versions_dir, version->id);
    natives_dir = path_join(version_dir, L"natives");
    libraries_dir = path_join(game_dir, L"libraries");
    assets_dir = path_join(game_dir, L"assets");
    log_dir = path_join(assets_dir, L"log_configs");
    if (mkdir_p(version_dir) != 0 || mkdir_p(natives_dir) != 0 ||
        mkdir_p(libraries_dir) != 0 || mkdir_p(assets_dir) != 0 ||
        mkdir_p(log_dir) != 0) {
        set_error(error, "无法创建游戏目录结构");
        goto fail;
    }

    client_id_wide = utf8_to_wide(version->jar_id);
    if (!client_id_wide) {
        set_error(error, "客户端 jar 名称无效: %s",
                  version->jar_id ? version->jar_id : "(null)");
        goto fail;
    }
    client_path = path_join3(versions_dir, client_id_wide, client_id_wide);
    if (!client_path) {
        free(client_id_wide);
        set_error(error, "无法构造客户端 jar 路径");
        goto fail;
    }
    {
        size_t len = wcslen(client_path);
        wchar_t *with_ext = (wchar_t *)xmalloc((len + 6) * sizeof(wchar_t));
        wcscpy(with_ext, client_path);
        wcscat(with_ext, L".jar");
        free(client_path);
        client_path = with_ext;
    }
    free(client_id_wide);

    printf("[*] 检查客户端 jar...\n");
    if (ensure_file(client_path, version->client.url, version->client.sha1,
                    version->client.size, verify, 1, 0,
                    "客户端 jar", error) != 0) {
        goto fail;
    }

    printf("[*] 检查 %d 个版本层、游戏库与本地库...\n",
           version->layer_count);
    for (i = 0; i < version->layer_count; i++) {
        VersionLayer *layer = &version->layers[i];
        for (j = 0; j < layer->library_count; j++) {
            Library *lib = &layer->libraries[j];
            if (lib->path && !path_relative_is_safe(lib->path)) {
                set_error(error, "游戏库路径不安全: %s", lib->path);
                goto fail;
            }
            if (lib->native_path && !path_relative_is_safe(lib->native_path)) {
                set_error(error, "本地库路径不安全: %s", lib->native_path);
                goto fail;
            }
            if (lib->path) {
                wchar_t *lib_path = join_utf8(libraries_dir, lib->path);
                if (!lib_path) {
                    set_error(error, "游戏库路径无效: %s", lib->path);
                    goto fail;
                }
                slash_to_backslash(lib_path);
                if (ensure_file(lib_path, lib->url, lib->sha1, lib->size,
                                verify, 0, 0, lib->name, error) != 0) {
                    free(lib_path);
                    goto fail;
                }
                free(lib_path);
            }
            if (lib->native_path) {
                wchar_t *native_jar = join_utf8(libraries_dir,
                                                lib->native_path);
                if (!native_jar) {
                    set_error(error, "本地库路径无效: %s", lib->native_path);
                    goto fail;
                }
                slash_to_backslash(native_jar);
                if (ensure_file(native_jar, lib->native_url, lib->native_sha1,
                                lib->native_size, verify, 0, 0,
                                lib->native_classifier ?
                                lib->native_classifier : "natives",
                                error) != 0) {
                    free(native_jar);
                    goto fail;
                }
                if (zip_extract_all(native_jar, natives_dir, error) < 0) {
                    char *detail = (error && *error) ? xstrdup(*error) : NULL;
                    set_error(error, "解压本地库失败（%s）%s%s", lib->name,
                              detail ? ": " : "", detail ? detail : "");
                    free(detail);
                    free(native_jar);
                    goto fail;
                }
                native_count++;
                free(native_jar);
            }
        }
    }
    printf("  natives: %d 个本地库已解压到 versions\\%s\\natives\n",
           native_count, version->id);

    printf("[*] 检查资源文件（首次启动可能较慢）...\n");
    if (install_assets(version, game_dir, verify, error) != 0) {
        goto fail;
    }

    if (version->logging_id) {
        wchar_t *log_path = join_utf8(log_dir, version->logging_id);
        if (!log_path) {
            set_error(error, "日志配置路径无效: %s", version->logging_id);
            goto fail;
        }
        if (ensure_file(log_path, version->logging.url, version->logging.sha1,
                        version->logging.size, verify, 0, 0, "日志配置",
                        error) != 0) {
            free(log_path);
            goto fail;
        }
        free(log_path);
    }

    free(versions_dir);
    free(version_dir);
    free(natives_dir);
    free(libraries_dir);
    free(assets_dir);
    free(log_dir);
    free(client_path);
    return 0;

fail:
    free(versions_dir);
    free(version_dir);
    free(natives_dir);
    free(libraries_dir);
    free(assets_dir);
    free(log_dir);
    free(client_path);
    return -1;
}
