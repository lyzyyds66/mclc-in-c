#include "version.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void replace_string(char **target, const char *value)
{
    if (!value) {
        return;
    }
    free(*target);
    *target = xstrdup(value);
}

const char *version_os_name(void)
{
    return "windows";
}

int version_is_windows_arm64(void)
{
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
#ifdef PROCESSOR_ARCHITECTURE_ARM64
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;
#else
    return 0;
#endif
}

const char *version_os_arch(void)
{
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x86_64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
#ifdef PROCESSOR_ARCHITECTURE_ARM64
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";
#endif
#ifdef PROCESSOR_ARCHITECTURE_ARM
    case PROCESSOR_ARCHITECTURE_ARM:
        return "arm";
#endif
    default:
        return "x86_64";
    }
}

static int simple_os_version_match(const char *pattern, const char *version)
{
    char plain[128];
    size_t j = 0;
    size_t i;
    int anchored_start = 0;
    int anchored_end = 0;

    if (!pattern || !*pattern) {
        return 1;
    }
    if (pattern[0] == '^') {
        anchored_start = 1;
        pattern++;
    }
    if (strchr(pattern, '[') || strchr(pattern, '(') ||
        strchr(pattern, '|') || strstr(pattern, "\\d")) {
        /* Complex regular expression: do not exclude the platform. */
        return 1;
    }
    for (i = 0; pattern[i] && j + 1 < sizeof(plain); i++) {
        if (pattern[i] == '\\' && pattern[i + 1]) {
            plain[j++] = pattern[++i];
        } else {
            plain[j++] = pattern[i];
        }
    }
    plain[j] = '\0';
    if (j && plain[j - 1] == '$') {
        plain[j - 1] = '\0';
        anchored_end = 1;
    }
    if (anchored_end) {
        size_t lp = strlen(plain);
        size_t lv = strlen(version);
        return lp <= lv && strcmp(version + lv - lp, plain) == 0;
    }
    if (anchored_start) {
        return strncmp(version, plain, strlen(plain)) == 0;
    }
    return strstr(version, plain) != NULL;
}

static int rule_os_matches(const JsonValue *os)
{
    const char *name;
    const char *arch;
    const char *os_version;
    static const char *windows_version = "10.0";

    if (!os || os->type != JSON_OBJECT) {
        return 1;
    }
    name = json_get_string(os, "name");
    if (name && !str_ieq(name, version_os_name())) {
        return 0;
    }
    arch = json_get_string(os, "arch");
    if (arch && !str_ieq(arch, version_os_arch())) {
        /* x86_64 is sometimes spelled x86-64 or amd64 in metadata. */
        if (!(str_ieq(version_os_arch(), "x86_64") &&
              (str_ieq(arch, "x86-64") || str_ieq(arch, "amd64")))) {
            return 0;
        }
    }
    os_version = json_get_string(os, "version");
    if (os_version && !simple_os_version_match(os_version, windows_version)) {
        return 0;
    }
    return 1;
}

static int feature_value(const Features *features, const char *key)
{
    if (!features) {
        return 0;
    }
    if (strcmp(key, "is_demo_user") == 0) return features->demo;
    if (strcmp(key, "has_custom_resolution") == 0)
        return features->custom_resolution;
    if (strcmp(key, "has_quick_plays_support") == 0)
        return features->quick_plays_support;
    if (strcmp(key, "is_quick_play_singleplayer") == 0)
        return features->quick_play_singleplayer;
    if (strcmp(key, "is_quick_play_multiplayer") == 0)
        return features->quick_play_multiplayer;
    return 0;
}

static int rule_features_match(const JsonValue *conditions,
                               const Features *features)
{
    JsonMember *m;

    if (!conditions || conditions->type != JSON_OBJECT) {
        return 1;
    }
    for (m = conditions->u.object.head; m; m = m->next) {
        if (m->value->type != JSON_BOOL) {
            return 0;
        }
        if (feature_value(features, m->key) != m->value->u.boolean) {
            return 0;
        }
    }
    return 1;
}

int version_rules_allow(const JsonValue *rules, const Features *features)
{
    int allowed;
    size_t i;

    if (!rules || rules->type != JSON_ARRAY || rules->u.array.count == 0) {
        return 1;
    }
    allowed = 0;
    for (i = 0; i < rules->u.array.count; i++) {
        JsonValue *rule = rules->u.array.items[i];
        JsonValue *os;
        JsonValue *cond;
        const char *action;
        int matches = 1;

        if (rule->type != JSON_OBJECT) {
            continue;
        }
        action = json_get_string(rule, "action");
        if (!action) {
            continue;
        }
        os = json_get(rule, "os");
        if (os && !rule_os_matches(os)) {
            matches = 0;
        }
        cond = json_get(rule, "features");
        if (cond && !rule_features_match(cond, features)) {
            matches = 0;
        }
        if (matches) {
            allowed = str_ieq(action, "allow");
        }
    }
    return allowed;
}

