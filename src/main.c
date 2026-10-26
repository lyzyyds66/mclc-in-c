#include "install.h"
#include "launch.h"
#include "remote.h"
#include "util.h"
#include "version.h"

#include <io.h>
#include <locale.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MCLC_VERSION "1.0.0"

typedef struct {
    wchar_t *game_dir;
    char *username;
    char *uuid;
    wchar_t *java;
    int memory_mb;
    int width;
    int height;
    int demo;
    int verify;
    int detach;
    int dry_run;
    int refresh;
    int snapshots;
    char *command;
    char *version;
    char **extra_jvm;
    size_t extra_jvm_count;
    char **extra_game;
    size_t extra_game_count;
} CliOptions;

static void print_help(void)
{
    printf(
        "MCLC %s - 用 C 语言编写的 Minecraft Java 版启动器\n"
        "\n"
        "用法:\n"
        "  mclc                          交互式启动\n"
        "  mclc launch <版本>            启动（自动下载缺失文件）\n"
        "  mclc install <版本>           只安装/校验，不启动\n"
        "  mclc list                     列出本地已安装版本\n"
        "  mclc remote [--snapshots]     列出 Mojang 官方版本\n"
        "\n"
        "版本可以是 1.21.4、latest（最新正式版）或 latest-snapshot。\n"
        "\n"
        "选项:\n"
        "  --dir <路径>        .minecraft 目录（默认 %%APPDATA%%\\.minecraft）\n"
        "  --user <名字>       离线用户名（默认 Steve）\n"
        "  --uuid <UUID>       覆盖离线 UUID\n"
        "  --java <路径>       java.exe 或 JAVA_HOME 目录\n"
        "  --memory <MB>       最大堆内存（默认 2048）\n"
        "  --width <像素>      窗口宽度\n"
        "  --height <像素>     窗口高度\n"
        "  --demo              启动演示模式\n"
        "  --verify            启动前校验已有文件的 SHA-1（较慢）\n"
        "  --refresh           强制重新下载版本 JSON\n"
        "  --snapshots         remote 命令同时列出快照\n"
        "  --dry-run           补齐文件后只打印 Java 命令行，不启动\n"
        "  --detach            启动后不等待游戏退出\n"
        "  --jvm <参数>        追加 JVM 参数（可重复）\n"
        "  --game-arg <参数>   追加游戏参数（可重复）\n"
        "  --                  其后的参数全部传给游戏\n"
        "  -h, --help          显示帮助\n"
        "  -V                  显示启动器版本\n"
        "\n"
        "示例:\n"
        "  mclc install latest\n"
        "  mclc launch 1.21.4 --memory 4096 --java \"C:\\Java\\jdk-21\\bin\\java.exe\"\n"
        "  mclc launch 1.20.1 --jvm -Dorg.lwjgl.util.Debug=true\n"
        "\n"
        "说明: 本启动器只支持离线登录。首次安装需要联网下载客户端、游戏库和资源。\n",
        MCLC_VERSION);
}

static wchar_t *get_default_game_dir(void)
{
    DWORD size = GetEnvironmentVariableW(L"APPDATA", NULL, 0);
    wchar_t *appdata;
    wchar_t *path;

    if (size == 0) {
        return xwcsdup(L".minecraft");
    }
    appdata = (wchar_t *)xmalloc((size + 1) * sizeof(wchar_t));
    if (GetEnvironmentVariableW(L"APPDATA", appdata, size + 1) == 0) {
        free(appdata);
        return xwcsdup(L".minecraft");
    }
    path = path_join(appdata, L".minecraft");
    free(appdata);
    return path;
}

static wchar_t *absolute_path(const wchar_t *path)
{
    DWORD needed;
    wchar_t *buffer;

    if (!path || !*path) {
        return NULL;
    }
    needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0) {
        return xwcsdup(path);
    }
    buffer = (wchar_t *)xmalloc(((size_t)needed + 1) * sizeof(wchar_t));
    if (GetFullPathNameW(path, needed + 1, buffer, NULL) == 0) {
        free(buffer);
        return xwcsdup(path);
    }
    return buffer;
}

