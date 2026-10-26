#ifndef MCLC_REMOTE_H
#define MCLC_REMOTE_H

#include <stddef.h>
#include <wchar.h>

typedef struct {
    char *id;
    char *type;
    char *url;
    char *sha1;
} RemoteVersion;

typedef struct {
    RemoteVersion *versions;
    size_t count;
    char *latest_release;
    char *latest_snapshot;
} RemoteManifest;

int remote_manifest_fetch(RemoteManifest *manifest, char **error);
void remote_manifest_free(RemoteManifest *manifest);

/* Resolve an exact version id, "latest" or "latest-snapshot". */
const RemoteVersion *remote_find(const RemoteManifest *manifest,
                                 const char *id);

/* Download versions/<id>/<id>.json if it is missing or force is set. */
int remote_install_json(const wchar_t *game_dir, const RemoteVersion *entry,
                        int force, int verify, char **error);

wchar_t *remote_version_json_path(const wchar_t *game_dir, const char *id);

#endif /* MCLC_REMOTE_H */