static void free_download(DownloadInfo *d)
{
    free(d->path);
    free(d->url);
    free(d->sha1);
    memset(d, 0, sizeof(*d));
}

static void parse_download(const JsonValue *object, DownloadInfo *out,
                           const char *fallback_path)
{
    const char *value;

    if (!object || object->type != JSON_OBJECT) {
        return;
    }
    if (fallback_path) {
        replace_string(&out->path, fallback_path);
    }
    value = json_get_string(object, "path");
    if (value) {
        replace_string(&out->path, value);
    }
    value = json_get_string(object, "url");
    if (value && *value) {
        replace_string(&out->url, value);
    }
    value = json_get_string(object, "sha1");
    if (value) {
        replace_string(&out->sha1, value);
    }
    out->size = json_get_i64(object, "size", out->size);
    out->present = 1;
}

char *version_maven_path(const char *name, const char *classifier)
{
    char *copy;
    char *parts[4];
    int count = 0;
    char *p;
    char *ext;
    char *last_at;
    StrBuf sb;
    size_t i;
    char *result;

    if (!name || !*name) {
        return NULL;
    }
    copy = xstrdup(name);
    ext = NULL;
    last_at = strrchr(copy, '@');
    if (last_at) {
        *last_at = '\0';
        ext = last_at + 1;
    }
    if (!ext || !*ext) {
        ext = "jar";
    }
    parts[count++] = copy;
    p = copy;
    while (count < 4) {
        p = strchr(p, ':');
        if (!p) {
            break;
        }
        *p++ = '\0';
        parts[count++] = p;
    }
    if (count < 3) {
        free(copy);
        return NULL;
    }
    if (classifier == NULL && count >= 4) {
        classifier = parts[3];
    }

    sb_init(&sb);
    for (i = 0; parts[0][i]; i++) {
        sb_append_char(&sb, parts[0][i] == '.' ? '/' : parts[0][i]);
    }
    sb_append_char(&sb, '/');
    sb_append(&sb, parts[1]);
    sb_append_char(&sb, '/');
    sb_append(&sb, parts[2]);
    sb_append_char(&sb, '/');
    sb_append(&sb, parts[1]);
    sb_append_char(&sb, '-');
    sb_append(&sb, parts[2]);
    if (classifier && *classifier) {
        sb_append_char(&sb, '-');
        sb_append(&sb, classifier);
    }
    sb_append_char(&sb, '.');
    sb_append(&sb, ext);
    result = sb_take(&sb);
    free(copy);
    return result;
}

static const char *pick_native_classifier(const JsonValue *library)
{
    JsonValue *natives = json_get(library, "natives");
    static const char *arm64_keys[] = {
        "windows-arm64", "windows-aarch64", "windows-64", "windows", NULL
    };
    static const char *x64_keys[] = {
        "windows-64", "windows-x86_64", "windows", NULL
    };
    static const char *x86_keys[] = {
        "windows-32", "windows-x86", "windows", NULL
    };
    const char **keys;
    int i;

    if (!natives || natives->type != JSON_OBJECT) {
        return NULL;
    }
    if (version_is_windows_arm64()) {
        keys = arm64_keys;
    } else if (strcmp(version_os_arch(), "x86") == 0) {
        keys = x86_keys;
    } else {
        keys = x64_keys;
    }
    for (i = 0; keys[i]; i++) {
        const char *value = json_get_string(natives, keys[i]);
        if (value && *value) {
            return value;
        }
    }
    return NULL;
}

static char *expand_native_classifier(const char *value)
{
    const char *arch;
    StrBuf sb;

    if (!value) {
        return NULL;
    }
    arch = strcmp(version_os_arch(), "x86") == 0 ? "32" : "64";
    sb_init(&sb);
    while (*value) {
        if (strncmp(value, "${arch}", 7) == 0) {
            sb_append(&sb, arch);
            value += 7;
        } else {
            sb_append_char(&sb, *value++);
        }
    }
    return sb_take(&sb);
}

static char *default_library_url(const char *path)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "https://libraries.minecraft.net/");
    sb_append(&sb, path);
    return sb_take(&sb);
}