static void set_wide_option(wchar_t **target, const wchar_t *value)
{
    free(*target);
    *target = xwcsdup(value);
}

static void set_string_option(char **target, const wchar_t *value)
{
    char *utf8 = wide_to_utf8(value);
    if (!utf8) {
        return;
    }
    free(*target);
    *target = utf8;
}

static void add_string(char ***array, size_t *count, const char *value)
{
    char **items = (char **)xrealloc(*array, (*count + 1) * sizeof(char *));
    *array = items;
    items[*count] = xstrdup(value);
    (*count)++;
}

static int parse_int_option(const wchar_t *value, int min_value, int max_value,
                            int *out)
{
    wchar_t *end = NULL;
    long number;

    if (!value || !*value) {
        return -1;
    }
    number = wcstol(value, &end, 10);
    if (!end || *end != L'\0' || number < min_value || number > max_value) {
        return -1;
    }
    *out = (int)number;
    return 0;
}

static int parse_cli(int argc, wchar_t **argv, CliOptions *options)
{
    int i;
    int after_double_dash = 0;

    memset(options, 0, sizeof(*options));
    options->memory_mb = 2048;
    options->username = xstrdup("Steve");

    for (i = 1; i < argc; i++) {
        wchar_t *arg = argv[i];
        if (after_double_dash) {
            char *utf8 = wide_to_utf8(arg);
            if (utf8) {
                add_string(&options->extra_game, &options->extra_game_count,
                           utf8);
                free(utf8);
            }
            continue;
        }
        if (wcscmp(arg, L"--") == 0) {
            after_double_dash = 1;
        } else if (wcscmp(arg, L"--dir") == 0) {
            if (++i >= argc) return -1;
            set_wide_option(&options->game_dir, argv[i]);
        } else if (wcscmp(arg, L"--user") == 0 ||
                   wcscmp(arg, L"--username") == 0) {
            if (++i >= argc) return -1;
            set_string_option(&options->username, argv[i]);
        } else if (wcscmp(arg, L"--uuid") == 0) {
            if (++i >= argc) return -1;
            set_string_option(&options->uuid, argv[i]);
        } else if (wcscmp(arg, L"--java") == 0) {
            if (++i >= argc) return -1;
            set_wide_option(&options->java, argv[i]);
        } else if (wcscmp(arg, L"--memory") == 0) {
            if (++i >= argc ||
                parse_int_option(argv[i], 256, 65536,
                                 &options->memory_mb) != 0) {
                return -1;
            }
        } else if (wcscmp(arg, L"--width") == 0) {
            if (++i >= argc ||
                parse_int_option(argv[i], 320, 7680,
                                 &options->width) != 0) {
                return -1;
            }
        } else if (wcscmp(arg, L"--height") == 0) {
            if (++i >= argc ||
                parse_int_option(argv[i], 240, 4320,
                                 &options->height) != 0) {
                return -1;
            }
        } else if (wcscmp(arg, L"--demo") == 0) {
            options->demo = 1;
        } else if (wcscmp(arg, L"--verify") == 0) {
            options->verify = 1;
        } else if (wcscmp(arg, L"--detach") == 0) {
            options->detach = 1;
        } else if (wcscmp(arg, L"--dry-run") == 0) {
            options->dry_run = 1;
        } else if (wcscmp(arg, L"--refresh") == 0) {
            options->refresh = 1;
        } else if (wcscmp(arg, L"--snapshots") == 0) {
            options->snapshots = 1;
        } else if (wcscmp(arg, L"--jvm") == 0) {
            char *utf8;
            if (++i >= argc) return -1;
            utf8 = wide_to_utf8(argv[i]);
            if (!utf8) return -1;
            add_string(&options->extra_jvm, &options->extra_jvm_count, utf8);
            free(utf8);
        } else if (wcscmp(arg, L"--game-arg") == 0) {
            char *utf8;
            if (++i >= argc) return -1;
            utf8 = wide_to_utf8(argv[i]);
            if (!utf8) return -1;
            add_string(&options->extra_game, &options->extra_game_count,
                       utf8);
            free(utf8);
        } else if (wcscmp(arg, L"-h") == 0 ||
                   wcscmp(arg, L"--help") == 0) {
            free(options->command);
            options->command = xstrdup("help");
        } else if (wcscmp(arg, L"-V") == 0 ||
                   wcscmp(arg, L"--version") == 0 ||
                   wcscmp(arg, L"--launcher-version") == 0) {
            free(options->command);
            options->command = xstrdup("version");
        } else if (arg[0] == L'-') {
            fprintf(stderr, "未知选项: %ls\n", arg);
            return -1;
        } else {
            char *utf8 = wide_to_utf8(arg);
            if (!utf8) return -1;
            if (!options->command) {
                if (strcmp(utf8, "launch") == 0 ||
                    strcmp(utf8, "install") == 0 ||
                    strcmp(utf8, "list") == 0 ||
                    strcmp(utf8, "remote") == 0 ||
                    strcmp(utf8, "help") == 0) {
                    options->command = xstrdup(utf8);
                    free(utf8);
                } else {
                    options->command = xstrdup("launch");
                    options->version = utf8;
                }
            } else if (!options->version &&
                       (strcmp(options->command, "launch") == 0 ||
                        strcmp(options->command, "install") == 0)) {
                options->version = utf8;
            } else {
                fprintf(stderr, "多余的参数: %s\n", utf8);
                free(utf8);
                return -1;
            }
        }
    }
    if ((options->width > 0) != (options->height > 0)) {
        fprintf(stderr, "--width 和 --height 必须同时指定。\n");
        return -1;
    }
    if (options->username && !*options->username) {
        fprintf(stderr, "用户名不能为空。\n");
        return -1;
    }
    return 0;
}

