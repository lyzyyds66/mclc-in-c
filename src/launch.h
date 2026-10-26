#ifndef MCLC_LAUNCH_H
#define MCLC_LAUNCH_H

#include "version.h"

#include <wchar.h>

typedef struct {
    wchar_t *game_dir;
    const char *username;
    const char *uuid;
    const wchar_t *java;
    int memory_mb;
    int width;
    int height;
    int demo;
    int detach;
    int dry_run;
    const char *const *extra_jvm;
    size_t extra_jvm_count;
    const char *const *extra_game;
    size_t extra_game_count;
} LaunchOptions;

int launch_version(const LaunchOptions *options, Version *version,
                   char **error);

wchar_t *launch_find_java(const wchar_t *preferred, int required_major,
                          char **error);

#endif /* MCLC_LAUNCH_H */