static void parse_library(const JsonValue *item, Library *lib)
{
    const char *name = json_get_string(item, "name");
    JsonValue *downloads;
    JsonValue *artifact;
    char *classifier;
    int has_artifact = 1;

    memset(lib, 0, sizeof(*lib));
    if (!name) {
        return;
    }
    lib->name = xstrdup(name);
    lib->path = version_maven_path(name, NULL);

    downloads = json_get(item, "downloads");
    if (downloads && downloads->type == JSON_OBJECT) {
        artifact = json_get(downloads, "artifact");
        if (artifact && artifact->type == JSON_OBJECT) {
            DownloadInfo d;
            char *fallback = lib->path;
            memset(&d, 0, sizeof(d));
            lib->path = NULL;
            parse_download(artifact, &d, fallback);
            free(fallback);
            lib->path = d.path;
            lib->url = d.url;
            lib->sha1 = d.sha1;
            lib->size = d.size;
        } else {
            /*
             * Some libraries only publish a natives classifier. Do not invent
             * a base artifact for them: it would 404 and poison classpath.
             * An otherwise empty downloads object still uses the Maven path.
             */
            JsonValue *classifiers = json_get(downloads, "classifiers");
            if (classifiers && classifiers->type == JSON_OBJECT) {
                has_artifact = 0;
                free(lib->path);
                lib->path = NULL;
            }
        }
    } else if (json_get(item, "natives") &&
               (strstr(name, "lwjgl-platform") ||
                strstr(name, "jinput-platform"))) {
        /* Legacy metadata for these two artifacts is native-only. */
        has_artifact = 0;
        free(lib->path);
        lib->path = NULL;
    }
    if (has_artifact) {
        if (!lib->path) {
            lib->path = version_maven_path(name, NULL);
        }
        if (!lib->url || !*lib->url) {
            free(lib->url);
            lib->url = lib->path ? default_library_url(lib->path) : NULL;
        }
    } else {
        free(lib->url);
        free(lib->sha1);
        lib->url = NULL;
        lib->sha1 = NULL;
        lib->size = 0;
    }

    classifier = expand_native_classifier(pick_native_classifier(item));
    if (classifier && *classifier) {
        JsonValue *classifiers = downloads ?
            json_get(downloads, "classifiers") : NULL;
        JsonValue *native = classifiers ?
            json_get(classifiers, classifier) : NULL;
        char *fallback = version_maven_path(name, classifier);
        DownloadInfo d;
        memset(&d, 0, sizeof(d));
        if (native && native->type == JSON_OBJECT) {
            parse_download(native, &d, fallback);
            lib->native_classifier = xstrdup(classifier);
            lib->native_path = d.path;
            lib->native_url = d.url;
            lib->native_sha1 = d.sha1;
            lib->native_size = d.size;
            if ((!lib->native_url || !*lib->native_url) && lib->native_path) {
                lib->native_url = default_library_url(lib->native_path);
            }
        } else if (fallback) {
            lib->native_classifier = xstrdup(classifier);
            lib->native_path = xstrdup(fallback);
            lib->native_url = default_library_url(fallback);
        }
        free(fallback);
    }
    free(classifier);

    if (lib->path && !path_relative_is_safe(lib->path)) {
        free(lib->path);
        lib->path = NULL;
    }
    if (lib->native_path && !path_relative_is_safe(lib->native_path)) {
        free(lib->native_path);
        lib->native_path = NULL;
        free(lib->native_url);
        lib->native_url = NULL;
        free(lib->native_sha1);
        lib->native_sha1 = NULL;
        lib->native_size = 0;
    }
}

static int parse_layer_libraries(VersionLayer *layer, char **error)
{
    JsonValue *libraries = json_get(layer->root, "libraries");
    Features none;
    size_t i;

    memset(&none, 0, sizeof(none));
    if (!libraries || libraries->type != JSON_ARRAY) {
        return 0;
    }
    layer->libraries = (Library *)xcalloc(libraries->u.array.count,
                                           sizeof(Library));
    for (i = 0; i < libraries->u.array.count; i++) {
        JsonValue *item = libraries->u.array.items[i];
        JsonValue *rules;
        Library lib;

        if (item->type != JSON_OBJECT) {
            continue;
        }
        rules = json_get(item, "rules");
        if (rules && !version_rules_allow(rules, &none)) {
            continue;
        }
        parse_library(item, &lib);
        if (!lib.name || (!lib.path && !lib.native_path)) {
            free(lib.name);
            free(lib.path);
            free(lib.url);
            free(lib.sha1);
            free(lib.native_classifier);
            free(lib.native_path);
            free(lib.native_url);
            free(lib.native_sha1);
            continue;
        }
        layer->libraries[layer->library_count++] = lib;
    }
    if (layer->library_count == 0) {
        free(layer->libraries);
        layer->libraries = NULL;
    }
    (void)error;
    return 0;
}