static void free_cli(CliOptions *options)
{
    size_t i;
    free(options->game_dir);
    free(options->username);
    free(options->uuid);
    free(options->java);
    free(options->command);
    free(options->version);
    for (i = 0; i < options->extra_jvm_count; i++) {
        free(options->extra_jvm[i]);
    }
    free(options->extra_jvm);
    for (i = 0; i < options->extra_game_count; i++) {
        free(options->extra_game[i]);
    }
    free(options->extra_game);
}

static int compare_strings(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return _stricmp(sa, sb);
}

static char **list_local_versions(const wchar_t *game_dir, size_t *count)
{
    wchar_t *versions_dir = path_join(game_dir, L"versions");
    wchar_t *pattern = path_join(versions_dir, L"*");
    WIN32_FIND_DATAW data;
    HANDLE handle;
    char **items = NULL;
    size_t n = 0;
    size_t cap = 0;

    *count = 0;
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        free(versions_dir);
        free(pattern);
        return NULL;
    }
    do {
        wchar_t *json;
        char *id;
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        id = wide_to_utf8(data.cFileName);
        if (!id) {
            continue;
        }
        json = remote_version_json_path(game_dir, id);
        if (json && path_exists(json)) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                items = (char **)xrealloc(items, cap * sizeof(char *));
            }
            items[n++] = id;
        } else {
            free(id);
        }
        free(json);
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    free(versions_dir);
    free(pattern);
    if (n > 1) {
        qsort(items, n, sizeof(char *), compare_strings);
    }
    *count = n;
    return items;
}

