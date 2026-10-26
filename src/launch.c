#include "launch.h"
#include "hash.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} ArgList;

typedef struct {
    const char *keys[32];
    const char *values[32];
    size_t count;
} Vars;

static void args_init(ArgList *args)
{
    memset(args, 0, sizeof(*args));
}

static void args_free(ArgList *args)
{
    size_t i;
    for (i = 0; i < args->count; i++) {
        free(args->items[i]);
    }
    free(args->items);
    memset(args, 0, sizeof(*args));
}

static void args_add(ArgList *args, const char *value)
{
    if (args->count == args->cap) {
        size_t cap = args->cap ? args->cap * 2 : 32;
        args->items = (char **)xrealloc(args->items,
                                        cap * sizeof(char *));
        args->cap = cap;
    }
    args->items[args->count++] = xstrdup(value ? value : "");
}

static void args_add_owned(ArgList *args, char *value)
{
    if (!value) {
        value = xstrdup("");
    }
    if (args->count == args->cap) {
        size_t cap = args->cap ? args->cap * 2 : 32;
        args->items = (char **)xrealloc(args->items,
                                        cap * sizeof(char *));
        args->cap = cap;
    }
    args->items[args->count++] = value;
}

static int args_has_exact(const ArgList *args, const char *value)
{
    size_t i;
    for (i = 0; i < args->count; i++) {
        if (strcmp(args->items[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int args_has_prefix(const ArgList *args, const char *prefix)
{
    size_t i;
    size_t len = strlen(prefix);
    for (i = 0; i < args->count; i++) {
        if (strncmp(args->items[i], prefix, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void vars_init(Vars *vars)
{
    memset(vars, 0, sizeof(*vars));
}

static void vars_add(Vars *vars, const char *key, const char *value)
{
    if (vars->count >= sizeof(vars->keys) / sizeof(vars->keys[0])) {
        return;
    }
    vars->keys[vars->count] = key;
    vars->values[vars->count] = value ? value : "";
    vars->count++;
}

static const char *vars_get(const Vars *vars, const char *key, size_t len)
{
    size_t i;
    for (i = 0; i < vars->count; i++) {
        if (strlen(vars->keys[i]) == len &&
            memcmp(vars->keys[i], key, len) == 0) {
            return vars->values[i];
        }
    }
    return "";
}

static char *substitute(const char *text, const Vars *vars)
{
    StrBuf sb;
    size_t i = 0;
    size_t len;

    if (!text) {
        return xstrdup("");
    }
    len = strlen(text);
    sb_init(&sb);
    while (i < len) {
        if (text[i] == '$' && i + 1 < len && text[i + 1] == '{') {
            size_t start = i + 2;
            size_t end = start;
            while (end < len && text[end] != '}') {
                end++;
            }
            if (end < len) {
                sb_append(&sb, vars_get(vars, text + start, end - start));
                i = end + 1;
                continue;
            }
        }
        sb_append_char(&sb, text[i++]);
    }
    return sb_take(&sb);
}

static wchar_t *join_utf8_path(const wchar_t *base, const char *relative)
{
    wchar_t *rel = utf8_to_wide(relative);
    wchar_t *result;

    if (!rel) {
        return NULL;
    }
    result = path_join(base, rel);
    free(rel);
    return result;
}

static char *path_to_utf8_alloc(const wchar_t *wide)
{
    return wide_to_utf8(wide);
}

static int classpath_contains(char **items, size_t count, const char *value)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (_stricmp(items[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int classpath_add(StrBuf *cp, char ***seen, size_t *seen_count,
                         const wchar_t *path, char **error)
{
    char *utf8;
    char **new_seen;

    if (!path_exists(path)) {
        set_error(error, "缺少游戏文件: %ls", path);
        return -1;
    }
    utf8 = path_to_utf8_alloc(path);
    if (!utf8) {
        set_error(error, "路径编码无效: %ls", path);
        return -1;
    }
    if (classpath_contains(*seen, *seen_count, utf8)) {
        free(utf8);
        return 0;
    }
    new_seen = (char **)xrealloc(*seen, (*seen_count + 1) * sizeof(char *));
    *seen = new_seen;
    (*seen)[(*seen_count)++] = utf8;
    if (cp->len) {
        sb_append_char(cp, ';');
    }
    sb_append(cp, utf8);
    return 0;
}

static int build_classpath(Version *version, const wchar_t *game_dir,
                           char **out, char **error)
{
    wchar_t *versions_dir = path_join(game_dir, L"versions");
    wchar_t *libraries_dir = path_join(game_dir, L"libraries");
    wchar_t *client_id = utf8_to_wide(version->jar_id);
    wchar_t *client_path = client_id ?
        path_join3(versions_dir, client_id, client_id) : NULL;
    wchar_t *client_jar = NULL;
    StrBuf cp;
    char **seen = NULL;
    size_t seen_count = 0;
    int i;
    size_t j;
    int rc = -1;

    sb_init(&cp);
    if (!client_path) {
        set_error(error, "客户端 jar 路径无效");
        goto done;
    }
    {
        size_t len = wcslen(client_path);
        client_jar = (wchar_t *)xmalloc((len + 5) * sizeof(wchar_t));
        wcscpy(client_jar, client_path);
        wcscat(client_jar, L".jar");
    }
    for (i = version->layer_count - 1; i >= 0; i--) {
        VersionLayer *layer = &version->layers[i];
        for (j = 0; j < layer->library_count; j++) {
            Library *lib = &layer->libraries[j];
            wchar_t *path;
            if (!lib->path) {
                continue;
            }
            if (!path_relative_is_safe(lib->path)) {
                set_error(error, "游戏库路径不安全: %s", lib->path);
                goto done;
            }
            path = join_utf8_path(libraries_dir, lib->path);
            if (!path) {
                set_error(error, "本地库路径无效: %s", lib->path);
                goto done;
            }
            {
                wchar_t *p;
                for (p = path; *p; p++) {
                    if (*p == L'/') *p = L'\\';
                }
            }
            if (classpath_add(&cp, &seen, &seen_count, path, error) != 0) {
                free(path);
                goto done;
            }
            free(path);
        }
    }
    if (classpath_add(&cp, &seen, &seen_count, client_jar, error) != 0) {
        goto done;
    }
    *out = sb_take(&cp);
    rc = 0;

done:
    {
        size_t k;
        for (k = 0; k < seen_count; k++) {
            free(seen[k]);
        }
    }
    free(seen);
    free(versions_dir);
    free(libraries_dir);
    free(client_id);
    free(client_path);
    free(client_jar);
    if (rc != 0) {
        sb_free(&cp);
    }
    return rc;
}

static int add_argument_value(ArgList *out, const JsonValue *value,
                              const Vars *vars)
{
    if (!value) {
        return 0;
    }
    if (value->type == JSON_STRING) {
        args_add_owned(out, substitute(value->u.string, vars));
        return 1;
    }
    if (value->type == JSON_ARRAY) {
        size_t i;
        int added = 0;
        for (i = 0; i < value->u.array.count; i++) {
            added += add_argument_value(out, value->u.array.items[i], vars);
        }
        return added;
    }
    return 0;
}

static int process_argument_array(const JsonValue *array, ArgList *out,
                                  const Vars *vars, const Features *features)
{
    size_t i;
    int added = 0;

    if (!array || array->type != JSON_ARRAY) {
        return 0;
    }
    for (i = 0; i < array->u.array.count; i++) {
        JsonValue *item = array->u.array.items[i];
        if (item->type == JSON_STRING) {
            args_add_owned(out, substitute(item->u.string, vars));
            added++;
        } else if (item->type == JSON_OBJECT) {
            JsonValue *rules = json_get(item, "rules");
            if (rules && !version_rules_allow(rules, features)) {
                continue;
            }
            if (add_argument_value(out, json_get(item, "value"), vars)) {
                added++;
            }
        }
    }
    return added;
}

static void add_legacy_arguments(const char *template, ArgList *out,
                                 const Vars *vars)
{
    size_t i = 0;
    size_t len;

    if (!template) {
        return;
    }
    len = strlen(template);
    while (i < len) {
        StrBuf token;
        int quote = 0;
        int had_placeholder = 0;

        while (i < len && isspace((unsigned char)template[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }
        sb_init(&token);
        while (i < len) {
            char c = template[i];
            if (quote) {
                if (c == (char)quote) {
                    quote = 0;
                    i++;
                } else if (c == '$' && i + 1 < len && template[i + 1] == '{') {
                    size_t start = i + 2;
                    size_t end = start;
                    while (end < len && template[end] != '}') end++;
                    if (end < len) {
                        sb_append(&token, vars_get(vars, template + start,
                                                   end - start));
                        i = end + 1;
                        had_placeholder = 1;
                    } else {
                        sb_append_char(&token, c);
                        i++;
                    }
                } else {
                    sb_append_char(&token, c);
                    i++;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
                i++;
            } else if (isspace((unsigned char)c)) {
                break;
            } else if (c == '$' && i + 1 < len && template[i + 1] == '{') {
                size_t start = i + 2;
                size_t end = start;
                while (end < len && template[end] != '}') end++;
                if (end < len) {
                    sb_append(&token, vars_get(vars, template + start,
                                               end - start));
                    i = end + 1;
                    had_placeholder = 1;
                } else {
                    sb_append_char(&token, c);
                    i++;
                }
            } else {
                sb_append_char(&token, c);
                i++;
            }
        }
        if (token.len || had_placeholder) {
            args_add_owned(out, sb_take(&token));
        } else {
            sb_free(&token);
        }
    }
}

static void add_fallback_game_arguments(ArgList *game,
                                        const LaunchOptions *options,
                                        const Version *version,
                                        const Vars *vars)
{
    args_add(game, "--username");
    args_add(game, vars_get(vars, "auth_player_name",
                            strlen("auth_player_name")));
    args_add(game, "--version");
    args_add(game, version->id);
    args_add(game, "--gameDir");
    args_add(game, vars_get(vars, "game_directory",
                            strlen("game_directory")));
    args_add(game, "--assetsDir");
    args_add(game, vars_get(vars, "assets_root", strlen("assets_root")));
    args_add(game, "--assetIndex");
    args_add(game, vars_get(vars, "assets_index_name",
                            strlen("assets_index_name")));
    args_add(game, "--uuid");
    args_add(game, vars_get(vars, "auth_uuid", strlen("auth_uuid")));
    args_add(game, "--accessToken");
    args_add(game, "0");
    args_add(game, "--clientId");
    args_add(game, "");
    args_add(game, "--xuid");
    args_add(game, "");
    args_add(game, "--userType");
    args_add(game, "legacy");
    args_add(game, "--versionType");
    args_add(game, version->version_type ? version->version_type : "release");
    args_add(game, "--userProperties");
    args_add(game, "{}");
    if (options->demo) {
        args_add(game, "--demo");
    }
    if (options->width > 0 && options->height > 0) {
        args_add(game, "--width");
        args_add(game, vars_get(vars, "resolution_width",
                                strlen("resolution_width")));
        args_add(game, "--height");
        args_add(game, vars_get(vars, "resolution_height",
                                strlen("resolution_height")));
    }
}

static int build_arguments(const LaunchOptions *options, Version *version,
                           ArgList *jvm, ArgList *game, char **error)
{
    wchar_t *game_dir = options->game_dir;
    wchar_t *versions_dir = path_join(game_dir, L"versions");
    wchar_t *version_dir = join_utf8_path(versions_dir, version->id);
    wchar_t *natives_dir = path_join(version_dir, L"natives");
    wchar_t *assets_dir = path_join(game_dir, L"assets");
    wchar_t *libraries_dir = path_join(game_dir, L"libraries");
    wchar_t *log_path = NULL;
    char *classpath = NULL;
    char *game_utf8 = NULL;
    char *assets_utf8 = NULL;
    char *natives_utf8 = NULL;
    char *libraries_utf8 = NULL;
    char *log_utf8 = NULL;
    char *game_assets_utf8 = NULL;
    char *uuid = NULL;
    char *auth_session = NULL;
    char width_buf[32];
    char height_buf[32];
    char memory_buf[64];
    char offline_uuid[37];
    Vars vars;
    Features features;
    int modern_game = 0;
    int i;

    memset(&features, 0, sizeof(features));
    features.demo = options->demo;
    features.custom_resolution = options->width > 0 && options->height > 0;

    if (build_classpath(version, game_dir, &classpath, error) != 0) {
        goto fail;
    }
    game_utf8 = path_to_utf8_alloc(game_dir);
    assets_utf8 = path_to_utf8_alloc(assets_dir);
    natives_utf8 = path_to_utf8_alloc(natives_dir);
    libraries_utf8 = path_to_utf8_alloc(libraries_dir);
    if (!game_utf8 || !assets_utf8 || !natives_utf8 || !libraries_utf8) {
        set_error(error, "路径编码转换失败");
        goto fail;
    }
    if (version->logging_id) {
        wchar_t *log_dir = path_join(assets_dir, L"log_configs");
        log_path = join_utf8_path(log_dir, version->logging_id);
        free(log_dir);
        log_utf8 = log_path ? path_to_utf8_alloc(log_path) : NULL;
    }
    {
        wchar_t *virtual_dir = path_join(assets_dir, L"virtual");
        wchar_t *index_dir = version->asset_index_id ?
            utf8_to_wide(version->asset_index_id) : NULL;
        wchar_t *full = index_dir ? path_join(virtual_dir, index_dir) : NULL;
        game_assets_utf8 = full ? path_to_utf8_alloc(full) : NULL;
        free(full);
        free(index_dir);
        free(virtual_dir);
    }
    if (options->uuid && *options->uuid) {
        uuid = xstrdup(options->uuid);
    } else {
        if (offline_uuid_for_name(options->username ? options->username :
                                  "Player", offline_uuid) != 0) {
            set_error(error, "无法生成离线 UUID");
            goto fail;
        }
        uuid = xstrdup(offline_uuid);
    }
    snprintf(width_buf, sizeof(width_buf), "%d",
             options->width > 0 ? options->width : 854);
    snprintf(height_buf, sizeof(height_buf), "%d",
             options->height > 0 ? options->height : 480);
    snprintf(memory_buf, sizeof(memory_buf), "-Xmx%dM",
             options->memory_mb > 0 ? options->memory_mb : 2048);

    vars_init(&vars);
    vars_add(&vars, "auth_player_name",
             options->username ? options->username : "Player");
    vars_add(&vars, "version_name", version->id);
    vars_add(&vars, "game_directory", game_utf8);
    vars_add(&vars, "assets_root", assets_utf8);
    vars_add(&vars, "assets_index_name",
             version->asset_index_id ? version->asset_index_id :
             version->assets);
    vars_add(&vars, "auth_uuid", uuid);
    vars_add(&vars, "auth_access_token", "0");
    auth_session = format_utf8("token:0:%s", uuid);
    vars_add(&vars, "auth_session", auth_session);
    vars_add(&vars, "clientid", "");
    vars_add(&vars, "auth_xuid", "");
    vars_add(&vars, "user_type", "legacy");
    vars_add(&vars, "version_type",
             version->version_type ? version->version_type : "release");
    vars_add(&vars, "natives_directory", natives_utf8);
    vars_add(&vars, "launcher_name", "MCLC");
    vars_add(&vars, "launcher_version", "1.0");
    vars_add(&vars, "classpath", classpath);
    vars_add(&vars, "classpath_separator", ";");
    vars_add(&vars, "library_directory", libraries_utf8);
    vars_add(&vars, "game_assets", game_assets_utf8 ? game_assets_utf8 :
             assets_utf8);
    vars_add(&vars, "profile_name",
             options->username ? options->username : "Player");
    vars_add(&vars, "user_properties", "{}");
    vars_add(&vars, "resolution_width", width_buf);
    vars_add(&vars, "resolution_height", height_buf);
    vars_add(&vars, "path", log_utf8 ? log_utf8 : "");

    args_add(jvm, memory_buf);
    for (i = 0; i < version->layer_count; i++) {
        JsonValue *arguments =
            json_get(version->layers[i].root, "arguments");
        if (arguments && arguments->type == JSON_OBJECT) {
            JsonValue *jvm_values = json_get(arguments, "jvm");
            JsonValue *game_values = json_get(arguments, "game");
            if (jvm_values && jvm_values->type == JSON_ARRAY) {
                process_argument_array(jvm_values, jvm, &vars, &features);
            }
            if (game_values && game_values->type == JSON_ARRAY) {
                modern_game = 1;
                process_argument_array(game_values, game, &vars, &features);
            }
        }
    }

    if (log_utf8 && version->logging_argument && *version->logging_argument) {
        char *arg = substitute(version->logging_argument, &vars);
        if (arg && *arg) {
            args_add_owned(jvm, arg);
        } else {
            free(arg);
        }
    }

    if (!modern_game && version->legacy_arguments) {
        add_legacy_arguments(version->legacy_arguments, game, &vars);
    }
    if (game->count == 0) {
        add_fallback_game_arguments(game, options, version, &vars);
    }

    if (options->extra_jvm) {
        size_t k;
        for (k = 0; k < options->extra_jvm_count; k++) {
            args_add(jvm, options->extra_jvm[k]);
        }
    }

    if (!args_has_exact(jvm, "-cp") &&
        !args_has_exact(jvm, "-classpath")) {
        args_add(jvm, "-cp");
        args_add(jvm, classpath);
    }
    if (!args_has_prefix(jvm, "-Djava.library.path=")) {
        char *libpath = format_utf8("-Djava.library.path=%s", natives_utf8);
        args_add_owned(jvm, libpath);
    }

    args_add(jvm, version->main_class);
    if (options->extra_game) {
        size_t k;
        for (k = 0; k < options->extra_game_count; k++) {
            args_add(game, options->extra_game[k]);
        }
    }
    for (i = 0; i < (int)game->count; i++) {
        args_add(jvm, game->items[i]);
    }

    free(versions_dir);
    free(version_dir);
    free(natives_dir);
    free(assets_dir);
    free(libraries_dir);
    free(log_path);
    free(classpath);
    free(game_utf8);
    free(assets_utf8);
    free(natives_utf8);
    free(libraries_utf8);
    free(log_utf8);
    free(game_assets_utf8);
    free(uuid);
    free(auth_session);
    return 0;

fail:
    free(versions_dir);
    free(version_dir);
    free(natives_dir);
    free(assets_dir);
    free(libraries_dir);
    free(log_path);
    free(classpath);
    free(game_utf8);
    free(assets_utf8);
    free(natives_utf8);
    free(libraries_utf8);
    free(log_utf8);
    free(game_assets_utf8);
    free(uuid);
    free(auth_session);
    return -1;
}

static wchar_t *try_java_file(const wchar_t *candidate)
{
    if (path_exists(candidate) && !path_is_dir(candidate)) {
        return xwcsdup(candidate);
    }
    return NULL;
}

static wchar_t *search_path_executable(const wchar_t *name)
{
    DWORD needed = SearchPathW(NULL, name, NULL, 0, NULL, NULL);
    wchar_t *buffer;
    if (needed == 0) {
        return NULL;
    }
    buffer = (wchar_t *)xmalloc((size_t)needed * sizeof(wchar_t) + 2);
    if (SearchPathW(NULL, name, NULL, needed + 1, buffer, NULL) == 0) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

static wchar_t *search_path_java(void)
{
    return search_path_executable(L"java.exe");
}

static wchar_t *java_from_home(const wchar_t *home)
{
    wchar_t *candidate;
    wchar_t *result;

    if (!home || !*home) {
        return NULL;
    }
    candidate = path_join(home, L"bin\\java.exe");
    result = try_java_file(candidate);
    free(candidate);
    if (!result) {
        candidate = path_join(home, L"java.exe");
        result = try_java_file(candidate);
        free(candidate);
    }
    return result;
}

static wchar_t *find_pattern_java(const wchar_t *parent,
                                  const wchar_t *pattern)
{
    wchar_t *search;
    WIN32_FIND_DATAW data;
    HANDLE handle;
    wchar_t *result = NULL;

    search = path_join(parent, pattern);
    handle = FindFirstFileW(search, &data);
    free(search);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t *dir;
            wchar_t *candidate;
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            dir = path_join(parent, data.cFileName);
            candidate = path_join(dir, L"bin\\java.exe");
            result = try_java_file(candidate);
            free(candidate);
            free(dir);
            if (result) {
                break;
            }
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return result;
}

static wchar_t *find_versioned_java(const wchar_t *parent, int major)
{
    wchar_t patterns[6][64];
    int count = 0;
    int i;

    if (major == 8) {
        wcscpy(patterns[count++], L"jdk1.8*");
        wcscpy(patterns[count++], L"jre1.8*");
        wcscpy(patterns[count++], L"jdk-8*");
        wcscpy(patterns[count++], L"jdk8*");
    } else {
        swprintf(patterns[count++], 64, L"jdk-%d*", major);
        swprintf(patterns[count++], 64, L"jdk%d*", major);
        swprintf(patterns[count++], 64, L"jre-%d*", major);
    }
    swprintf(patterns[count++], 64, L"zulu-%d*", major);
    swprintf(patterns[count++], 64, L"liberica-%d*", major);

    for (i = 0; i < count; i++) {
        wchar_t *result = find_pattern_java(parent, patterns[i]);
        if (result) {
            return result;
        }
    }
    return NULL;
}

static wchar_t *find_common_java(int major)
{
    static const wchar_t *vendors[] = {
        L"Java", L"Eclipse Adoptium", L"Microsoft", L"Zulu", L"BellSoft"
    };
    wchar_t *program_files = NULL;
    DWORD size;
    size_t v;

    size = GetEnvironmentVariableW(L"ProgramFiles", NULL, 0);
    if (size) {
        program_files = (wchar_t *)xmalloc((size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"ProgramFiles", program_files, size + 1);
    }
    if (!program_files) {
        return NULL;
    }
    if (major > 0) {
        for (v = 0; v < sizeof(vendors) / sizeof(vendors[0]); v++) {
            wchar_t *base = path_join(program_files, vendors[v]);
            wchar_t *r = find_versioned_java(base, major);
            free(base);
            if (r) {
                free(program_files);
                return r;
            }
        }
    } else {
        wchar_t *base = path_join(program_files, L"Java");
        wchar_t *r = find_pattern_java(base, L"jdk*");
        free(base);
        if (r) {
            free(program_files);
            return r;
        }
    }
    free(program_files);
    return NULL;
}

static wchar_t *find_official_runtime(int major)
{
    static const wchar_t *names[] = {
        L"java-runtime-delta", L"java-runtime-epsilon",
        L"java-runtime-gamma", L"java-runtime-alpha", L"jre-legacy"
    };
    static const wchar_t *arch = L"windows-x64";
    static const int order_legacy[] = { 4, 3, 2, 0, 1 };
    static const int order_16[] = { 3, 2, 0, 1, 4 };
    static const int order_17[] = { 2, 0, 1, 3, 4 };
    static const int order_21[] = { 0, 1, 2, 3, 4 };
    const int *order;
    wchar_t *bases[2] = { NULL, NULL };
    DWORD size;
    int b;
    int oi;

    if (major <= 8) {
        order = order_legacy;
    } else if (major <= 16) {
        order = order_16;
    } else if (major <= 17) {
        order = order_17;
    } else {
        order = order_21;
    }

    if (version_is_windows_arm64()) {
        arch = L"windows-arm64";
    }
    size = GetEnvironmentVariableW(L"ProgramFiles(x86)", NULL, 0);
    if (size) {
        bases[0] = (wchar_t *)xmalloc((size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"ProgramFiles(x86)", bases[0], size + 1);
    }
    size = GetEnvironmentVariableW(L"ProgramFiles", NULL, 0);
    if (size) {
        bases[1] = (wchar_t *)xmalloc((size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"ProgramFiles", bases[1], size + 1);
    }
    for (b = 0; b < 2; b++) {
        wchar_t *runtime;
        if (!bases[b]) continue;
        runtime = path_join(bases[b], L"Minecraft Launcher\\runtime");
        for (oi = 0; oi < 5; oi++) {
            int i = order[oi];
            wchar_t *candidate;
            wchar_t *name = path_join(runtime, names[i]);
            wchar_t *candidate2;
            wchar_t *result;

            if (major >= 21 && i != 0 && i != 1) {
                /* A Java 17 runtime cannot start a Java 21 game. */
                free(name);
                continue;
            }
            candidate = path_join3(name, arch, names[i]);
            candidate2 = path_join(candidate, L"bin\\java.exe");
            result = try_java_file(candidate2);
            free(candidate2);
            free(candidate);
            if (!result) {
                candidate2 = path_join(name, L"bin\\java.exe");
                result = try_java_file(candidate2);
                free(candidate2);
            }
            free(name);
            if (result) {
                free(runtime);
                free(bases[0]);
                free(bases[1]);
                return result;
            }
        }
        free(runtime);
    }
    free(bases[0]);
    free(bases[1]);
    return NULL;
}

wchar_t *launch_find_java(const wchar_t *preferred, int required_major,
                          char **error)
{
    wchar_t *result = NULL;
    wchar_t *env;
    DWORD size;

    if (preferred && *preferred) {
        if (path_is_dir(preferred)) {
            result = java_from_home(preferred);
        } else if (path_exists(preferred) && !path_is_dir(preferred)) {
            result = xwcsdup(preferred);
        } else if (!wcschr(preferred, L'\\') && !wcschr(preferred, L'/')) {
            result = search_path_executable(preferred);
        }
        if (!result) {
            set_error(error, "找不到指定的 Java: %ls", preferred);
            return NULL;
        }
        return result;
    }

    size = GetEnvironmentVariableW(L"MCLC_JAVA", NULL, 0);
    if (size) {
        env = (wchar_t *)xmalloc((size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"MCLC_JAVA", env, size + 1);
        if (path_is_dir(env)) {
            result = java_from_home(env);
        } else if (!wcschr(env, L'\\') && !wcschr(env, L'/')) {
            result = search_path_executable(env);
        } else {
            result = try_java_file(env);
        }
        free(env);
        if (result) return result;
    }

    size = GetEnvironmentVariableW(L"JAVA_HOME", NULL, 0);
    if (size) {
        env = (wchar_t *)xmalloc((size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"JAVA_HOME", env, size + 1);
        result = java_from_home(env);
        free(env);
        if (result) return result;
    }

    result = find_official_runtime(required_major);
    if (result) return result;

    result = find_common_java(required_major);
    if (result) return result;

    result = search_path_java();
    if (result) return result;

    printf("[!] 未在常见位置找到 java.exe，将尝试 PATH。\n");
    return xwcsdup(L"java.exe");
}

static void append_quoted_argument(StrBuf *sb, const char *arg)
{
    int need_quote = !arg || !*arg || strpbrk(arg, " \t\n\v\"") != NULL;
    size_t i;
    size_t backslashes = 0;

    if (!need_quote) {
        sb_append(sb, arg);
        return;
    }
    sb_append_char(sb, '"');
    for (i = 0; arg && arg[i]; i++) {
        char c = arg[i];
        if (c == '\\') {
            backslashes++;
        } else if (c == '"') {
            size_t k;
            for (k = 0; k < backslashes * 2 + 1; k++) {
                sb_append_char(sb, '\\');
            }
            sb_append_char(sb, '"');
            backslashes = 0;
        } else {
            size_t k;
            for (k = 0; k < backslashes; k++) {
                sb_append_char(sb, '\\');
            }
            backslashes = 0;
            sb_append_char(sb, c);
        }
    }
    {
        size_t k;
        for (k = 0; k < backslashes * 2; k++) {
            sb_append_char(sb, '\\');
        }
    }
    sb_append_char(sb, '"');
}

static void build_command_line(const wchar_t *java, const ArgList *args,
                               StrBuf *out)
{
    char *java_utf8 = wide_to_utf8(java);
    size_t i;

    append_quoted_argument(out, java_utf8 ? java_utf8 : "java.exe");
    for (i = 0; i < args->count; i++) {
        sb_append_char(out, ' ');
        append_quoted_argument(out, args->items[i]);
    }
    free(java_utf8);
}

static char *win32_error_text(DWORD code)
{
    wchar_t *buffer = NULL;
    DWORD n;
    char *text;

    n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPWSTR)&buffer, 0, NULL);
    if (!n || !buffer) {
        return format_utf8("Windows 错误 %lu", (unsigned long)code);
    }
    while (n > 0 && (buffer[n - 1] == L'\r' || buffer[n - 1] == L'\n')) {
        buffer[--n] = L'\0';
    }
    text = wide_to_utf8(buffer);
    LocalFree(buffer);
    return text ? text : format_utf8("Windows 错误 %lu",
                                     (unsigned long)code);
}

int launch_version(const LaunchOptions *options, Version *version,
                   char **error)
{
    ArgList jvm;
    ArgList game;
    StrBuf command;
    wchar_t *java;
    wchar_t *command_wide = NULL;
    wchar_t *working_dir;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    BOOL created;
    DWORD exit_code = 0;
    int rc = 0;
    int use_app_name;
    char *preview = NULL;

    if (!options || !options->game_dir || !version) {
        set_error(error, "启动参数不完整");
        return -1;
    }
    if (mkdir_p(options->game_dir) != 0) {
        set_error(error, "无法创建游戏目录: %ls", options->game_dir);
        return -1;
    }
    working_dir = xwcsdup(options->game_dir);
    args_init(&jvm);
    args_init(&game);
    sb_init(&command);

    if (build_arguments(options, version, &jvm, &game, error) != 0) {
        rc = -1;
        goto cleanup;
    }

    java = launch_find_java(options->java, version->java_major, error);
    if (!java) {
        rc = -1;
        goto cleanup;
    }
    printf("[*] Java: %ls（版本要求 Java %d+）\n", java,
           version->java_major > 0 ? version->java_major : 8);
    build_command_line(java, &jvm, &command);
    if (command.len == 0) {
        set_error(error, "无法构造启动命令");
        free(java);
        rc = -1;
        goto cleanup;
    }
    preview = sb_take(&command);
    sb_init(&command);

    command_wide = utf8_to_wide(preview);
    if (!command_wide) {
        set_error(error, "启动命令包含无效字符");
        free(java);
        rc = -1;
        goto cleanup;
    }
    if (wcslen(command_wide) > 32000) {
        set_error(error, "启动命令过长（%zu 个字符）",
                  wcslen(command_wide));
        free(java);
        rc = -1;
        goto cleanup;
    }

    if (options->dry_run) {
        printf("[dry-run] %s\n", preview);
        printf("[dry-run] 工作目录: %ls\n", working_dir);
        free(java);
        rc = 0;
        goto cleanup;
    }

    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    use_app_name = (wcschr(java, L'\\') || wcschr(java, L'/')) &&
                   path_exists(java);
    created = CreateProcessW(use_app_name ? java : NULL, command_wide,
                             NULL, NULL, TRUE,
                             options->gui_mode ? CREATE_NO_WINDOW : 0,
                             NULL, working_dir, &startup, &process);
    if (!created) {
        char *msg = win32_error_text(GetLastError());
        set_error(error, "无法启动 Java: %s\n命令: %s", msg, preview);
        free(msg);
        free(java);
        rc = -1;
        goto cleanup;
    }
    CloseHandle(process.hThread);
    printf("[*] 已启动 Minecraft，PID %lu\n",
           (unsigned long)process.dwProcessId);
    if (options->detach) {
        CloseHandle(process.hProcess);
        free(java);
        rc = 0;
        goto cleanup;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    if (GetExitCodeProcess(process.hProcess, &exit_code)) {
        printf("[*] Minecraft 已退出，代码 %lu\n", (unsigned long)exit_code);
        rc = (int)exit_code;
    } else {
        set_error(error, "无法读取 Minecraft 退出代码");
        rc = -1;
    }
    CloseHandle(process.hProcess);
    free(java);

cleanup:
    free(working_dir);
    args_free(&jvm);
    args_free(&game);
    sb_free(&command);
    free(command_wide);
    free(preview);
    return rc;
}
