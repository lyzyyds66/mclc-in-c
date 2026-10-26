#include "remote.h"
#include "hash.h"
#include "http.h"
#include "json.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The GUI captures stderr; route all progress text there. */
#define printf(...) fprintf(stderr, __VA_ARGS__)

#define MCLC_VERSION_MANIFEST_URL \
    L"https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

static int version_id_safe(const char *id)
{
    return path_component_is_safe(id);
}

wchar_t *remote_version_json_path(const wchar_t *game_dir, const char *id)
{
    wchar_t *id_wide;
    wchar_t *versions_dir;
    wchar_t *version_dir;
    wchar_t *json_path;
    size_t len;
    wchar_t *with_ext;

    if (!version_id_safe(id)) {
        return NULL;
    }
    id_wide = utf8_to_wide(id);
    if (!id_wide) {
        return NULL;
    }
    versions_dir = path_join(game_dir, L"versions");
    version_dir = path_join(versions_dir, id_wide);
    json_path = path_join(version_dir, id_wide);
    len = wcslen(json_path);
    with_ext = (wchar_t *)xmalloc((len + 6) * sizeof(wchar_t));
    wcscpy(with_ext, json_path);
    wcscat(with_ext, L".json");
    free(json_path);
    free(version_dir);
    free(versions_dir);
    free(id_wide);
    return with_ext;
}

int remote_manifest_fetch(RemoteManifest *manifest, char **error)
{
    char *data = NULL;
    size_t size = 0;
    JsonValue *root;
    JsonValue *latest;
    JsonValue *versions;
    size_t i;

    memset(manifest, 0, sizeof(*manifest));
    if (http_get_string(MCLC_VERSION_MANIFEST_URL, &data, &size,
                        16u * 1024u * 1024u, error) != 0) {
        return -1;
    }
    root = json_parse(data, size, error);
    free(data);
    if (!root) {
        return -1;
    }
    latest = json_get(root, "latest");
    if (latest && latest->type == JSON_OBJECT) {
        const char *release = json_get_string(latest, "release");
        const char *snapshot = json_get_string(latest, "snapshot");
        if (release) manifest->latest_release = xstrdup(release);
        if (snapshot) manifest->latest_snapshot = xstrdup(snapshot);
    }
    versions = json_get(root, "versions");
    if (!versions || versions->type != JSON_ARRAY) {
        set_error(error, "版本清单缺少 versions 数组");
        json_free(root);
        remote_manifest_free(manifest);
        return -1;
    }
    manifest->versions = (RemoteVersion *)xcalloc(
        versions->u.array.count, sizeof(RemoteVersion));
    for (i = 0; i < versions->u.array.count; i++) {
        JsonValue *item = versions->u.array.items[i];
        const char *id;
        const char *url;
        const char *type;
        const char *sha1;
        RemoteVersion *entry;

        if (item->type != JSON_OBJECT) {
            continue;
        }
        id = json_get_string(item, "id");
        url = json_get_string(item, "url");
        if (!id || !url || !version_id_safe(id)) {
            continue;
        }
        entry = &manifest->versions[manifest->count++];
        entry->id = xstrdup(id);
        entry->url = xstrdup(url);
        type = json_get_string(item, "type");
        entry->type = xstrdup(type ? type : "release");
        sha1 = json_get_string(item, "sha1");
        if (sha1) {
            entry->sha1 = xstrdup(sha1);
        }
    }
    json_free(root);
    return 0;
}

void remote_manifest_free(RemoteManifest *manifest)
{
    size_t i;
    if (!manifest) {
        return;
    }
    for (i = 0; i < manifest->count; i++) {
        free(manifest->versions[i].id);
        free(manifest->versions[i].type);
        free(manifest->versions[i].url);
        free(manifest->versions[i].sha1);
    }
    free(manifest->versions);
    free(manifest->latest_release);
    free(manifest->latest_snapshot);
    memset(manifest, 0, sizeof(*manifest));
}

const RemoteVersion *remote_find(const RemoteManifest *manifest,
                                 const char *id)
{
    size_t i;
    const char *wanted = id;

    if (!manifest || !id) {
        return NULL;
    }
    if (strcmp(id, "latest") == 0) {
        wanted = manifest->latest_release;
    } else if (strcmp(id, "latest-snapshot") == 0) {
        wanted = manifest->latest_snapshot;
    }
    if (!wanted) {
        return NULL;
    }
    for (i = 0; i < manifest->count; i++) {
        if (strcmp(manifest->versions[i].id, wanted) == 0) {
            return &manifest->versions[i];
        }
    }
    return NULL;
}

int remote_install_json(const wchar_t *game_dir, const RemoteVersion *entry,
                        int force, int verify, char **error)
{
    wchar_t *path;
    wchar_t *url;
    char actual[41];

    if (!entry || !entry->id || !entry->url) {
        set_error(error, "版本清单条目无效");
        return -1;
    }
    path = remote_version_json_path(game_dir, entry->id);
    if (!path) {
        set_error(error, "版本 ID 无效: %s", entry->id);
        return -1;
    }
    if (path_exists(path) && !force) {
        if (!verify || !entry->sha1 ||
            (sha1_file_hex(path, actual) == 0 &&
             _stricmp(actual, entry->sha1) == 0)) {
            printf("  [版本元数据] 已存在且校验通过: %s\n", entry->id);
            free(path);
            return 0;
        }
        printf("  ! 版本 JSON SHA-1 不符，删除并重新下载: %s\n", entry->id);
        DeleteFileW(path);
    }
    url = utf8_to_wide(entry->url);
    if (!url) {
        set_error(error, "版本 JSON URL 无效");
        free(path);
        return -1;
    }
    printf("  [版本元数据] %s\n", entry->id);
    printf("          URL: %s\n", entry->url);
    if (http_download(url, path, 0, 0, error) != 0) {
        free(url);
        free(path);
        return -1;
    }
    free(url);
    if (entry->sha1 && *entry->sha1) {
        if (sha1_file_hex(path, actual) != 0 ||
            _stricmp(actual, entry->sha1) != 0) {
            DeleteFileW(path);
            set_error(error, "版本 JSON SHA-1 校验失败: %s", entry->id);
            free(path);
            return -1;
        }
    }
    printf("  [版本元数据] 下载完成: %s\n", entry->id);
    free(path);
    return 0;
}