static void free_local_versions(char **items, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static void print_local_versions(const wchar_t *game_dir)
{
    size_t count;
    char **items = list_local_versions(game_dir, &count);
    size_t i;

    printf("本地版本（%ls）:\n", game_dir);
    if (count == 0) {
        printf("  （没有找到已安装版本）\n");
    }
    for (i = 0; i < count; i++) {
        printf("  %s\n", items[i]);
    }
    free_local_versions(items, count);
}

static char *prepare_version(CliOptions *options, const char *requested,
                             char **error)
{
    int alias = strcmp(requested, "latest") == 0 ||
                strcmp(requested, "latest-snapshot") == 0;
    wchar_t *local_path = remote_version_json_path(options->game_dir,
                                                   requested);
    RemoteManifest manifest;
    const RemoteVersion *entry;
    char *resolved = NULL;

    if (!alias && !options->refresh && local_path && path_exists(local_path)) {
        free(local_path);
        return xstrdup(requested);
    }
    free(local_path);

    if (remote_manifest_fetch(&manifest, error) != 0) {
        if (!alias) {
            local_path = remote_version_json_path(options->game_dir, requested);
            if (local_path && path_exists(local_path)) {
                printf("[!] 无法获取版本清单，使用本地版本。\n");
                free(local_path);
                if (error) {
                    free(*error);
                    *error = NULL;
                }
                return xstrdup(requested);
            }
            free(local_path);
        }
        return NULL;
    }
    entry = remote_find(&manifest, requested);
    if (!entry) {
        if (!alias) {
            local_path = remote_version_json_path(options->game_dir, requested);
            if (local_path && path_exists(local_path)) {
                printf("[!] 版本清单中没有 %s，使用本地版本。\n", requested);
                free(local_path);
                remote_manifest_free(&manifest);
                return xstrdup(requested);
            }
            free(local_path);
        }
        set_error(error, "找不到版本: %s", requested);
        remote_manifest_free(&manifest);
        return NULL;
    }
    printf("[*] 解析版本 %s -> %s\n", requested, entry->id);
    if (remote_install_json(options->game_dir, entry,
                            options->refresh || alias,
                            options->verify, error) != 0) {
        remote_manifest_free(&manifest);
        return NULL;
    }
    resolved = xstrdup(entry->id);
    remote_manifest_free(&manifest);
    return resolved;
}

static void fill_launch_options(const CliOptions *options,
                                LaunchOptions *launch)
{
    memset(launch, 0, sizeof(*launch));
    launch->game_dir = options->game_dir;
    launch->username = options->username;
    launch->uuid = options->uuid;
    launch->java = options->java;
    launch->memory_mb = options->memory_mb;
    launch->width = options->width;
    launch->height = options->height;
    launch->demo = options->demo;
    launch->detach = options->detach;
    launch->dry_run = options->dry_run;
    launch->extra_jvm = (const char *const *)options->extra_jvm;
    launch->extra_jvm_count = options->extra_jvm_count;
    launch->extra_game = (const char *const *)options->extra_game;
    launch->extra_game_count = options->extra_game_count;
}

static int command_launch(CliOptions *options, const char *requested)
{
    char *error = NULL;
    char *version_id = prepare_version(options, requested, &error);
    Version version;
    int rc;

    if (!version_id) {
        fprintf(stderr, "[错误] %s\n", error ? error : "准备版本失败");
        free(error);
        return 1;
    }
    if (version_load(options->game_dir, version_id, &version, &error) != 0) {
        fprintf(stderr, "[错误] %s\n", error ? error : "加载版本失败");
        free(error);
        free(version_id);
        return 1;
    }
    printf("[*] 版本: %s（Java %d+，类型 %s）\n", version.id,
           version.java_major, version.version_type ? version.version_type :
           "release");
    if (install_version(&version, options->game_dir, options->verify,
                        &error) != 0) {
        fprintf(stderr, "[错误] 安装失败: %s\n",
                error ? error : "未知错误");
        free(error);
        version_free(&version);
        free(version_id);
        return 1;
    }
    {
        LaunchOptions launch;
        fill_launch_options(options, &launch);
        rc = launch_version(&launch, &version, &error);
    }
    if (rc < 0) {
        fprintf(stderr, "[错误] 启动失败: %s\n",
                error ? error : "未知错误");
        free(error);
        rc = 1;
    }
    version_free(&version);
    free(version_id);
    return rc;
}

static int command_install(CliOptions *options, const char *requested)
{
    char *error = NULL;
    char *version_id = prepare_version(options, requested, &error);
    Version version;

    if (!version_id) {
        fprintf(stderr, "[错误] %s\n", error ? error : "准备版本失败");
        free(error);
        return 1;
    }
    if (version_load(options->game_dir, version_id, &version, &error) != 0) {
        fprintf(stderr, "[错误] %s\n", error ? error : "加载版本失败");
        free(error);
        free(version_id);
        return 1;
    }
    printf("[*] 安装 %s（Java %d+）\n", version.id, version.java_major);
    if (install_version(&version, options->game_dir, options->verify,
                        &error) != 0) {
        fprintf(stderr, "[错误] 安装失败: %s\n",
                error ? error : "未知错误");
        free(error);
        version_free(&version);
        free(version_id);
        return 1;
    }
    printf("[√] 安装完成: %s\n", version.id);
    version_free(&version);
    free(version_id);
    return 0;
}

static int command_remote(CliOptions *options)
{
    RemoteManifest manifest;
    char *error = NULL;
    size_t i;
    size_t shown = 0;

    if (remote_manifest_fetch(&manifest, &error) != 0) {
        fprintf(stderr, "[错误] %s\n", error ? error : "获取版本清单失败");
        free(error);
        return 1;
    }
    printf("Mojang 官方版本（最新正式版: %s，最新快照: %s）\n",
           manifest.latest_release ? manifest.latest_release : "?",
           manifest.latest_snapshot ? manifest.latest_snapshot : "?");
    for (i = 0; i < manifest.count && shown < 60; i++) {
        const RemoteVersion *v = &manifest.versions[i];
        if (!options->snapshots && strcmp(v->type, "release") != 0) {
            continue;
        }
        printf("  %-18s %-9s%s\n", v->id, v->type,
               (manifest.latest_release &&
                strcmp(v->id, manifest.latest_release) == 0) ?
               "  <- latest" : "");
        shown++;
    }
    if (shown == 0) {
        printf("  （没有匹配版本）\n");
    }
    remote_manifest_free(&manifest);
    return 0;
}

static int read_line(char *buffer, size_t size)
{
    size_t len;
    if (!fgets(buffer, (int)size, stdin)) {
        return -1;
    }
    len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        buffer[--len] = '\0';
    }
    return 0;
}