static int load_recursive(const wchar_t *game_dir, const char *version_id,
                          int depth, Version *out, char **error)
{
    wchar_t *id_wide;
    wchar_t *version_dir;
    wchar_t *json_path;
    JsonValue *root;
    const char *parent_id;

    if (depth >= MCLC_MAX_VERSION_LAYERS) {
        set_error(error, "版本继承链过深（超过 %d 层）",
                  MCLC_MAX_VERSION_LAYERS);
        return -1;
    }
    if (!path_component_is_safe(version_id)) {
        set_error(error, "版本 ID 不安全: %s",
                  version_id ? version_id : "(null)");
        return -1;
    }
    id_wide = utf8_to_wide(version_id);
    if (!id_wide) {
        set_error(error, "版本 ID 编码无效");
        return -1;
    }
    version_dir = path_join3(game_dir, L"versions", id_wide);
    json_path = path_join(version_dir, id_wide);
    {
        size_t len = wcslen(json_path);
        wchar_t *with_ext = (wchar_t *)xmalloc((len + 6) * sizeof(wchar_t));
        wcscpy(with_ext, json_path);
        wcscat(with_ext, L".json");
        free(json_path);
        json_path = with_ext;
    }
    root = json_parse_file(json_path, error);
    if (!root) {
        char *detail = (error && *error) ? xstrdup(*error) : NULL;
        set_error(error, "无法加载版本 %s 的 JSON: %ls%s%s", version_id,
                  json_path, detail ? " - " : "", detail ? detail : "");
        free(detail);
        free(id_wide);
        free(version_dir);
        free(json_path);
        return -1;
    }
    free(id_wide);
    free(version_dir);
    free(json_path);

    parent_id = json_get_string(root, "inheritsFrom");
    if (parent_id && *parent_id) {
        Version parent;
        int i;
        memset(&parent, 0, sizeof(parent));
        if (load_recursive(game_dir, parent_id, depth + 1, &parent, error) != 0) {
            version_free(&parent);
            json_free(root);
            return -1;
        }
        if (out->layer_count + parent.layer_count > MCLC_MAX_VERSION_LAYERS) {
            version_free(&parent);
            json_free(root);
            set_error(error, "版本继承链过深");
            return -1;
        }
        for (i = 0; i < parent.layer_count; i++) {
            out->layers[out->layer_count++] = parent.layers[i];
            memset(&parent.layers[i], 0, sizeof(VersionLayer));
        }
        parent.layer_count = 0;
        version_free(&parent);
    }

    if (out->layer_count >= MCLC_MAX_VERSION_LAYERS) {
        json_free(root);
        set_error(error, "版本继承链过深");
        return -1;
    }
    {
        VersionLayer *layer = &out->layers[out->layer_count];
        memset(layer, 0, sizeof(*layer));
        layer->root = root;
        if (parse_layer_libraries(layer, error) != 0) {
            json_free(root);
            layer->root = NULL;
            return -1;
        }
        out->layer_count++;
    }
    return 0;
}

static void resolve_version(Version *version)
{
    int i;

    for (i = 0; i < version->layer_count; i++) {
        JsonValue *root = version->layers[i].root;
        const char *value;
        JsonValue *downloads;
        JsonValue *asset_index;
        JsonValue *logging;
        JsonValue *java_version;

        value = json_get_string(root, "mainClass");
        if (value) replace_string(&version->main_class, value);
        value = json_get_string(root, "assets");
        if (value) replace_string(&version->assets, value);
        value = json_get_string(root, "jar");
        if (value) replace_string(&version->jar_id, value);
        value = json_get_string(root, "type");
        if (value) replace_string(&version->version_type, value);
        value = json_get_string(root, "minecraftArguments");
        if (value) replace_string(&version->legacy_arguments, value);

        downloads = json_get(root, "downloads");
        if (downloads && downloads->type == JSON_OBJECT) {
            JsonValue *client = json_get(downloads, "client");
            if (client && client->type == JSON_OBJECT) {
                free_download(&version->client);
                parse_download(client, &version->client, NULL);
            }
        }
        asset_index = json_get(root, "assetIndex");
        if (asset_index && asset_index->type == JSON_OBJECT) {
            value = json_get_string(asset_index, "id");
            if (value) replace_string(&version->asset_index_id, value);
            free_download(&version->asset_index);
            parse_download(asset_index, &version->asset_index, NULL);
        }
        logging = json_get(root, "logging");
        if (logging && logging->type == JSON_OBJECT) {
            JsonValue *client = json_get(logging, "client");
            if (client && client->type == JSON_OBJECT) {
                JsonValue *file = json_get(client, "file");
                value = json_get_string(client, "argument");
                if (value) replace_string(&version->logging_argument, value);
                if (file && file->type == JSON_OBJECT) {
                    value = json_get_string(file, "id");
                    if (value) replace_string(&version->logging_id, value);
                    free_download(&version->logging);
                    parse_download(file, &version->logging, NULL);
                }
            }
        }
        java_version = json_get(root, "javaVersion");
        if (java_version && java_version->type == JSON_OBJECT) {
            long long major = json_get_i64(java_version, "majorVersion", 0);
            if (major > 0) {
                version->java_major = (int)major;
            }
        }
    }

    if (!version->assets) {
        version->assets = xstrdup("legacy");
    }
    if (!version->asset_index_id && version->assets) {
        version->asset_index_id = xstrdup(version->assets);
    }
    if (!version->jar_id) {
        version->jar_id = xstrdup(version->id ? version->id : "");
    }
    if (!version->version_type) {
        version->version_type = xstrdup("release");
    }
    if (!version->java_major) {
        version->java_major = 8;
    }
}

