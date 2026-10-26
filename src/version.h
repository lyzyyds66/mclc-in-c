#ifndef MCLC_VERSION_H
#define MCLC_VERSION_H

#include "json.h"

#include <stddef.h>
#include <wchar.h>

#define MCLC_MAX_VERSION_LAYERS 8

typedef struct {
    int present;
    char *path;
    char *url;
    char *sha1;
    long long size;
} DownloadInfo;

typedef struct {
    char *name;
    char *path;
    char *url;
    char *sha1;
    long long size;
    char *native_classifier;
    char *native_path;
    char *native_url;
    char *native_sha1;
    long long native_size;
} Library;

typedef struct {
    JsonValue *root;
    Library *libraries;
    size_t library_count;
} VersionLayer;

typedef struct {
    int demo;
    int custom_resolution;
    int quick_plays_support;
    int quick_play_singleplayer;
    int quick_play_multiplayer;
} Features;

typedef struct {
    char *id;
    char *main_class;
    char *assets;
    char *jar_id;
    char *version_type;
    char *asset_index_id;
    char *logging_id;
    char *logging_argument;
    char *legacy_arguments;
    DownloadInfo client;
    DownloadInfo asset_index;
    DownloadInfo logging;
    int java_major;
    VersionLayer layers[MCLC_MAX_VERSION_LAYERS];
    int layer_count;
} Version;

int version_load(const wchar_t *game_dir, const char *version_id,
                 Version *out, char **error);
void version_free(Version *version);

int version_rules_allow(const JsonValue *rules, const Features *features);
const char *version_os_name(void);
const char *version_os_arch(void);
int version_is_windows_arm64(void);

/* "group:artifact:version[:classifier][@ext]" -> Maven repository path. */
char *version_maven_path(const char *name, const char *classifier);

/* Replace ${key} placeholders using the supplied parallel arrays. */
char *version_substitute(const char *input, const char *const *keys,
                         const char *const *values, size_t count);

#endif /* MCLC_VERSION_H */