static int interactive(CliOptions *options)
{
    size_t count;
    char **versions;
    char line[512];
    char *chosen = NULL;
    int rc = 0;

    printf("==============================================\n");
    printf("        MCLC - Minecraft Java 启动器\n");
    printf("==============================================\n");
    versions = list_local_versions(options->game_dir, &count);
    if (count == 0) {
        printf("没有找到本地版本。\n");
        printf("是否下载并安装最新正式版？[Y/n]: ");
        fflush(stdout);
        if (read_line(line, sizeof(line)) != 0) {
            free_local_versions(versions, count);
            return 0;
        }
        if (line[0] && (_stricmp(line, "n") == 0 ||
                        _stricmp(line, "no") == 0)) {
            printf("已取消。\n");
            free_local_versions(versions, count);
            return 0;
        }
        if (command_install(options, "latest") != 0) {
            free_local_versions(versions, count);
            return 1;
        }
        versions = list_local_versions(options->game_dir, &count);
    }

    printf("本地版本:\n");
    {
        size_t i;
        for (i = 0; i < count; i++) {
            printf("  [%zu] %s\n", i + 1, versions[i]);
        }
    }
    printf("选择版本（序号或 ID，直接回车选择 1）: ");
    fflush(stdout);
    if (read_line(line, sizeof(line)) != 0) {
        free_local_versions(versions, count);
        return 0;
    }
    if (!line[0] && count > 0) {
        chosen = xstrdup(versions[0]);
    } else if (line[0] >= '0' && line[0] <= '9') {
        char *end = NULL;
        long index = strtol(line, &end, 10);
        if (end && *end == '\0' && index >= 1 && (size_t)index <= count) {
            chosen = xstrdup(versions[index - 1]);
        }
    }
    if (!chosen) {
        chosen = xstrdup(line);
    }
    free_local_versions(versions, count);
    if (!chosen || !*chosen) {
        free(chosen);
        return 0;
    }

    printf("用户名 [%s]: ", options->username ? options->username : "Steve");
    fflush(stdout);
    if (read_line(line, sizeof(line)) == 0 && line[0]) {
        free(options->username);
        options->username = xstrdup(line);
    }
    printf("最大内存 MB [%d]: ", options->memory_mb);
    fflush(stdout);
    if (read_line(line, sizeof(line)) == 0 && line[0]) {
        char *end = NULL;
        long mem = strtol(line, &end, 10);
        if (end && *end == '\0' && mem >= 256 && mem <= 65536) {
            options->memory_mb = (int)mem;
        } else {
            printf("内存值无效，继续使用 %d MB。\n", options->memory_mb);
        }
    }
    rc = command_launch(options, chosen);
    free(chosen);
    return rc;
}