int version_load(const wchar_t *game_dir, const char *version_id,
                 Version *out, char **error)
{
    char *id_copy;

    memset(out, 0, sizeof(*out));
    if (!game_dir || !version_id || !*version_id) {
        set_error(error, "版本 ID 为空");
        return -1;
    }
    if (load_recursive(game_dir, version_id, 0, out, error) != 0) {
        version_free(out);
        return -1;
    }
    id_copy = xstrdup(version_id);
    out->id = id_copy;
    resolve_version(out);
    if (!out->main_class) {
        version_free(out);
        set_error(error, "版本 %s 缺少 mainClass", version_id);
        return -1;
    }
    if (!path_component_is_safe(out->jar_id)) {
        version_free(out);
        set_error(error, "版本 %s 的 jar 名称不安全", version_id);
        return -1;
    }
    if (out->asset_index_id &&
        !path_component_is_safe(out->asset_index_id)) {
        version_free(out);
        set_error(error, "版本 %s 的 assetIndex.id 不安全", version_id);
        return -1;
    }
    if (out->logging_id && !path_component_is_safe(out->logging_id)) {
        version_free(out);
        set_error(error, "版本 %s 的 logging.file.id 不安全", version_id);
        return -1;
    }
    return 0;
}

void version_free(Version *version)
{
    int i;

    if (!version) {
        return;
    }
    for (i = 0; i < version->layer_count; i++) {
        VersionLayer *layer = &version->layers[i];
        size_t j;
        json_free(layer->root);
        for (j = 0; j < layer->library_count; j++) {
            Library *lib = &layer->libraries[j];
            free(lib->name);
            free(lib->path);
            free(lib->url);
            free(lib->sha1);
            free(lib->native_classifier);
            free(lib->native_path);
            free(lib->native_url);
            free(lib->native_sha1);
        }
        free(layer->libraries);
    }
    free(version->id);
    free(version->main_class);
    free(version->assets);
    free(version->jar_id);
    free(version->version_type);
    free(version->asset_index_id);
    free(version->logging_id);
    free(version->logging_argument);
    free(version->legacy_arguments);
    free_download(&version->client);
    free_download(&version->asset_index);
    free_download(&version->logging);
    memset(version, 0, sizeof(*version));
}

char *version_substitute(const char *input, const char *const *keys,
                         const char *const *values, size_t count)
{
    StrBuf sb;
    size_t i = 0;
    size_t len;

    if (!input) {
        return NULL;
    }
    len = strlen(input);
    sb_init(&sb);
    while (i < len) {
        if (input[i] == '$' && i + 1 < len && input[i + 1] == '{') {
            size_t start = i + 2;
            size_t end = start;
            size_t k;
            int found = 0;
            while (end < len && input[end] != '}') {
                end++;
            }
            if (end >= len) {
                sb_append(&sb, input + i);
                break;
            }
            for (k = 0; k < count; k++) {
                size_t key_len = strlen(keys[k]);
                if (key_len == end - start &&
                    memcmp(keys[k], input + start, key_len) == 0) {
                    if (values[k]) {
                        sb_append(&sb, values[k]);
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* Unknown variables expand to an empty string. */
            }
            i = end + 1;
        } else {
            sb_append_char(&sb, input[i++]);
        }
    }
    return sb_take(&sb);
}