int main(void)
{
    int argc = 0;
    wchar_t **argv;
    CliOptions options;
    int rc = 1;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if (!setlocale(LC_CTYPE, ".UTF-8")) {
        setlocale(LC_CTYPE, ".UTF8");
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        fprintf(stderr, "无法读取命令行参数。\n");
        return 1;
    }
    if (parse_cli(argc, argv, &options) != 0) {
        fprintf(stderr, "参数错误，使用 -h 查看帮助。\n");
        LocalFree(argv);
        free_cli(&options);
        return 1;
    }
    LocalFree(argv);

    if (!options.command) {
        if (!options.game_dir) {
            options.game_dir = get_default_game_dir();
        }
        {
            wchar_t *absolute = absolute_path(options.game_dir);
            free(options.game_dir);
            options.game_dir = absolute;
        }
        if (!options.game_dir || mkdir_p(options.game_dir) != 0) {
            fprintf(stderr, "[错误] 无法创建游戏目录。\n");
            free_cli(&options);
            return 1;
        }
        if (!_isatty(_fileno(stdin))) {
            fprintf(stderr,
                    "没有指定命令。请使用 mclc launch <版本> 或运行 -h。\n");
            free_cli(&options);
            return 1;
        }
        rc = interactive(&options);
        free_cli(&options);
        return rc;
    }

    if (strcmp(options.command, "help") == 0) {
        print_help();
        free_cli(&options);
        return 0;
    }
    if (strcmp(options.command, "version") == 0) {
        printf("MCLC %s\n", MCLC_VERSION);
        free_cli(&options);
        return 0;
    }
    if (!options.game_dir) {
        options.game_dir = get_default_game_dir();
    }
    {
        wchar_t *absolute = absolute_path(options.game_dir);
        free(options.game_dir);
        options.game_dir = absolute;
    }
    if (!options.game_dir) {
        fprintf(stderr, "[错误] 游戏目录无效。\n");
        free_cli(&options);
        return 1;
    }

    if (strcmp(options.command, "list") == 0) {
        print_local_versions(options.game_dir);
        rc = 0;
    } else if (strcmp(options.command, "remote") == 0) {
        rc = command_remote(&options);
    } else if (strcmp(options.command, "install") == 0) {
        if (!options.version || !*options.version) {
            fprintf(stderr, "usage: mclc install <版本>\n");
            rc = 1;
        } else {
            rc = command_install(&options, options.version);
        }
    } else if (strcmp(options.command, "launch") == 0) {
        if (!options.version || !*options.version) {
            fprintf(stderr, "usage: mclc launch <版本>\n");
            rc = 1;
        } else {
            rc = command_launch(&options, options.version);
        }
    } else {
        fprintf(stderr, "未知命令: %s\n", options.command);
        rc = 1;
    }
    free_cli(&options);
    return rc;
}
