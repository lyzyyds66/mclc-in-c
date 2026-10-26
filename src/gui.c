/*
 * MCLC GUI - native Win32 front-end.
 *
 * Core progress output is redirected into a pipe, combined with GUI operation
 * records, rendered in the log control and kept for export. No console window
 * is created.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "util.h"
#include "hash.h"
#include "launch.h"
#include "remote.h"
#include "version.h"
#include "install.h"
#include "progress.h"

#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>

#include <fcntl.h>
#include <io.h>
#include <locale.h>
#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MCLC_GUI_CLASS L"MCLCGuiWindow"
#define MCLC_PROGRESS_CLASS L"MCLCDownloadProgress"
#define MCLC_PICKER_CLASS L"MCLCDownloadPicker"
#define MCLC_USER_CLASS L"MCLCUserWindow"
#define MCLC_NEWUSER_CLASS L"MCLCNewUserWindow"
#define MCLC_SETTINGS_CLASS L"MCLCSettingsWindow"
#define MCLC_GUI_TITLE L"MCLC - Minecraft Java 启动器"
#define WM_APP_JOB_DONE (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_PICKER_LOADED (WM_APP + 3)

enum {
    ID_GAME_DIR = 100,
    ID_BROWSE_GAME,
    ID_VERSION,
    ID_REFRESH,
    ID_USER,
    ID_MEMORY,
    ID_JAVA,
    ID_BROWSE_JAVA,
    ID_DEMO,
    ID_LAUNCH,
    ID_USER_BUTTON,
    ID_SETTINGS,
    ID_DOWNLOAD,
    ID_OPEN_DIR,
    ID_LOG,
    ID_STATUS,
    ID_EXPORT_LOG,

    ID_PICK_CATEGORY = 300,
    ID_PICK_LIST,
    ID_PICK_CONFIRM,

    ID_PROFILE_COMBO = 400,
    ID_PROFILE_NEW,
    ID_PROFILE_OK,
    ID_PROFILE_CANCEL,

    ID_NEW_NAME = 450,
    ID_NEW_UUID,
    ID_NEW_SAVE,
    ID_NEW_CANCEL,

    ID_SET_ISOLATION = 500,
    ID_SET_TITLE,
    ID_SET_INFO,
    ID_SET_PRIORITY,
    ID_SET_MODE,
    ID_SET_WIDTH,
    ID_SET_HEIGHT,
    ID_SET_JVM,
    ID_SET_GAME,
    ID_SET_PRE,
    ID_SET_OK,
    ID_SET_CANCEL
};

enum {
    JOB_LAUNCH = 1,
    JOB_DOWNLOAD = 2
};

typedef struct {
    HWND hwnd;
    int kind;
    wchar_t *game_dir;
    char *version_id;
    char *json_url;
    char *json_sha1;
    char *username;
    char *uuid;
    wchar_t *java;
    int memory_mb;
    int demo;
    int verify;
    wchar_t *window_title;
    wchar_t *custom_info;
    wchar_t *pre_launch;
    int fullscreen;
    int priority;
    char **jvm_args;
    size_t jvm_count;
    char **game_args;
    size_t game_count;
} GuiJob;

typedef struct {
    wchar_t *path;
    wchar_t *label;
} JavaEntry;

#define MCLC_MAX_PROFILES 64

typedef struct {
    wchar_t *name;
    wchar_t *uuid;
} Profile;

typedef struct {
    int version_isolation;
    wchar_t *window_title;
    wchar_t *custom_info;
    int priority;      /* 0 low, 1 normal, 2 high */
    int window_mode;   /* 0 default, 1 custom, 2 fullscreen */
    int width;
    int height;
    wchar_t *jvm_args;
    wchar_t *game_args;
    wchar_t *pre_launch;
} Settings;

static Profile *g_profiles;
static size_t g_profile_count;
static size_t g_profile_current;
static Settings g_settings;
static wchar_t *g_config_path;

static JavaEntry *g_java_entries;
static size_t g_java_entry_count;

static HINSTANCE g_instance;
static HWND g_hwnd;
static HWND g_game_dir;
static HWND g_browse_game;
static HWND g_version;
static HWND g_refresh;
static HWND g_memory;
static HWND g_user_button;
static HWND g_settings_button;
static HWND g_java;
static HWND g_browse_java;
static HWND g_demo;
static HWND g_launch;
static HWND g_download;
static HWND g_open_dir;
static HWND g_log;
static HWND g_status;
static HWND g_export_log;
static HWND g_progress;
static HWND g_progress_bar;
static HWND g_progress_text;
static HFONT g_font;
static HFONT g_log_font;
static HANDLE g_log_read;
static volatile LONG g_running;
static volatile LONG g_picker_loading;
static char *g_installed_version;

#define MCLC_LOG_LIMIT (16u * 1024u * 1024u)
static CRITICAL_SECTION g_log_lock;
static int g_log_lock_ready;
static wchar_t *g_log_ui;
static size_t g_log_ui_len;
static size_t g_log_ui_cap;
static volatile LONG g_log_dirty;

static void gui_logf(const wchar_t *fmt, ...);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static wchar_t *append_json_suffix(const wchar_t *base)
{
    size_t len = wcslen(base);
    wchar_t *out = (wchar_t *)xmalloc((len + 6) * sizeof(wchar_t));
    wcscpy(out, base);
    wcscat(out, L".json");
    return out;
}

static wchar_t *get_control_text(HWND control)
{
    int len = GetWindowTextLengthW(control);
    wchar_t *text = (wchar_t *)xmalloc(((size_t)len + 1) * sizeof(wchar_t));
    GetWindowTextW(control, text, len + 1);
    return text;
}

static wchar_t *get_default_game_dir(void)
{
    DWORD size = GetEnvironmentVariableW(L"APPDATA", NULL, 0);
    wchar_t *appdata;
    wchar_t *path;

    if (size == 0) {
        return xwcsdup(L".minecraft");
    }
    appdata = (wchar_t *)xmalloc(((size_t)size + 1) * sizeof(wchar_t));
    if (GetEnvironmentVariableW(L"APPDATA", appdata, size + 1) == 0) {
        free(appdata);
        return xwcsdup(L".minecraft");
    }
    path = path_join(appdata, L".minecraft");
    free(appdata);
    return path;
}

static wchar_t *absolute_path_w(const wchar_t *path)
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

static wchar_t *get_game_dir_value(void)
{
    wchar_t *text = get_control_text(g_game_dir);
    wchar_t *absolute;

    if (!*text) {
        free(text);
        text = get_default_game_dir();
        SetWindowTextW(g_game_dir, text);
    }
    absolute = absolute_path_w(text);
    free(text);
    return absolute;
}

static void set_status(const wchar_t *text)
{
    SetWindowTextW(g_status, text);
}

/* ------------------------------------------------------------------ */
/* Configuration (mclc_config.ini next to the executable)              */
/* ------------------------------------------------------------------ */

static void replace_wide(wchar_t **target, const wchar_t *value)
{
    free(*target);
    *target = xwcsdup(value ? value : L"");
}

static wchar_t *exe_directory(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);

    if (n == 0 || n >= MAX_PATH) {
        return NULL;
    }
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') {
        n--;
    }
    path[n] = L'\0';
    return xwcsdup(path[0] ? path : L".");
}

static char *trim_ascii(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return text;
}

static void free_profiles(void)
{
    size_t i;
    for (i = 0; i < g_profile_count; i++) {
        free(g_profiles[i].name);
        free(g_profiles[i].uuid);
    }
    free(g_profiles);
    g_profiles = NULL;
    g_profile_count = 0;
    g_profile_current = 0;
}

static void set_default_settings(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.priority = 1;
    g_settings.window_mode = 0;
    g_settings.width = 854;
    g_settings.height = 480;
    g_settings.window_title = xwcsdup(L"");
    g_settings.custom_info = xwcsdup(L"");
    g_settings.jvm_args = xwcsdup(L"");
    g_settings.game_args = xwcsdup(L"");
    g_settings.pre_launch = xwcsdup(L"");
}

static void ensure_default_profile(void)
{
    char uuid[37];
    if (g_profile_count > 0) {
        if (g_profile_current >= g_profile_count) {
            g_profile_current = 0;
        }
        return;
    }
    if (offline_uuid_for_name("Steve", uuid) != 0) {
        strcpy(uuid, "00000000-0000-3000-8000-000000000000");
    }
    g_profiles = (Profile *)xcalloc(1, sizeof(Profile));
    g_profiles[0].name = xwcsdup(L"Steve");
    {
        wchar_t *wide_uuid = utf8_to_wide(uuid);
        g_profiles[0].uuid = wide_uuid ? wide_uuid : xwcsdup(L"");
    }
    g_profile_count = 1;
    g_profile_current = 0;
}

static void load_config(void)
{
    wchar_t *dir;
    char *data = NULL;
    char *line;
    char *tmp_names[MCLC_MAX_PROFILES];
    char *tmp_uuids[MCLC_MAX_PROFILES];
    int section = 0;
    int parsed_count = 0;
    int parsed_current = 0;
    int parsed_max = -1;
    size_t i;

    memset(tmp_names, 0, sizeof(tmp_names));
    memset(tmp_uuids, 0, sizeof(tmp_uuids));
    set_default_settings();
    free_profiles();

    dir = exe_directory();
    g_config_path = dir ? path_join(dir, L"mclc_config.ini") : NULL;
    free(dir);
    if (g_config_path) {
        data = read_text_file(g_config_path, NULL, NULL);
    }

    if (data) {
        line = data;
        if (strlen(line) >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF) {
            line += 3;
        }
        while (*line) {
            char *next = strchr(line, '\n');
            char *text;
            char *eq;
            if (next) {
                *next = '\0';
            }
            text = trim_ascii(line);
            if (*text == '\0' || *text == ';' || *text == '#') {
                /* skip */
            } else if (text[0] == '[') {
                if (_stricmp(text, "[profiles]") == 0) {
                    section = 1;
                } else if (_stricmp(text, "[settings]") == 0) {
                    section = 2;
                } else {
                    section = 0;
                }
            } else if ((eq = strchr(text, '=')) != NULL) {
                char *key;
                char *value;
                *eq = '\0';
                key = trim_ascii(text);
                value = trim_ascii(eq + 1);
                if (section == 1) {
                    if (_stricmp(key, "count") == 0) {
                        parsed_count = atoi(value);
                    } else if (_stricmp(key, "current") == 0) {
                        parsed_current = atoi(value);
                    } else if (_strnicmp(key, "name", 4) == 0) {
                        int index = atoi(key + 4);
                        if (index >= 0 && index < MCLC_MAX_PROFILES) {
                            free(tmp_names[index]);
                            tmp_names[index] = xstrdup(value);
                            if (index > parsed_max) parsed_max = index;
                        }
                    } else if (_strnicmp(key, "uuid", 4) == 0) {
                        int index = atoi(key + 4);
                        if (index >= 0 && index < MCLC_MAX_PROFILES) {
                            free(tmp_uuids[index]);
                            tmp_uuids[index] = xstrdup(value);
                            if (index > parsed_max) parsed_max = index;
                        }
                    }
                } else if (section == 2) {
                    if (_stricmp(key, "version_isolation") == 0) {
                        g_settings.version_isolation = atoi(value) != 0;
                    } else if (_stricmp(key, "window_title") == 0) {
                        wchar_t *w = utf8_to_wide(value);
                        replace_wide(&g_settings.window_title, w);
                        free(w);
                    } else if (_stricmp(key, "custom_info") == 0) {
                        wchar_t *w = utf8_to_wide(value);
                        replace_wide(&g_settings.custom_info, w);
                        free(w);
                    } else if (_stricmp(key, "priority") == 0) {
                        int p = atoi(value);
                        if (p >= 0 && p <= 2) g_settings.priority = p;
                    } else if (_stricmp(key, "window_mode") == 0) {
                        int m = atoi(value);
                        if (m >= 0 && m <= 2) g_settings.window_mode = m;
                    } else if (_stricmp(key, "width") == 0) {
                        int w = atoi(value);
                        if (w > 0) g_settings.width = w;
                    } else if (_stricmp(key, "height") == 0) {
                        int h = atoi(value);
                        if (h > 0) g_settings.height = h;
                    } else if (_stricmp(key, "jvm_args") == 0) {
                        wchar_t *w = utf8_to_wide(value);
                        replace_wide(&g_settings.jvm_args, w);
                        free(w);
                    } else if (_stricmp(key, "game_args") == 0) {
                        wchar_t *w = utf8_to_wide(value);
                        replace_wide(&g_settings.game_args, w);
                        free(w);
                    } else if (_stricmp(key, "pre_launch") == 0) {
                        wchar_t *w = utf8_to_wide(value);
                        replace_wide(&g_settings.pre_launch, w);
                        free(w);
                    }
                }
            }
            if (!next) {
                break;
            }
            line = next + 1;
        }
        free(data);
    }

    if (parsed_count <= 0) {
        parsed_count = parsed_max + 1;
    }
    if (parsed_count > MCLC_MAX_PROFILES) {
        parsed_count = MCLC_MAX_PROFILES;
    }
    for (i = 0; i < (size_t)parsed_count; i++) {
        if (tmp_names[i] && *tmp_names[i]) {
            g_profiles = (Profile *)xrealloc(
                g_profiles, (g_profile_count + 1) * sizeof(Profile));
            g_profiles[g_profile_count].name = utf8_to_wide(tmp_names[i]);
            if (!g_profiles[g_profile_count].name) {
                g_profiles[g_profile_count].name = xwcsdup(L"Player");
            }
            if (tmp_uuids[i] && *tmp_uuids[i]) {
                g_profiles[g_profile_count].uuid =
                    utf8_to_wide(tmp_uuids[i]);
            } else {
                char uuid[37];
                if (offline_uuid_for_name(tmp_names[i], uuid) != 0) {
                    strcpy(uuid, "00000000-0000-3000-8000-000000000000");
                }
                g_profiles[g_profile_count].uuid = utf8_to_wide(uuid);
            }
            if (!g_profiles[g_profile_count].uuid) {
                g_profiles[g_profile_count].uuid = xwcsdup(L"");
            }
            g_profile_count++;
        }
        free(tmp_names[i]);
        free(tmp_uuids[i]);
    }
    ensure_default_profile();
    if (parsed_current >= 0 &&
        (size_t)parsed_current < g_profile_count) {
        g_profile_current = (size_t)parsed_current;
    } else {
        g_profile_current = 0;
    }
    gui_logf(L"配置文件：%ls（%zu 个用户名）",
             g_config_path ? g_config_path : L"(无法定位)",
             g_profile_count);
}

static char *config_value_utf8(const wchar_t *text)
{
    wchar_t *copy = xwcsdup(text ? text : L"");
    wchar_t *p;
    char *result;

    for (p = copy; *p; p++) {
        if (*p == L'\r' || *p == L'\n') {
            *p = L' ';
        }
    }
    result = wide_to_utf8(copy);
    free(copy);
    return result;
}

static void save_config(void)
{
    StrBuf sb;
    size_t i;
    int ok;

    if (!g_config_path) {
        return;
    }
    ensure_default_profile();
    sb_init(&sb);
    sb_append(&sb, "[profiles]\r\n");
    {
        char *line = format_utf8("count=%zu\r\ncurrent=%zu\r\n",
                                 g_profile_count, g_profile_current);
        sb_append(&sb, line);
        free(line);
    }
    for (i = 0; i < g_profile_count; i++) {
        char *name = wide_to_utf8(g_profiles[i].name);
        char *uuid = wide_to_utf8(g_profiles[i].uuid);
        char *line = format_utf8("name%zu=%s\r\nuuid%zu=%s\r\n",
                                 i, name ? name : "Player",
                                 i, uuid ? uuid : "");
        sb_append(&sb, line);
        free(line);
        free(name);
        free(uuid);
    }
    sb_append(&sb, "[settings]\r\n");
    {
        char *line = format_utf8(
            "version_isolation=%d\r\n"
            "priority=%d\r\n"
            "window_mode=%d\r\n"
            "width=%d\r\n"
            "height=%d\r\n",
            g_settings.version_isolation,
            g_settings.priority,
            g_settings.window_mode,
            g_settings.width,
            g_settings.height);
        sb_append(&sb, line);
        free(line);
    }
    {
        char *title = config_value_utf8(g_settings.window_title);
        char *info = config_value_utf8(g_settings.custom_info);
        char *jvm = config_value_utf8(g_settings.jvm_args);
        char *game = config_value_utf8(g_settings.game_args);
        char *pre = config_value_utf8(g_settings.pre_launch);
        char *line = format_utf8(
            "window_title=%s\r\n"
            "custom_info=%s\r\n"
            "jvm_args=%s\r\n"
            "game_args=%s\r\n"
            "pre_launch=%s\r\n",
            title ? title : "", info ? info : "",
            jvm ? jvm : "", game ? game : "", pre ? pre : "");
        sb_append(&sb, line);
        free(line);
        free(title);
        free(info);
        free(jvm);
        free(game);
        free(pre);
    }
    ok = write_file_all(g_config_path, sb.data, sb.len);
    sb_free(&sb);
    if (ok != 0) {
        gui_logf(L"保存配置失败：%ls", g_config_path);
    } else {
        gui_logf(L"配置已保存：%ls", g_config_path);
    }
}

static Profile *current_profile(void)
{
    if (g_profile_count == 0 ||
        g_profile_current >= g_profile_count) {
        return NULL;
    }
    return &g_profiles[g_profile_current];
}

/* ------------------------------------------------------------------ */
/* Java discovery                                                      */
/* ------------------------------------------------------------------ */

static void free_java_entries(void)
{
    size_t i;
    for (i = 0; i < g_java_entry_count; i++) {
        free(g_java_entries[i].path);
        free(g_java_entries[i].label);
    }
    free(g_java_entries);
    g_java_entries = NULL;
    g_java_entry_count = 0;
}

static wchar_t *java_home_from_exe(const wchar_t *java_exe)
{
    wchar_t *home = xwcsdup(java_exe);
    wchar_t *slash = wcsrchr(home, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    slash = wcsrchr(home, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    return home;
}

static void detect_java_version(const wchar_t *java_exe,
                                wchar_t *version, size_t version_count)
{
    wchar_t *home = java_home_from_exe(java_exe);
    wchar_t *release = path_join(home, L"release");
    char *data = read_text_file(release, NULL, NULL);
    char *found = NULL;

    version[0] = L'\0';
    if (data) {
        found = strstr(data, "JAVA_VERSION=\"");
        if (!found) {
            found = strstr(data, "JAVA_RUNTIME_VERSION=\"");
        }
        if (found) {
            char value[128];
            size_t i = 0;
            found = strchr(found, '"');
            if (found) {
                found++;
                while (found[i] && found[i] != '"' &&
                       i + 1 < sizeof(value)) {
                    value[i] = found[i];
                    i++;
                }
                value[i] = '\0';
                {
                    wchar_t *wide = utf8_to_wide(value);
                    if (wide) {
                        wcsncpy(version, wide, version_count - 1);
                        version[version_count - 1] = L'\0';
                        free(wide);
                    }
                }
            }
        }
        free(data);
    }
    if (version[0] == L'\0') {
        const wchar_t *name = wcsrchr(home, L'\\');
        name = name ? name + 1 : home;
        wcsncpy(version, name, version_count - 1);
        version[version_count - 1] = L'\0';
    }
    free(release);
    free(home);
}

static int java_entry_exists(const wchar_t *path)
{
    size_t i;
    for (i = 0; i < g_java_entry_count; i++) {
        if (_wcsicmp(g_java_entries[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int add_java_candidate(const wchar_t *candidate)
{
    wchar_t *path;
    wchar_t *full = NULL;
    wchar_t version[128];
    wchar_t *label;
    DWORD needed;
    int existing;

    if (!candidate || !*candidate) {
        return -1;
    }
    if (path_is_dir(candidate)) {
        path = path_join(candidate, L"bin\\java.exe");
        if (!path_exists(path)) {
            free(path);
            path = path_join(candidate, L"java.exe");
        }
    } else {
        path = xwcsdup(candidate);
    }
    if (!path || !path_exists(path) || path_is_dir(path)) {
        free(path);
        return -1;
    }

    needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed) {
        full = (wchar_t *)xmalloc(((size_t)needed + 1) * sizeof(wchar_t));
        if (GetFullPathNameW(path, needed + 1, full, NULL) == 0) {
            free(full);
            full = NULL;
        }
    }
    if (!full) {
        full = xwcsdup(path);
    }
    free(path);

    existing = java_entry_exists(full);
    if (existing >= 0) {
        free(full);
        return existing;
    }

    detect_java_version(full, version, 128);
    {
        size_t label_len = wcslen(version) + wcslen(full) + 32;
        label = (wchar_t *)xmalloc(label_len * sizeof(wchar_t));
        if (version[0]) {
            swprintf(label, label_len, L"Java %s (%s)", version, full);
        } else {
            swprintf(label, label_len, L"Java (%s)", full);
        }
    }

    g_java_entries = (JavaEntry *)xrealloc(
        g_java_entries, (g_java_entry_count + 1) * sizeof(JavaEntry));
    g_java_entries[g_java_entry_count].path = full;
    g_java_entries[g_java_entry_count].label = label;
    g_java_entry_count++;
    return (int)(g_java_entry_count - 1);
}

static void collect_java_children(const wchar_t *parent)
{
    wchar_t *pattern;
    wchar_t *candidate;
    WIN32_FIND_DATAW data;
    HANDLE handle;

    if (!parent || !path_is_dir(parent)) {
        return;
    }
    candidate = path_join(parent, L"bin\\java.exe");
    add_java_candidate(candidate);
    free(candidate);

    pattern = path_join(parent, L"*");
    handle = FindFirstFileW(pattern, &data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        {
            wchar_t *child = path_join(parent, data.cFileName);
            wchar_t *exe = path_join(child, L"bin\\java.exe");
            add_java_candidate(exe);
            free(exe);
            free(child);
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
}

static void collect_official_java(void)
{
    static const wchar_t *names[] = {
        L"java-runtime-delta", L"java-runtime-epsilon",
        L"java-runtime-gamma", L"java-runtime-alpha", L"jre-legacy"
    };
    const wchar_t *arch = version_is_windows_arm64()
                              ? L"windows-arm64" : L"windows-x64";
    wchar_t *bases[2] = { NULL, NULL };
    DWORD size;
    int b;
    size_t i;

    size = GetEnvironmentVariableW(L"ProgramFiles(x86)", NULL, 0);
    if (size) {
        bases[0] = (wchar_t *)xmalloc(((size_t)size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"ProgramFiles(x86)", bases[0], size + 1);
    }
    size = GetEnvironmentVariableW(L"ProgramFiles", NULL, 0);
    if (size) {
        bases[1] = (wchar_t *)xmalloc(((size_t)size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"ProgramFiles", bases[1], size + 1);
    }
    for (b = 0; b < 2; b++) {
        wchar_t *runtime;
        if (!bases[b]) continue;
        runtime = path_join(bases[b], L"Minecraft Launcher\\runtime");
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            wchar_t *name = path_join(runtime, names[i]);
            wchar_t *exe = path_join3(name, arch, names[i]);
            wchar_t *bin = path_join(exe, L"bin\\java.exe");
            add_java_candidate(bin);
            free(bin);
            free(exe);
            exe = path_join(name, L"bin\\java.exe");
            add_java_candidate(exe);
            free(exe);
            free(name);
        }
        free(runtime);
    }
    free(bases[0]);
    free(bases[1]);
}

static void collect_common_java(void)
{
    static const wchar_t *vendors[] = {
        L"Java", L"Eclipse Adoptium", L"Microsoft", L"Zulu", L"BellSoft"
    };
    wchar_t *program_files = NULL;
    DWORD size;
    size_t i;
    wchar_t *path;
    wchar_t *exe;

    size = GetEnvironmentVariableW(L"ProgramFiles", NULL, 0);
    if (size) {
        program_files = (wchar_t *)xmalloc(((size_t)size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"ProgramFiles", program_files, size + 1);
    }
    if (program_files) {
        for (i = 0; i < sizeof(vendors) / sizeof(vendors[0]); i++) {
            path = path_join(program_files, vendors[i]);
            collect_java_children(path);
            free(path);
        }
        free(program_files);
    }
    size = GetEnvironmentVariableW(L"ProgramFiles(x86)", NULL, 0);
    if (size) {
        path = (wchar_t *)xmalloc(((size_t)size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"ProgramFiles(x86)", path, size + 1);
        exe = path_join(path, L"Java");
        collect_java_children(exe);
        free(exe);
        free(path);
    }
    size = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
    if (size) {
        wchar_t *local = (wchar_t *)xmalloc(
            ((size_t)size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, size + 1);
        path = path_join(local, L"Programs\\Eclipse Adoptium");
        collect_java_children(path);
        free(path);
        free(local);
    }
}

static void collect_environment_java(void)
{
    const wchar_t *vars[] = { L"MCLC_JAVA", L"JAVA_HOME" };
    int i;
    for (i = 0; i < 2; i++) {
        DWORD size = GetEnvironmentVariableW(vars[i], NULL, 0);
        wchar_t *value;
        if (!size) {
            continue;
        }
        value = (wchar_t *)xmalloc(((size_t)size + 1) * sizeof(wchar_t));
        GetEnvironmentVariableW(vars[i], value, size + 1);
        if (!wcschr(value, L'\\') && !wcschr(value, L'/')) {
            DWORD needed = SearchPathW(NULL, value, NULL, 0, NULL, NULL);
            if (needed) {
                wchar_t *found = (wchar_t *)xmalloc(
                    ((size_t)needed + 1) * sizeof(wchar_t));
                if (SearchPathW(NULL, value, NULL, needed + 1, found,
                                NULL) != 0) {
                    add_java_candidate(found);
                }
                free(found);
            }
        } else {
            add_java_candidate(value);
        }
        free(value);
    }
    {
        DWORD needed = SearchPathW(NULL, L"java.exe", NULL, 0, NULL, NULL);
        if (needed) {
            wchar_t *found = (wchar_t *)xmalloc(
                ((size_t)needed + 1) * sizeof(wchar_t));
            if (SearchPathW(NULL, L"java.exe", NULL, needed + 1, found,
                            NULL) != 0) {
                add_java_candidate(found);
            }
            free(found);
        }
    }
}

static void refresh_java_list(void)
{
    size_t i;

    free_java_entries();
    SendMessageW(g_java, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_java, CB_ADDSTRING, 0,
                 (LPARAM)L"自动选择（推荐）");
    SendMessageW(g_java, CB_SETITEMDATA, 0, (LPARAM)-1);

    collect_environment_java();
    collect_official_java();
    collect_common_java();

    for (i = 0; i < g_java_entry_count; i++) {
        LRESULT index = SendMessageW(g_java, CB_ADDSTRING, 0,
                                     (LPARAM)g_java_entries[i].label);
        if (index != CB_ERR) {
            SendMessageW(g_java, CB_SETITEMDATA, (WPARAM)index,
                         (LPARAM)i);
        }
    }
    SendMessageW(g_java, CB_SETCURSEL, 0, 0);
    gui_logf(L"Java 检测完成，共 %zu 个版本。", g_java_entry_count);
}

static wchar_t *get_selected_java_path(void)
{
    LRESULT selection = SendMessageW(g_java, CB_GETCURSEL, 0, 0);
    LRESULT data;

    if (selection == CB_ERR || selection <= 0) {
        return NULL;
    }
    data = SendMessageW(g_java, CB_GETITEMDATA, (WPARAM)selection, 0);
    if (data < 0 || (size_t)data >= g_java_entry_count) {
        return NULL;
    }
    return xwcsdup(g_java_entries[data].path);
}

static void select_java_entry(int entry_index)
{
    int count;
    int i;

    if (entry_index < 0 || (size_t)entry_index >= g_java_entry_count) {
        return;
    }
    count = (int)SendMessageW(g_java, CB_GETCOUNT, 0, 0);
    for (i = 0; i < count; i++) {
        LRESULT data = SendMessageW(g_java, CB_GETITEMDATA, (WPARAM)i, 0);
        if ((int)data == entry_index) {
            SendMessageW(g_java, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    {
        LRESULT index = SendMessageW(
            g_java, CB_ADDSTRING, 0,
            (LPARAM)g_java_entries[entry_index].label);
        if (index != CB_ERR) {
            SendMessageW(g_java, CB_SETITEMDATA, (WPARAM)index,
                         (LPARAM)entry_index);
            SendMessageW(g_java, CB_SETCURSEL, (WPARAM)index, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Log pipe: stdout/stderr -> read-only edit control                   */
/* ------------------------------------------------------------------ */

static void set_log_view_text(const wchar_t *text)
{
    int len;

    if (!g_log || !text) {
        return;
    }
    SendMessageW(g_log, WM_SETTEXT, 0, (LPARAM)text);
    len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void refresh_log_view(void)
{
    wchar_t *snapshot = NULL;

    if (!g_log || !g_log_lock_ready) {
        return;
    }
    EnterCriticalSection(&g_log_lock);
    if (g_log_ui) {
        snapshot = xwcsdup(g_log_ui);
    }
    InterlockedExchange(&g_log_dirty, 0);
    LeaveCriticalSection(&g_log_lock);
    if (snapshot) {
        set_log_view_text(snapshot);
        free(snapshot);
    }
}

static void append_log_text(const wchar_t *text)
{
    size_t add;
    size_t needed;

    if (!text || !*text) {
        return;
    }
    add = wcslen(text);
    if (add > MCLC_LOG_LIMIT) {
        text += add - MCLC_LOG_LIMIT;
        add = MCLC_LOG_LIMIT;
    }
    if (!g_log_lock_ready) {
        return;
    }

    EnterCriticalSection(&g_log_lock);
    if (g_log_ui_len + add > MCLC_LOG_LIMIT) {
        size_t drop = g_log_ui_len + add - MCLC_LOG_LIMIT;
        if (drop < g_log_ui_len) {
            memmove(g_log_ui, g_log_ui + drop,
                    (g_log_ui_len - drop) * sizeof(wchar_t));
            g_log_ui_len -= drop;
        } else {
            g_log_ui_len = 0;
        }
    }
    needed = g_log_ui_len + add + 1;
    if (needed > g_log_ui_cap) {
        size_t cap = g_log_ui_cap ? g_log_ui_cap : 4096;
        while (cap < needed) {
            cap *= 2;
        }
        g_log_ui = (wchar_t *)xrealloc(g_log_ui, cap * sizeof(wchar_t));
        g_log_ui_cap = cap;
    }
    memcpy(g_log_ui + g_log_ui_len, text, add * sizeof(wchar_t));
    g_log_ui_len += add;
    g_log_ui[g_log_ui_len] = L'\0';
    InterlockedExchange(&g_log_dirty, 1);
    LeaveCriticalSection(&g_log_lock);
}

static void append_core_text(const wchar_t *text)
{
    static int at_line_start = 1;
    wchar_t stamp[40];
    SYSTEMTIME now;
    size_t len;
    size_t lines = 1;
    size_t cap;
    size_t i;
    size_t j = 0;
    wchar_t *out;

    if (!text || !*text) {
        return;
    }
    GetLocalTime(&now);
    swprintf(stamp, 40, L"[%04u-%02u-%02u %02u:%02u:%02u] ",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
             (unsigned)now.wHour, (unsigned)now.wMinute,
             (unsigned)now.wSecond);
    len = wcslen(text);
    for (i = 0; i < len; i++) {
        if (text[i] == L'\n') lines++;
    }
    cap = len + (lines + 1) * wcslen(stamp) + 2;
    out = (wchar_t *)xmalloc(cap * sizeof(wchar_t));
    for (i = 0; i < len; i++) {
        if (at_line_start) {
            size_t sl = wcslen(stamp);
            memcpy(out + j, stamp, sl * sizeof(wchar_t));
            j += sl;
            at_line_start = 0;
        }
        out[j++] = text[i];
        if (text[i] == L'\n') {
            at_line_start = 1;
        }
    }
    out[j] = L'\0';
    append_log_text(out);
    free(out);
}

static void gui_logf(const wchar_t *fmt, ...)
{
    wchar_t message[4096];
    wchar_t line[4224];
    SYSTEMTIME now;
    va_list args;

    if (!fmt) {
        return;
    }
    message[0] = L'\0';
    va_start(args, fmt);
    vswprintf(message, 4096, fmt, args);
    va_end(args);
    message[4095] = L'\0';

    GetLocalTime(&now);
    swprintf(line, 4224, L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
             (unsigned)now.wHour, (unsigned)now.wMinute,
             (unsigned)now.wSecond, message);
    append_log_text(line);
}

static void log_feed(const char *data, size_t len)
{
    static unsigned char pending[8];
    static size_t pending_len;
    char combined[4104];
    size_t total;
    size_t i;
    size_t valid;
    int wide_len;
    wchar_t *wide;
    wchar_t *out;
    size_t j;

    if (len > 4096) {
        len = 4096;
    }
    total = pending_len + len;
    if (total > sizeof(combined)) {
        total = sizeof(combined);
    }
    if (pending_len) {
        memcpy(combined, pending, pending_len);
    }
    memcpy(combined + pending_len, data, len);

    i = 0;
    while (i < total) {
        unsigned char c = (unsigned char)combined[i];
        size_t need;
        if (c < 0x80) {
            need = 1;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        } else {
            i++;
            continue;
        }
        if (i + need > total) {
            break;
        }
        i += need;
    }
    valid = i;
    pending_len = total - valid;
    if (pending_len) {
        memcpy(pending, combined + valid, pending_len);
    }
    if (valid == 0) {
        return;
    }

    wide_len = MultiByteToWideChar(CP_UTF8, 0, combined, (int)valid, NULL, 0);
    if (wide_len <= 0) {
        return;
    }
    wide = (wchar_t *)xmalloc(((size_t)wide_len + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, combined, (int)valid, wide, wide_len);
    wide[wide_len] = L'\0';

    out = (wchar_t *)xmalloc(((size_t)wide_len * 2 + 1) * sizeof(wchar_t));
    j = 0;
    for (i = 0; i < (size_t)wide_len; i++) {
        wchar_t c = wide[i];
        if (c == L'\r') {
            if (i + 1 < (size_t)wide_len && wide[i + 1] == L'\n') {
                continue;
            }
            out[j++] = L'\r';
            out[j++] = L'\n';
        } else if (c == L'\n') {
            out[j++] = L'\r';
            out[j++] = L'\n';
        } else {
            out[j++] = c;
        }
    }
    out[j] = L'\0';
    append_core_text(out);

    free(out);
    free(wide);
}

static unsigned __stdcall log_reader_thread(void *parameter)
{
    char buffer[4096];
    DWORD read;

    (void)parameter;
    while (g_log_read &&
           ReadFile(g_log_read, buffer, sizeof(buffer), &read, NULL) &&
           read > 0) {
        log_feed(buffer, (size_t)read);
    }
    return 0;
}

static void setup_log_pipe(void)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE read_end;
    HANDLE write_end;
    int fd;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);

    fd = _open_osfhandle((intptr_t)write_end, _O_WRONLY | _O_BINARY);
    if (fd == -1) {
        CloseHandle(write_end);
        CloseHandle(read_end);
        return;
    }
    if (_dup2(fd, _fileno(stdout)) == -1 ||
        _dup2(fd, _fileno(stderr)) == -1) {
        _close(fd);
        CloseHandle(read_end);
        return;
    }
    /*
     * Keep fd open. _dup2 gives stdout/stderr their own descriptors in the
     * CRT; closing this one is unnecessary and can close the pipe on CRTs
     * that share the underlying handle.
     */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetHandleInformation((HANDLE)_get_osfhandle(_fileno(stdout)),
                         HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation((HANDLE)_get_osfhandle(_fileno(stderr)),
                         HANDLE_FLAG_INHERIT, 0);

    g_log_read = read_end;
}

/* ------------------------------------------------------------------ */
/* Download progress window                                            */
/* ------------------------------------------------------------------ */

static void gui_progress_cb(int percent, const char *message, void *user)
{
    HWND hwnd = (HWND)user;
    char *copy;

    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    copy = xstrdup(message ? message : "");
    if (!PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)percent,
                      (LPARAM)copy)) {
        free(copy);
    }
}

static LRESULT CALLBACK progress_proc(HWND hwnd, UINT message,
                                      WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        g_progress = hwnd;
        g_progress_text = CreateWindowExW(
            0, L"STATIC", L"准备下载...",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 20, 440, 40, hwnd, NULL, g_instance, NULL);
        g_progress_bar = CreateWindowExW(
            0, PROGRESS_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            20, 70, 440, 26, hwnd, NULL, g_instance, NULL);
        if (g_progress_text) {
            SendMessageW(g_progress_text, WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        if (g_progress_bar) {
            SendMessageW(g_progress_bar, PBM_SETRANGE32, 0, 100);
            SendMessageW(g_progress_bar, PBM_SETPOS, 0, 0);
        }
        return 0;

    case WM_APP_PROGRESS: {
        char *text = (char *)lparam;
        int percent = (int)wparam;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        if (g_progress_bar) {
            SendMessageW(g_progress_bar, PBM_SETPOS, (WPARAM)percent, 0);
        }
        if (text) {
            wchar_t *wide = utf8_to_wide(text);
            if (wide && g_progress_text) {
                SetWindowTextW(g_progress_text, wide);
            }
            free(wide);
            free(text);
        }
        return 0;
    }

    case WM_TIMER:
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        /* The progress window has no close button; completion closes it. */
        return 0;

    case WM_DESTROY:
        g_progress = NULL;
        g_progress_bar = NULL;
        g_progress_text = NULL;
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wparam, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

static HWND create_progress_window(const char *version_id)
{
    WNDCLASSW wc;
    RECT owner;
    RECT rect;
    wchar_t title[256];
    wchar_t *wide_id;
    int width = 500;
    int height = 170;
    int x;
    int y;
    HWND hwnd;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = progress_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = MCLC_PROGRESS_CLASS;
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }

    wide_id = version_id ? utf8_to_wide(version_id) : NULL;
    swprintf(title, 256, L"下载进度 - %s",
             wide_id ? wide_id : L"Minecraft");
    free(wide_id);

    GetWindowRect(g_hwnd, &owner);
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION, FALSE, 0);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    x = owner.left + ((owner.right - owner.left) - width) / 2;
    y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, MCLC_PROGRESS_CLASS, title,
        WS_OVERLAPPED | WS_CAPTION,
        x, y, width, height, g_hwnd, NULL, g_instance, NULL);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

/* ------------------------------------------------------------------ */
/* Version list                                                        */
/* ------------------------------------------------------------------ */

static void refresh_versions(void)
{
    wchar_t *game_dir = get_game_dir_value();
    wchar_t *versions_dir;
    wchar_t *pattern;
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int count = 0;

    if (!game_dir) {
        set_status(L"游戏目录无效。");
        return;
    }
    versions_dir = path_join(game_dir, L"versions");
    pattern = path_join(versions_dir, L"*");

    SendMessageW(g_version, CB_RESETCONTENT, 0, 0);
    handle = FindFirstFileW(pattern, &data);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *version_dir;
            wchar_t *base;
            wchar_t *json;

            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            version_dir = path_join(versions_dir, data.cFileName);
            base = path_join(version_dir, data.cFileName);
            json = append_json_suffix(base);
            if (path_exists(json)) {
                SendMessageW(g_version, CB_ADDSTRING, 0,
                             (LPARAM)data.cFileName);
                count++;
            }
            free(json);
            free(base);
            free(version_dir);
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }
    free(pattern);
    free(versions_dir);
    free(game_dir);

    gui_logf(L"本地版本扫描完成，共 %d 个。", count);
    if (count > 0) {
        SendMessageW(g_version, CB_SETCURSEL, 0, 0);
        set_status(L"就绪。");
    } else {
        set_status(L"没有本地版本。请先点击“下载游戏”选择并安装版本。");
    }
}

static void select_version_by_id(const char *id)
{
    wchar_t *wide;
    LRESULT index;

    if (!id || !*id) {
        return;
    }
    wide = utf8_to_wide(id);
    if (!wide) {
        return;
    }
    index = SendMessageW(g_version, CB_FINDSTRINGEXACT, (WPARAM)-1,
                         (LPARAM)wide);
    if (index != CB_ERR) {
        SendMessageW(g_version, CB_SETCURSEL, (WPARAM)index, 0);
    }
    free(wide);
}

static char *get_selected_version_id(void)
{
    wchar_t *text = get_control_text(g_version);
    char *id;

    if (!*text) {
        free(text);
        return NULL;
    }
    id = wide_to_utf8(text);
    free(text);
    return id;
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static void free_job(GuiJob *job)
{
    if (!job) {
        return;
    }
    free(job->game_dir);
    free(job->version_id);
    free(job->json_url);
    free(job->json_sha1);
    free(job->username);
    free(job->uuid);
    free(job->java);
    free(job->window_title);
    free(job->custom_info);
    free(job->pre_launch);
    {
        size_t i;
        for (i = 0; i < job->jvm_count; i++) free(job->jvm_args[i]);
        free(job->jvm_args);
        for (i = 0; i < job->game_count; i++) free(job->game_args[i]);
        free(job->game_args);
    }
    free(job);
}

static char *ensure_version_json(const wchar_t *game_dir,
                                 const char *version_id, int verify,
                                 char **error)
{
    wchar_t *local = remote_version_json_path(game_dir, version_id);
    RemoteManifest manifest;
    const RemoteVersion *entry;
    char *resolved = NULL;

    if (!version_id || !path_component_is_safe(version_id)) {
        set_error(error, "版本号无效: %s", version_id ? version_id : "(空)");
        free(local);
        return NULL;
    }
    if (local && path_exists(local) && !verify) {
        free(local);
        return xstrdup(version_id);
    }
    free(local);
    if (remote_manifest_fetch(&manifest, error) != 0) {
        return NULL;
    }
    entry = remote_find(&manifest, version_id);
    if (!entry) {
        set_error(error, "Mojang 版本清单中没有 %s", version_id);
        remote_manifest_free(&manifest);
        return NULL;
    }
    resolved = xstrdup(entry->id);
    if (remote_install_json(game_dir, entry, 0, verify, error) != 0) {
        free(resolved);
        resolved = NULL;
    }
    remote_manifest_free(&manifest);
    return resolved;
}

static int run_pre_launch(const wchar_t *command, const wchar_t *directory,
                          char **error)
{
    wchar_t *mutable_command;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 0;
    BOOL created;

    if (!command || !*command) {
        return 0;
    }
    mutable_command = xwcsdup(command);
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    gui_logf(L"执行启动前命令：%ls", mutable_command);
    created = CreateProcessW(NULL, mutable_command, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, directory,
                             &startup, &process);
    free(mutable_command);
    if (!created) {
        set_error(error, "启动前命令执行失败（Windows 错误 %lu）",
                  (unsigned long)GetLastError());
        return -1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        CloseHandle(process.hProcess);
        set_error(error, "无法读取启动前命令退出代码");
        return -1;
    }
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        set_error(error, "启动前命令退出代码为 %lu",
                  (unsigned long)exit_code);
        return -1;
    }
    gui_logf(L"启动前命令执行完成。");
    return 0;
}

static int job_launch(GuiJob *job, char **error)
{
    Version version;
    LaunchOptions launch;
    wchar_t *data_dir = NULL;
    int rc;

    if (version_load(job->game_dir, job->version_id, &version, error) != 0) {
        char *detail = (error && *error) ? xstrdup(*error) : NULL;
        set_error(error, "请先点击“下载游戏”安装该版本。%s%s",
                  detail ? " " : "", detail ? detail : "");
        free(detail);
        return -1;
    }

    if (g_settings.version_isolation) {
        wchar_t *wide_id = utf8_to_wide(version.id);
        data_dir = wide_id ? path_join3(job->game_dir, L"versions", wide_id)
                           : NULL;
        free(wide_id);
        if (!data_dir) {
            set_error(error, "无法构造版本隔离目录");
            version_free(&version);
            return -1;
        }
        if (mkdir_p(data_dir) != 0) {
            set_error(error, "无法创建版本隔离目录: %ls", data_dir);
            free(data_dir);
            version_free(&version);
            return -1;
        }
        gui_logf(L"版本隔离目录：%ls", data_dir);
    }

    if (run_pre_launch(job->pre_launch, job->game_dir, error) != 0) {
        free(data_dir);
        version_free(&version);
        return -1;
    }

    memset(&launch, 0, sizeof(launch));
    launch.game_dir = job->game_dir;
    launch.game_data_dir = data_dir;
    launch.username = job->username;
    launch.uuid = job->uuid;
    launch.java = job->java;
    launch.memory_mb = job->memory_mb;
    launch.window_title = job->window_title;
    launch.custom_info = job->custom_info;
    launch.demo = job->demo;
    launch.fullscreen = job->fullscreen;
    launch.priority = job->priority;
    launch.extra_jvm = (const char *const *)job->jvm_args;
    launch.extra_jvm_count = job->jvm_count;
    launch.extra_game = (const char *const *)job->game_args;
    launch.extra_game_count = job->game_count;
    if (g_settings.window_mode == 1) {
        launch.width = g_settings.width;
        launch.height = g_settings.height;
    }
    launch.detach = 1;
    launch.gui_mode = 1;
    rc = launch_version(&launch, &version, error);
    version_free(&version);
    free(data_dir);
    return rc < 0 ? -1 : 0;
}

static int job_download(GuiJob *job, char **error)
{
    Version version;
    char *version_id;
    int rc = -1;

    if (job->json_url && *job->json_url) {
        RemoteVersion entry;
        memset(&entry, 0, sizeof(entry));
        entry.id = job->version_id;
        entry.url = job->json_url;
        entry.sha1 = job->json_sha1;
        if (remote_install_json(job->game_dir, &entry, 0, 1, error) != 0) {
            return -1;
        }
        version_id = xstrdup(job->version_id);
    } else {
        version_id = ensure_version_json(job->game_dir, job->version_id, 1,
                                         error);
        if (!version_id) {
            return -1;
        }
    }
    if (version_load(job->game_dir, version_id, &version, error) != 0) {
        free(version_id);
        return -1;
    }
    printf("[*] 下载 %s（Java %d+）\n", version.id, version.java_major);
    /*
     * Download mode always verifies existing files. ensure_file() replaces
     * any size/SHA-1 mismatch and verifies every new download again.
     */
    if (install_version(&version, job->game_dir, 1, error) != 0) {
        version_free(&version);
        free(version_id);
        return -1;
    }
    printf("[完成] %s 已下载并校验。\n", version.id);
    {
        char *copy = xstrdup(version.id);
        char *old = (char *)InterlockedExchangePointer(
            (PVOID volatile *)&g_installed_version, copy);
        free(old);
    }
    version_free(&version);
    free(version_id);
    rc = 0;
    return rc;
}

static unsigned __stdcall job_thread(void *parameter)
{
    GuiJob *job = (GuiJob *)parameter;
    char *error = NULL;
    int rc;

    if (job->kind == JOB_DOWNLOAD) {
        rc = job_download(job, &error);
    } else {
        rc = job_launch(job, &error);
    }

    if (rc < 0) {
        wchar_t *detail = error ? utf8_to_wide(error) : NULL;
        fprintf(stderr, "[错误] %s\n",
                error ? error : "未知错误");
        gui_logf(L"任务失败：%ls", detail ? detail : L"未知错误");
        free(detail);
    } else if (job->kind == JOB_DOWNLOAD) {
        fprintf(stderr, "[完成] 下载与校验完成。\n");
        gui_logf(L"下载与校验完成。");
    } else {
        fprintf(stderr, "[完成] Minecraft 已启动。\n");
        gui_logf(L"Minecraft 已启动。");
    }
    free(error);
    PostMessageW(job->hwnd, WM_APP_JOB_DONE, (WPARAM)job->kind,
                 (LPARAM)(rc >= 0 ? 1 : 0));
    free_job(job);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Controls and layout                                                 */
/* ------------------------------------------------------------------ */

static HFONT make_font(int height, const wchar_t *face)
{
    HFONT font = CreateFontW(
        height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, face);
    return font ? font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static HWND make_control(const wchar_t *class_name, const wchar_t *text,
                         DWORD style, DWORD ex_style, int x, int y,
                         int width, int height, int id)
{
    HWND control = CreateWindowExW(
        ex_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, g_hwnd, (HMENU)(INT_PTR)id, g_instance, NULL);
    if (control) {
        SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    return control;
}

static HWND make_label(const wchar_t *text, int x, int y, int width, int height)
{
    return make_control(L"STATIC", text, SS_LEFT, 0, x, y, width, height, 0);
}

static void set_busy(BOOL busy)
{
    EnableWindow(g_game_dir, !busy);
    EnableWindow(g_browse_game, !busy);
    EnableWindow(g_version, !busy);
    EnableWindow(g_refresh, !busy);
    EnableWindow(g_memory, !busy);
    EnableWindow(g_java, !busy);
    EnableWindow(g_browse_java, !busy);
    EnableWindow(g_demo, !busy);
    EnableWindow(g_launch, !busy);
    EnableWindow(g_user_button, !busy);
    EnableWindow(g_settings_button, !busy);
    EnableWindow(g_download, !busy);
    EnableWindow(g_open_dir, !busy);
}

static void create_controls(HWND hwnd)
{
    wchar_t *default_dir = get_default_game_dir();

    g_hwnd = hwnd;
    g_font = make_font(-15, L"Microsoft YaHei UI");
    g_log_font = make_font(-14, L"Microsoft YaHei UI");

    make_label(L"游戏目录", 16, 21, 78, 22);
    g_game_dir = make_control(L"EDIT", default_dir,
                              WS_BORDER | ES_AUTOHSCROLL,
                              WS_EX_CLIENTEDGE, 100, 18, 540, 26,
                              ID_GAME_DIR);
    g_browse_game = make_control(L"BUTTON", L"浏览...", BS_PUSHBUTTON,
                                 0, 648, 18, 96, 26, ID_BROWSE_GAME);

    make_label(L"版本", 16, 59, 78, 22);
    g_version = make_control(L"COMBOBOX", L"",
                             CBS_DROPDOWN | CBS_AUTOHSCROLL |
                             WS_VSCROLL | WS_TABSTOP,
                             WS_EX_CLIENTEDGE, 100, 54, 540, 240,
                             ID_VERSION);
    g_refresh = make_control(L"BUTTON", L"刷新", BS_PUSHBUTTON,
                             0, 648, 54, 96, 26, ID_REFRESH);

    make_label(L"内存 MB", 16, 97, 78, 22);
    g_memory = make_control(L"EDIT", L"2048",
                            WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                            WS_EX_CLIENTEDGE, 100, 94, 100, 26, ID_MEMORY);
    make_label(L"Java 支持多版本选择", 220, 97, 180, 22);

    make_label(L"Java", 16, 135, 78, 22);
    g_java = make_control(L"COMBOBOX", L"",
                          CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                          WS_EX_CLIENTEDGE, 100, 132, 540, 220, ID_JAVA);
    if (g_java) {
        SendMessageW(g_java, CB_SETDROPPEDWIDTH, 900, 0);
    }
    g_browse_java = make_control(L"BUTTON", L"浏览...", BS_PUSHBUTTON,
                                 0, 648, 132, 96, 26, ID_BROWSE_JAVA);

    g_demo = make_control(L"BUTTON", L"演示模式",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                          104, 170, 100, 24, ID_DEMO);
    make_label(L"下载时会校验已有文件", 240, 170, 240, 24);

    g_launch = make_control(L"BUTTON", L"启动游戏", BS_PUSHBUTTON | WS_TABSTOP,
                            0, 100, 202, 140, 34, ID_LAUNCH);
    g_user_button = make_control(L"BUTTON", L"用户名",
                                 BS_PUSHBUTTON | WS_TABSTOP, 0,
                                 250, 202, 110, 34, ID_USER_BUTTON);
    g_settings_button = make_control(L"BUTTON", L"设置",
                                     BS_PUSHBUTTON | WS_TABSTOP, 0,
                                     370, 202, 100, 34, ID_SETTINGS);
    g_download = make_control(L"BUTTON", L"下载游戏",
                              BS_PUSHBUTTON | WS_TABSTOP, 0,
                              480, 202, 130, 34, ID_DOWNLOAD);
    g_open_dir = make_control(L"BUTTON", L"打开目录",
                              BS_PUSHBUTTON | WS_TABSTOP, 0,
                              620, 202, 124, 34, ID_OPEN_DIR);

    make_label(L"日志", 16, 246, 78, 22);
    g_log = make_control(
        L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        WS_EX_CLIENTEDGE, 16, 268, 728, 290, ID_LOG);
    if (g_log) {
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_log_font, TRUE);
        SendMessageW(g_log, EM_SETLIMITTEXT, MCLC_LOG_LIMIT, 0);
    }
    g_status = make_control(L"STATIC", L"就绪。", SS_LEFT, 0,
                            16, 572, 620, 22, ID_STATUS);
    if (g_status) {
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    g_export_log = make_control(L"BUTTON", L"导出日志",
                                BS_PUSHBUTTON | WS_TABSTOP, 0,
                                648, 570, 96, 28, ID_EXPORT_LOG);

    free(default_dir);
    refresh_java_list();
    refresh_versions();
    set_busy(FALSE);
}

/* ------------------------------------------------------------------ */
/* Dialog helpers                                                      */
/* ------------------------------------------------------------------ */

static void browse_game_dir(void)
{
    BROWSEINFOW browse;
    wchar_t path[MAX_PATH];
    wchar_t display_name[MAX_PATH];
    LPITEMIDLIST pidl;

    gui_logf(L"用户点击浏览游戏目录。");
    memset(&browse, 0, sizeof(browse));
    browse.hwndOwner = g_hwnd;
    browse.pszDisplayName = display_name;
    browse.lpszTitle = L"选择 Minecraft 游戏目录（.minecraft）";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    pidl = SHBrowseForFolderW(&browse);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) {
            gui_logf(L"用户选择游戏目录：%ls", path);
            SetWindowTextW(g_game_dir, path);
            refresh_versions();
        }
        CoTaskMemFree(pidl);
    } else {
        gui_logf(L"用户取消选择游戏目录。");
    }
}

static void browse_java(void)
{
    OPENFILENAMEW dialog;
    wchar_t path[MAX_PATH];

    gui_logf(L"用户点击浏览 Java。");
    path[0] = L'\0';
    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter =
        L"Java (java.exe)\0java.exe\0可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"选择 java.exe";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&dialog)) {
        int entry = add_java_candidate(path);
        gui_logf(L"用户选择 Java：%ls", path);
        if (entry < 0) {
            gui_logf(L"所选文件不是有效的 java.exe：%ls", path);
            MessageBoxW(g_hwnd, L"所选文件不是有效的 java.exe。",
                        L"MCLC", MB_ICONWARNING);
        } else {
            select_java_entry(entry);
        }
    } else {
        gui_logf(L"用户取消选择 Java。");
    }
}

static void open_game_dir(void)
{
    wchar_t *dir = get_game_dir_value();
    HINSTANCE result;

    if (!dir) {
        gui_logf(L"打开游戏目录失败：目录无效。");
        MessageBoxW(g_hwnd, L"游戏目录无效。", L"MCLC", MB_ICONERROR);
        return;
    }
    mkdir_p(dir);
    gui_logf(L"打开游戏目录：%ls", dir);
    result = ShellExecuteW(g_hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        gui_logf(L"打开游戏目录失败，ShellExecute 返回 %lld。",
                 (long long)(INT_PTR)result);
        MessageBoxW(g_hwnd, L"无法打开游戏目录。", L"MCLC", MB_ICONERROR);
    }
    free(dir);
}

static void export_log(void)
{
    OPENFILENAMEW dialog;
    wchar_t path[MAX_PATH];
    wchar_t *snapshot = NULL;
    char *utf8 = NULL;
    SYSTEMTIME now;
    FILE *file;
    int ok = 0;

    gui_logf(L"用户点击导出日志。");
    GetLocalTime(&now);
    swprintf(path, MAX_PATH, L"mclc-log-%04u%02u%02u-%02u%02u%02u.log",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
             (unsigned)now.wHour, (unsigned)now.wMinute,
             (unsigned)now.wSecond);
    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter =
        L"日志文件 (*.log)\0*.log\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"导出日志";
    dialog.lpstrDefExt = L"log";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                   OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&dialog)) {
        gui_logf(L"用户取消导出日志。");
        return;
    }
    {
        size_t path_len = wcslen(path);
        if (path_len < 4 ||
            _wcsicmp(path + path_len - 4, L".log") != 0) {
            if (path_len + 5 < MAX_PATH) {
                wcscat(path, L".log");
            }
        }
    }

    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
        snapshot = xwcsdup(g_log_ui ? g_log_ui : L"");
        LeaveCriticalSection(&g_log_lock);
    }
    utf8 = snapshot ? wide_to_utf8(snapshot) : NULL;
    file = utf8 ? _wfopen(path, L"wb") : NULL;
    if (file) {
        static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        int wrote = fwrite(bom, 1, sizeof(bom), file) == sizeof(bom) &&
                    fwrite(utf8, 1, strlen(utf8), file) == strlen(utf8);
        if (wrote) {
            if (fclose(file) == 0) {
                ok = 1;
            }
        } else {
            fclose(file);
        }
    }
    free(utf8);
    free(snapshot);

    if (ok) {
        gui_logf(L"日志已导出：%ls", path);
        set_status(L"日志已导出。");
    } else {
        gui_logf(L"日志导出失败：%ls", path);
        MessageBoxW(g_hwnd, L"无法写入日志文件。", L"MCLC", MB_ICONERROR);
    }
}

/* ------------------------------------------------------------------ */
/* Start jobs                                                          */
/* ------------------------------------------------------------------ */

static int start_job(GuiJob *job, const wchar_t *status)
{
    HANDLE thread;

    if (!job) {
        return 0;
    }
    {
        char *old = (char *)InterlockedExchangePointer(
            (PVOID volatile *)&g_installed_version, NULL);
        free(old);
    }
    gui_logf(L"后台任务开始：%ls", status);
    InterlockedExchange(&g_running, 1);
    set_busy(TRUE);
    set_status(status);
    thread = (HANDLE)_beginthreadex(NULL, 0, job_thread, job, 0, NULL);
    if (!thread) {
        InterlockedExchange(&g_running, 0);
        set_busy(FALSE);
        set_status(L"无法创建后台线程。");
        gui_logf(L"后台任务启动失败：无法创建线程。");
        free_job(job);
        MessageBoxW(g_hwnd, L"无法创建后台线程。", L"MCLC", MB_ICONERROR);
        return 0;
    }
    CloseHandle(thread);
    return 1;
}

static void free_arg_list(char **items, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static int split_args_utf8(const wchar_t *text, char ***out, size_t *count,
                           char **error)
{
    wchar_t *command;
    LPWSTR *argv;
    int argc = 0;
    size_t i;
    size_t n;
    char **items = NULL;
    int rc = -1;

    *out = NULL;
    *count = 0;
    if (!text || !*text) {
        return 0;
    }
    command = (wchar_t *)xmalloc((wcslen(text) + 8) * sizeof(wchar_t));
    wcscpy(command, L"mclc ");
    wcscat(command, text);
    argv = CommandLineToArgvW(command, &argc);
    free(command);
    if (!argv) {
        set_error(error, "参数解析失败");
        return -1;
    }
    n = argc > 1 ? (size_t)(argc - 1) : 0;
    if (n) {
        items = (char **)xcalloc(n, sizeof(char *));
        for (i = 0; i < n; i++) {
            items[i] = wide_to_utf8(argv[i + 1]);
            if (!items[i]) {
                set_error(error, "参数编码无效");
                goto done;
            }
        }
    }
    *out = items;
    *count = n;
    items = NULL;
    rc = 0;

done:
    LocalFree(argv);
    free_arg_list(items, n);
    return rc;
}

static GuiJob *base_job(int kind)
{
    GuiJob *job = (GuiJob *)xcalloc(1, sizeof(GuiJob));
    Profile *profile;
    wchar_t *memory_text;
    int memory;

    job->hwnd = g_hwnd;
    job->kind = kind;
    job->game_dir = get_game_dir_value();
    if (!job->game_dir) {
        gui_logf(L"任务失败：游戏目录无效。");
        MessageBoxW(g_hwnd, L"游戏目录无效。", L"MCLC", MB_ICONERROR);
        free_job(job);
        return NULL;
    }
    if (mkdir_p(job->game_dir) != 0) {
        gui_logf(L"任务失败：无法创建游戏目录 %ls。", job->game_dir);
        MessageBoxW(g_hwnd, L"无法创建游戏目录。", L"MCLC", MB_ICONERROR);
        free_job(job);
        return NULL;
    }
    gui_logf(L"游戏目录：%ls", job->game_dir);

    if (kind == JOB_DOWNLOAD) {
        job->verify = 1;
        job->username = xstrdup("Steve");
        job->memory_mb = 2048;
        return job;
    }
    job->verify = 0;

    profile = current_profile();
    if (!profile) {
        gui_logf(L"启动失败：没有可用的用户名。");
        MessageBoxW(g_hwnd, L"请先在“用户名”中创建一个用户名。",
                    L"MCLC", MB_ICONWARNING);
        free_job(job);
        return NULL;
    }
    job->username = wide_to_utf8(profile->name);
    job->uuid = wide_to_utf8(profile->uuid);
    if (!job->username || !job->uuid) {
        gui_logf(L"任务失败：用户名或 UUID 编码无效。");
        MessageBoxW(g_hwnd, L"用户名或 UUID 编码无效。", L"MCLC",
                    MB_ICONERROR);
        free_job(job);
        return NULL;
    }

    memory_text = get_control_text(g_memory);
    memory = _wtoi(memory_text);
    free(memory_text);
    if (memory < 256 || memory > 65536) {
        gui_logf(L"任务失败：内存设置无效（%d MB）。", memory);
        MessageBoxW(g_hwnd, L"内存必须是 256 到 65536 之间的 MB 数。",
                    L"MCLC", MB_ICONERROR);
        free_job(job);
        return NULL;
    }
    job->memory_mb = memory;

    job->java = get_selected_java_path();
    job->demo = (IsDlgButtonChecked(g_hwnd, ID_DEMO) == BST_CHECKED);
    job->window_title = xwcsdup(g_settings.window_title);
    job->custom_info = xwcsdup(g_settings.custom_info);
    job->pre_launch = xwcsdup(g_settings.pre_launch);
    job->fullscreen = g_settings.window_mode == 2;
    job->priority = g_settings.priority;
    if (split_args_utf8(g_settings.jvm_args, &job->jvm_args,
                        &job->jvm_count, NULL) != 0 ||
        split_args_utf8(g_settings.game_args, &job->game_args,
                        &job->game_count, NULL) != 0) {
        gui_logf(L"启动失败：JVM/游戏参数解析失败。");
        MessageBoxW(g_hwnd, L"JVM 或游戏参数格式无效。", L"MCLC",
                    MB_ICONERROR);
        free_job(job);
        return NULL;
    }
    return job;
}

static void start_launch(void)
{
    GuiJob *job;
    char *id;

    if (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
        return;
    }

    job = base_job(JOB_LAUNCH);
    if (!job) {
        return;
    }
    id = get_selected_version_id();
    if (!id) {
        gui_logf(L"启动失败：未选择版本。");
        MessageBoxW(g_hwnd, L"请选择或输入版本号。", L"MCLC", MB_ICONWARNING);
        free_job(job);
        return;
    }
    job->version_id = id;
    {
        wchar_t *wide_id = utf8_to_wide(id);
        wchar_t *wide_user = job->username
                                 ? utf8_to_wide(job->username) : NULL;
        wchar_t *wide_uuid = job->uuid
                                 ? utf8_to_wide(job->uuid) : NULL;
        gui_logf(L"用户点击启动游戏：版本=%ls，用户=%ls，UUID=%ls，"
                 L"内存=%d MB，Java=%ls，演示=%ls，目录=%ls",
                 wide_id ? wide_id : L"(未知)",
                 wide_user ? wide_user : L"Steve",
                 wide_uuid ? wide_uuid : L"(自动)", job->memory_mb,
                 job->java ? job->java : L"自动查找",
                 job->demo ? L"是" : L"否", job->game_dir);
        free(wide_uuid);
        free(wide_user);
        free(wide_id);
    }
    start_job(job, L"正在启动游戏...");
}

static void start_download(const char *version_id, const char *json_url,
                           const char *json_sha1)
{
    GuiJob *job;

    if (!version_id || !*version_id ||
        InterlockedCompareExchange(&g_running, 0, 0) != 0) {
        return;
    }
    job = base_job(JOB_DOWNLOAD);
    if (!job) {
        return;
    }
    job->version_id = xstrdup(version_id);
    job->json_url = (json_url && *json_url) ? xstrdup(json_url) : NULL;
    job->json_sha1 = (json_sha1 && *json_sha1) ? xstrdup(json_sha1) : NULL;
    {
        wchar_t *wide_id = utf8_to_wide(version_id);
        gui_logf(L"用户确认下载游戏：版本=%ls，目录=%ls",
                 wide_id ? wide_id : L"(未知)", job->game_dir);
        free(wide_id);
    }
    if (!create_progress_window(version_id)) {
        gui_logf(L"下载失败：无法创建进度窗口。");
        MessageBoxW(g_hwnd, L"无法创建下载进度窗口。", L"MCLC", MB_ICONERROR);
        free_job(job);
        return;
    }
    mclc_progress_set(gui_progress_cb, g_progress);
    if (!start_job(job, L"正在下载并校验游戏文件...")) {
        mclc_progress_clear();
        if (g_progress) {
            DestroyWindow(g_progress);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Download version picker                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND hwnd;
    HWND category;
    HWND list;
    HWND status;
    HWND confirm;
    RemoteManifest manifest;
    int manifest_ready;
    char *selected;
    char *selected_url;
    char *selected_sha1;
} PickerState;

typedef struct {
    RemoteManifest manifest;
    char *error;
    int ok;
} PickerLoadResult;

static int is_april_fools_version(const char *id)
{
    static const char *known[] = {
        "2.0",
        "1.RV-Pre1",
        "15w14a",
        "3D Shareware v1.34",
        "20w14infinite",
        "22w13oneBlockAtATime",
        "23w13a_or_b",
        "24w14potato",
        "25w14craftmine",
        NULL
    };
    int i;

    if (!id) {
        return 0;
    }
    for (i = 0; known[i]; i++) {
        if (_stricmp(id, known[i]) == 0) {
            return 1;
        }
    }
    if (strstr(id, "infinite") || strstr(id, "potato") ||
        strstr(id, "craftmine") || strstr(id, "oneblock") ||
        strstr(id, "oneBlock") || strstr(id, "or_b") ||
        strstr(id, "Shareware")) {
        return 1;
    }
    return 0;
}

static int picker_category_matches(const RemoteVersion *version, int category)
{
    if (!version || !version->type) {
        return 0;
    }
    if (category == 0) {
        return strcmp(version->type, "release") == 0 &&
               !is_april_fools_version(version->id);
    }
    if (category == 1) {
        return strcmp(version->type, "snapshot") == 0 &&
               !is_april_fools_version(version->id);
    }
    if (category == 2) {
        return strcmp(version->type, "old_beta") == 0 ||
               strcmp(version->type, "old_alpha") == 0;
    }
    if (category == 3) {
        return is_april_fools_version(version->id);
    }
    return 0;
}

static void picker_fill_versions(PickerState *state)
{
    int category;
    int count = 0;
    size_t i;
    wchar_t status[128];

    if (!state || !state->manifest_ready) {
        return;
    }
    category = (int)SendMessageW(state->category, CB_GETCURSEL, 0, 0);
    if (category == CB_ERR) {
        category = 0;
    }
    SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < state->manifest.count; i++) {
        const RemoteVersion *version = &state->manifest.versions[i];
        if (picker_category_matches(version, category)) {
            wchar_t *wide_id = utf8_to_wide(version->id);
            if (wide_id) {
                LRESULT index = SendMessageW(state->list, LB_ADDSTRING, 0,
                                              (LPARAM)wide_id);
                free(wide_id);
                if (index != LB_ERR) {
                    SendMessageW(state->list, LB_SETITEMDATA,
                                 (WPARAM)index, (LPARAM)i);
                    count++;
                }
            }
        }
    }
    if (count > 0) {
        SendMessageW(state->list, LB_SETCURSEL, 0, 0);
        EnableWindow(state->confirm, TRUE);
    } else {
        EnableWindow(state->confirm, FALSE);
    }
    swprintf(status, 128, L"共 %d 个版本", count);
    SetWindowTextW(state->status, status);
}

static void picker_confirm(PickerState *state)
{
    LRESULT selection;
    LRESULT item;
    if (!state || !state->manifest_ready) {
        return;
    }
    selection = SendMessageW(state->list, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) {
        return;
    }
    item = SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)selection, 0);
    if (item == LB_ERR || item < 0 ||
        (size_t)item >= state->manifest.count) {
        return;
    }
    free(state->selected);
    free(state->selected_url);
    free(state->selected_sha1);
    state->selected = xstrdup(state->manifest.versions[item].id);
    state->selected_url = state->manifest.versions[item].url
                              ? xstrdup(state->manifest.versions[item].url)
                              : NULL;
    state->selected_sha1 = state->manifest.versions[item].sha1
                               ? xstrdup(state->manifest.versions[item].sha1)
                               : NULL;
    {
        wchar_t *wide_id = state->selected
                               ? utf8_to_wide(state->selected) : NULL;
        gui_logf(L"用户确认下载版本：%ls",
                 wide_id ? wide_id : L"(未知)");
        free(wide_id);
    }
    DestroyWindow(state->hwnd);
}

static unsigned __stdcall picker_load_thread(void *argument)
{
    HWND hwnd = (HWND)argument;
    PickerLoadResult *result = (PickerLoadResult *)xcalloc(
        1, sizeof(PickerLoadResult));

    if (remote_manifest_fetch(&result->manifest, &result->error) == 0) {
        result->ok = 1;
    } else {
        remote_manifest_free(&result->manifest);
    }
    if (!PostMessageW(hwnd, WM_APP_PICKER_LOADED, 0, (LPARAM)result)) {
        remote_manifest_free(&result->manifest);
        free(result->error);
        free(result);
    }
    InterlockedExchange(&g_picker_loading, 0);
    return 0;
}

static LRESULT CALLBACK picker_proc(HWND hwnd, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    PickerState *state = (PickerState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        state = (PickerState *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        state->hwnd = hwnd;

        CreateWindowExW(0, L"STATIC", L"版本类型",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 22, 78, 22, hwnd, NULL, g_instance, NULL);
        state->category = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL |
            WS_TABSTOP, 100, 18, 190, 200, hwnd,
            (HMENU)(INT_PTR)ID_PICK_CATEGORY, g_instance, NULL);
        state->list = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
            LBS_NOINTEGRALHEIGHT, 16, 55, 488, 285, hwnd,
            (HMENU)(INT_PTR)ID_PICK_LIST, g_instance, NULL);
        state->status = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 352, 488, 22, hwnd, NULL, g_instance, NULL);
        state->confirm = CreateWindowExW(
            0, L"BUTTON", L"下载",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            410, 385, 94, 32, hwnd,
            (HMENU)(INT_PTR)ID_PICK_CONFIRM, g_instance, NULL);

        SendMessageW(state->category, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->list, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->status, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->confirm, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->category, CB_ADDSTRING, 0, (LPARAM)L"正式版");
        SendMessageW(state->category, CB_ADDSTRING, 0, (LPARAM)L"预览版");
        SendMessageW(state->category, CB_ADDSTRING, 0, (LPARAM)L"远古版");
        SendMessageW(state->category, CB_ADDSTRING, 0, (LPARAM)L"愚人节版");
        SendMessageW(state->category, CB_SETCURSEL, 0, 0);
        SetWindowTextW(state->status, L"正在获取版本清单...");
        EnableWindow(state->category, FALSE);
        EnableWindow(state->confirm, FALSE);
        return 0;
    }

    case WM_APP_PICKER_LOADED: {
        PickerLoadResult *result = (PickerLoadResult *)lparam;
        if (!result) {
            return 0;
        }
        if (!state || !result->ok) {
            if (state) {
                wchar_t *detail = result->error
                                      ? utf8_to_wide(result->error) : NULL;
                wchar_t message[512];
                swprintf(message, 512, L"获取版本清单失败。%s%s",
                         detail ? L"\r\n" : L"",
                         detail ? detail : L"");
                SetWindowTextW(state->status, message);
                gui_logf(L"版本清单加载失败：%ls",
                         detail ? detail : L"未知错误");
                set_status(L"获取版本清单失败。");
                free(detail);
            }
            remote_manifest_free(&result->manifest);
            free(result->error);
            free(result);
            return 0;
        }
        remote_manifest_free(&state->manifest);
        state->manifest = result->manifest;
        memset(&result->manifest, 0, sizeof(result->manifest));
        state->manifest_ready = 1;
        EnableWindow(state->category, TRUE);
        picker_fill_versions(state);
        gui_logf(L"版本清单加载完成，共 %zu 个版本。",
                 state->manifest.count);
        set_status(L"请选择要下载的版本。");
        SetFocus(state->category);
        free(result->error);
        free(result);
        return 0;
    }

    case WM_COMMAND:
        if (!state) {
            break;
        }
        if (LOWORD(wparam) == ID_PICK_CATEGORY &&
            HIWORD(wparam) == CBN_SELCHANGE) {
            wchar_t *category = get_control_text(state->category);
            gui_logf(L"用户切换版本类型：%ls",
                     category && *category ? category : L"(未知)");
            free(category);
            picker_fill_versions(state);
            return 0;
        }
        if (LOWORD(wparam) == ID_PICK_LIST &&
            HIWORD(wparam) == LBN_DBLCLK) {
            picker_confirm(state);
            return 0;
        }
        if (LOWORD(wparam) == IDOK) {
            picker_confirm(state);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wparam) == ID_PICK_CONFIRM &&
            HIWORD(wparam) == BN_CLICKED) {
            picker_confirm(state);
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wparam, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void open_download_picker(void)
{
    PickerState state;
    WNDCLASSW wc;
    RECT owner;
    RECT rect;
    int width = 520;
    int height = 430;
    int x;
    int y;
    int quit = 0;
    MSG msg;
    HANDLE thread;

    if (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
        return;
    }
    gui_logf(L"用户点击下载游戏。");
    if (InterlockedCompareExchange(&g_picker_loading, 1, 0) != 0) {
        gui_logf(L"版本清单仍在加载，已忽略本次请求。");
        set_status(L"版本清单正在加载，请稍候。");
        return;
    }

    memset(&state, 0, sizeof(state));
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = picker_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = MCLC_PICKER_CLASS;
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        InterlockedExchange(&g_picker_loading, 0);
        MessageBoxW(g_hwnd, L"无法创建下载版本窗口。", L"MCLC",
                    MB_ICONERROR);
        return;
    }

    GetWindowRect(g_hwnd, &owner);
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, 0);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    x = owner.left + ((owner.right - owner.left) - width) / 2;
    y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    EnableWindow(g_hwnd, FALSE);
    set_status(L"正在获取版本清单...");
    UpdateWindow(g_hwnd);
    state.hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        MCLC_PICKER_CLASS, L"下载游戏",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, g_hwnd, NULL, g_instance, &state);
    if (!state.hwnd) {
        InterlockedExchange(&g_picker_loading, 0);
        EnableWindow(g_hwnd, TRUE);
        SetForegroundWindow(g_hwnd);
        set_status(L"无法创建下载版本窗口。");
        MessageBoxW(g_hwnd, L"无法创建下载版本窗口。", L"MCLC",
                    MB_ICONERROR);
        return;
    }
    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);

    thread = (HANDLE)_beginthreadex(
        NULL, 0, picker_load_thread, (void *)(uintptr_t)state.hwnd, 0, NULL);
    if (!thread) {
        InterlockedExchange(&g_picker_loading, 0);
        gui_logf(L"无法启动版本清单加载线程。");
        SetWindowTextW(state.status, L"无法启动版本清单加载线程。");
        set_status(L"无法获取版本清单。");
    } else {
        gui_logf(L"版本选择窗口已打开，正在后台加载版本清单。");
        CloseHandle(thread);
    }

    while (IsWindow(state.hwnd)) {
        int result = GetMessageW(&msg, NULL, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                PostQuitMessage((int)msg.wParam);
                quit = 1;
            }
            break;
        }
        if (!IsDialogMessageW(state.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    while (PeekMessageW(&msg, NULL, WM_APP_PICKER_LOADED,
                        WM_APP_PICKER_LOADED, PM_REMOVE)) {
        PickerLoadResult *result = (PickerLoadResult *)msg.lParam;
        if (result) {
            remote_manifest_free(&result->manifest);
            free(result->error);
            free(result);
        }
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);

    if (!quit && state.selected && *state.selected) {
        start_download(state.selected, state.selected_url,
                       state.selected_sha1);
    } else if (!quit) {
        gui_logf(L"用户取消下载游戏。");
        set_status(L"已取消下载。");
    }
    remote_manifest_free(&state.manifest);
    free(state.selected);
    free(state.selected_url);
    free(state.selected_sha1);
}

/* ------------------------------------------------------------------ */
/* User profile window                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND hwnd;
    HWND combo;
} UserState;

typedef struct {
    HWND hwnd;
    HWND name;
    HWND uuid;
    int saved;
} NewUserState;

static wchar_t *trim_wide_copy(const wchar_t *text)
{
    const wchar_t *start;
    const wchar_t *end;
    size_t len;
    wchar_t *out;

    if (!text) {
        return xwcsdup(L"");
    }
    start = text;
    while (*start == L' ' || *start == L'\t' ||
           *start == L'\r' || *start == L'\n') {
        start++;
    }
    end = start + wcslen(start);
    while (end > start && (end[-1] == L' ' || end[-1] == L'\t' ||
                           end[-1] == L'\r' || end[-1] == L'\n')) {
        end--;
    }
    len = (size_t)(end - start);
    out = (wchar_t *)xmalloc((len + 1) * sizeof(wchar_t));
    memcpy(out, start, len * sizeof(wchar_t));
    out[len] = L'\0';
    return out;
}

static wchar_t *get_trimmed_control_text(HWND control)
{
    wchar_t *text = get_control_text(control);
    wchar_t *trimmed = trim_wide_copy(text);
    free(text);
    return trimmed;
}

static void set_setting_from_control(wchar_t **target, HWND control)
{
    wchar_t *text = get_control_text(control);
    replace_wide(target, text);
    free(text);
}

static int uuid_is_valid(const wchar_t *uuid)
{
    int hex = 0;
    const wchar_t *p;
    for (p = uuid; *p; p++) {
        if ((*p >= L'0' && *p <= L'9') ||
            (*p >= L'a' && *p <= L'f') ||
            (*p >= L'A' && *p <= L'F')) {
            hex++;
        } else if (*p != L'-') {
            return 0;
        }
    }
    return hex == 32;
}

static void fill_profile_combo(UserState *state, size_t select)
{
    size_t i;
    SendMessageW(state->combo, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_profile_count; i++) {
        size_t cap = wcslen(g_profiles[i].name) +
                     wcslen(g_profiles[i].uuid) + 8;
        wchar_t *label = (wchar_t *)xmalloc(cap * sizeof(wchar_t));
        swprintf(label, cap, L"%ls (%ls)", g_profiles[i].name,
                 g_profiles[i].uuid);
        SendMessageW(state->combo, CB_ADDSTRING, 0, (LPARAM)label);
        free(label);
    }
    if (select >= g_profile_count) {
        select = 0;
    }
    SendMessageW(state->combo, CB_SETCURSEL, (WPARAM)select, 0);
}

static LRESULT CALLBACK new_user_proc(HWND hwnd, UINT message,
                                      WPARAM wparam, LPARAM lparam)
{
    NewUserState *state = (NewUserState *)GetWindowLongPtrW(
        hwnd, GWLP_USERDATA);

    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        state = (NewUserState *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        state->hwnd = hwnd;
        CreateWindowExW(0, L"STATIC", L"用户名",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 22, 80, 22, hwnd, NULL, g_instance, NULL);
        state->name = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            100, 18, 320, 26, hwnd,
            (HMENU)(INT_PTR)ID_NEW_NAME, g_instance, NULL);
        CreateWindowExW(0, L"STATIC", L"UUID（留空自动生成）",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 62, 80, 40, hwnd, NULL, g_instance, NULL);
        state->uuid = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            100, 58, 320, 26, hwnd,
            (HMENU)(INT_PTR)ID_NEW_UUID, g_instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"保存",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                        BS_DEFPUSHBUTTON,
                        230, 110, 90, 30, hwnd,
                        (HMENU)(INT_PTR)ID_NEW_SAVE, g_instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        330, 110, 90, 30, hwnd,
                        (HMENU)(INT_PTR)ID_NEW_CANCEL, g_instance, NULL);
        SendMessageW(state->name, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->uuid, WM_SETFONT, (WPARAM)g_font, TRUE);
        SetFocus(state->name);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wparam) == ID_NEW_SAVE ||
            LOWORD(wparam) == IDOK) {
            wchar_t *name = get_trimmed_control_text(state->name);
            wchar_t *uuid = get_trimmed_control_text(state->uuid);
            char *name_utf8;
            wchar_t *final_uuid = NULL;
            char generated[37];
            if (!*name) {
                MessageBoxW(hwnd, L"用户名不能为空。", L"MCLC",
                            MB_ICONWARNING);
            } else if (g_profile_count >= MCLC_MAX_PROFILES) {
                MessageBoxW(hwnd, L"用户名数量已达上限。", L"MCLC",
                            MB_ICONWARNING);
            } else {
                name_utf8 = wide_to_utf8(name);
                if (*uuid) {
                    if (!uuid_is_valid(uuid)) {
                        MessageBoxW(hwnd,
                                    L"UUID 格式无效，应包含 32 个十六进制字符。",
                                    L"MCLC", MB_ICONWARNING);
                        free(name);
                        free(uuid);
                        free(name_utf8);
                        return 0;
                    }
                    final_uuid = xwcsdup(uuid);
                } else {
                    if (offline_uuid_for_name(
                            name_utf8 ? name_utf8 : "Player",
                            generated) != 0) {
                        strcpy(generated,
                               "00000000-0000-3000-8000-000000000000");
                    }
                    final_uuid = utf8_to_wide(generated);
                }
                if (!name_utf8 || !final_uuid) {
                    MessageBoxW(hwnd, L"用户名或 UUID 编码无效。", L"MCLC",
                                MB_ICONERROR);
                } else {
                    g_profiles = (Profile *)xrealloc(
                        g_profiles,
                        (g_profile_count + 1) * sizeof(Profile));
                    g_profiles[g_profile_count].name = name;
                    g_profiles[g_profile_count].uuid = final_uuid;
                    g_profile_count++;
                    g_profile_current = g_profile_count - 1;
                    name = NULL;
                    final_uuid = NULL;
                    save_config();
                    state->saved = 1;
                    gui_logf(L"新增用户名：%ls", g_profiles[g_profile_current].name);
                    DestroyWindow(hwnd);
                }
                free(name_utf8);
            }
            free(name);
            free(uuid);
            free(final_uuid);
            return 0;
        }
        if (LOWORD(wparam) == ID_NEW_CANCEL ||
            LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void open_new_user(UserState *owner)
{
    WNDCLASSW wc;
    NewUserState state;
    RECT owner_rect;
    RECT rect;
    MSG msg;
    int quit = 0;

    memset(&state, 0, sizeof(state));
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = new_user_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = MCLC_NEWUSER_CLASS;
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }
    GetWindowRect(owner->hwnd, &owner_rect);
    rect.left = 0;
    rect.top = 0;
    rect.right = 450;
    rect.bottom = 170;
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, 0);
    EnableWindow(owner->hwnd, FALSE);
    state.hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        MCLC_NEWUSER_CLASS, L"新建用户名",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        owner_rect.left + 30, owner_rect.top + 40,
        rect.right - rect.left, rect.bottom - rect.top,
        owner->hwnd, NULL, g_instance, &state);
    if (!state.hwnd) {
        EnableWindow(owner->hwnd, TRUE);
        return;
    }
    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);
    while (IsWindow(state.hwnd)) {
        int result = GetMessageW(&msg, NULL, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                PostQuitMessage((int)msg.wParam);
                quit = 1;
            }
            break;
        }
        if (!IsDialogMessageW(state.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner->hwnd, TRUE);
    SetForegroundWindow(owner->hwnd);
    if (!quit && state.saved) {
        fill_profile_combo(owner, g_profile_current);
        gui_logf(L"用户名已保存到 mclc_config.ini。");
    }
}

static LRESULT CALLBACK user_proc(HWND hwnd, UINT message,
                                  WPARAM wparam, LPARAM lparam)
{
    UserState *state = (UserState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        state = (UserState *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        state->hwnd = hwnd;
        CreateWindowExW(0, L"STATIC", L"选择用户名和 UUID",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 20, 418, 22, hwnd, NULL, g_instance, NULL);
        state->combo = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL |
            WS_TABSTOP, 16, 48, 418, 220, hwnd,
            (HMENU)(INT_PTR)ID_PROFILE_COMBO, g_instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"新建",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        16, 92, 90, 30, hwnd,
                        (HMENU)(INT_PTR)ID_PROFILE_NEW, g_instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"确定",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                        BS_DEFPUSHBUTTON,
                        240, 92, 90, 30, hwnd,
                        (HMENU)(INT_PTR)ID_PROFILE_OK, g_instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        340, 92, 90, 30, hwnd,
                        (HMENU)(INT_PTR)ID_PROFILE_CANCEL, g_instance, NULL);
        SendMessageW(state->combo, WM_SETFONT, (WPARAM)g_font, TRUE);
        fill_profile_combo(state, g_profile_current);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wparam) == ID_PROFILE_NEW) {
            open_new_user(state);
            return 0;
        }
        if (LOWORD(wparam) == ID_PROFILE_OK ||
            LOWORD(wparam) == IDOK) {
            LRESULT sel = SendMessageW(state->combo, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR && (size_t)sel < g_profile_count) {
                g_profile_current = (size_t)sel;
                save_config();
                gui_logf(L"当前用户名：%ls",
                         g_profiles[g_profile_current].name);
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (LOWORD(wparam) == ID_PROFILE_CANCEL ||
            LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void open_user_window(void)
{
    WNDCLASSW wc;
    gui_logf(L"用户点击用户名设置。");
    UserState state;
    RECT owner;
    RECT rect;
    MSG msg;
    int quit = 0;

    memset(&state, 0, sizeof(state));
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = user_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = MCLC_USER_CLASS;
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }
    GetWindowRect(g_hwnd, &owner);
    rect.left = 0;
    rect.top = 0;
    rect.right = 450;
    rect.bottom = 150;
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, 0);
    EnableWindow(g_hwnd, FALSE);
    state.hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        MCLC_USER_CLASS, L"用户名",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        owner.left + 40, owner.top + 40,
        rect.right - rect.left, rect.bottom - rect.top,
        g_hwnd, NULL, g_instance, &state);
    if (!state.hwnd) {
        EnableWindow(g_hwnd, TRUE);
        return;
    }
    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);
    while (IsWindow(state.hwnd)) {
        int result = GetMessageW(&msg, NULL, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                PostQuitMessage((int)msg.wParam);
                quit = 1;
            }
            break;
        }
        if (!IsDialogMessageW(state.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    if (quit) {
        return;
    }
    set_status(L"用户名设置已更新。");
}

/* ------------------------------------------------------------------ */
/* Settings window                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND hwnd;
    HWND isolation;
    HWND title;
    HWND info;
    HWND priority;
    HWND mode;
    HWND width;
    HWND height;
    HWND jvm;
    HWND game;
    HWND pre;
} SettingsState;

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT message,
                                      WPARAM wparam, LPARAM lparam)
{
    SettingsState *state = (SettingsState *)GetWindowLongPtrW(
        hwnd, GWLP_USERDATA);

    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        state = (SettingsState *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        state->hwnd = hwnd;
        state->isolation = CreateWindowExW(
            0, L"BUTTON", L"开启版本隔离（保存到 versions/<版本>/）",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            16, 16, 420, 24, hwnd,
            (HMENU)(INT_PTR)ID_SET_ISOLATION, g_instance, NULL);
        CreateWindowExW(0, L"STATIC", L"游戏窗口标题",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 52, 120, 22, hwnd, NULL, g_instance, NULL);
        state->title = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            150, 48, 380, 26, hwnd,
            (HMENU)(INT_PTR)ID_SET_TITLE, g_instance, NULL);
        CreateWindowExW(0, L"STATIC", L"自定义信息",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 84, 120, 22, hwnd, NULL, g_instance, NULL);
        state->info = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            150, 80, 380, 26, hwnd,
            (HMENU)(INT_PTR)ID_SET_INFO, g_instance, NULL);
        CreateWindowExW(0, L"STATIC", L"进程优先级",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 116, 120, 22, hwnd, NULL, g_instance, NULL);
        state->priority = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            150, 112, 120, 160, hwnd,
            (HMENU)(INT_PTR)ID_SET_PRIORITY, g_instance, NULL);
        SendMessageW(state->priority, CB_ADDSTRING, 0, (LPARAM)L"低");
        SendMessageW(state->priority, CB_ADDSTRING, 0, (LPARAM)L"中");
        SendMessageW(state->priority, CB_ADDSTRING, 0, (LPARAM)L"高");
        SendMessageW(state->priority, CB_SETCURSEL,
                     (WPARAM)g_settings.priority, 0);

        CreateWindowExW(0, L"STATIC", L"窗口大小",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        300, 116, 70, 22, hwnd, NULL, g_instance, NULL);
        state->mode = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            370, 112, 100, 160, hwnd,
            (HMENU)(INT_PTR)ID_SET_MODE, g_instance, NULL);
        SendMessageW(state->mode, CB_ADDSTRING, 0, (LPARAM)L"默认");
        SendMessageW(state->mode, CB_ADDSTRING, 0, (LPARAM)L"自定义");
        SendMessageW(state->mode, CB_ADDSTRING, 0, (LPARAM)L"全屏");
        SendMessageW(state->mode, CB_SETCURSEL,
                     (WPARAM)g_settings.window_mode, 0);

        CreateWindowExW(0, L"STATIC", L"宽",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 148, 30, 22, hwnd, NULL, g_instance, NULL);
        state->width = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            50, 144, 70, 26, hwnd,
            (HMENU)(INT_PTR)ID_SET_WIDTH, g_instance, NULL);
        CreateWindowExW(0, L"STATIC", L"高",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        140, 148, 30, 22, hwnd, NULL, g_instance, NULL);
        state->height = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            174, 144, 70, 26, hwnd,
            (HMENU)(INT_PTR)ID_SET_HEIGHT, g_instance, NULL);

        CreateWindowExW(0, L"STATIC", L"JVM 参数",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 182, 120, 22, hwnd, NULL, g_instance, NULL);
        state->jvm = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | WS_VSCROLL,
            150, 178, 380, 64, hwnd,
            (HMENU)(INT_PTR)ID_SET_JVM, g_instance, NULL);
        CreateWindowExW(0, L"STATIC", L"游戏参数",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 252, 120, 22, hwnd, NULL, g_instance, NULL);
        state->game = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | WS_VSCROLL,
            150, 248, 380, 64, hwnd,
            (HMENU)(INT_PTR)ID_SET_GAME, g_instance, NULL);
        CreateWindowExW(0, L"STATIC", L"启动前命令",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 322, 120, 22, hwnd, NULL, g_instance, NULL);
        state->pre = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            150, 318, 380, 26, hwnd,
            (HMENU)(INT_PTR)ID_SET_PRE, g_instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"保存",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                        BS_DEFPUSHBUTTON,
                        340, 370, 90, 30, hwnd,
                        (HMENU)(INT_PTR)ID_SET_OK, g_instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        440, 370, 90, 30, hwnd,
                        (HMENU)(INT_PTR)ID_SET_CANCEL, g_instance, NULL);

        SendMessageW(state->isolation, BM_SETCHECK,
                     g_settings.version_isolation ? BST_CHECKED
                                                  : BST_UNCHECKED, 0);
        SetWindowTextW(state->title, g_settings.window_title);
        SetWindowTextW(state->info, g_settings.custom_info);
        SetWindowTextW(state->jvm, g_settings.jvm_args);
        SetWindowTextW(state->game, g_settings.game_args);
        SetWindowTextW(state->pre, g_settings.pre_launch);
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"%d", g_settings.width);
            SetWindowTextW(state->width, buf);
            swprintf(buf, 32, L"%d", g_settings.height);
            SetWindowTextW(state->height, buf);
        }
        SendMessageW(state->isolation, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->title, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->info, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->priority, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->mode, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->width, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->height, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->jvm, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->game, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(state->pre, WM_SETFONT, (WPARAM)g_font, TRUE);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wparam) == ID_SET_OK ||
            LOWORD(wparam) == IDOK) {
            int mode = (int)SendMessageW(state->mode, CB_GETCURSEL, 0, 0);
            int priority = (int)SendMessageW(state->priority,
                                             CB_GETCURSEL, 0, 0);
            wchar_t *width = get_control_text(state->width);
            wchar_t *height = get_control_text(state->height);
            int w = _wtoi(width);
            int h = _wtoi(height);
            free(width);
            free(height);
            if (mode == 1 && (w < 320 || w > 16384 ||
                              h < 240 || h > 16384)) {
                MessageBoxW(hwnd,
                            L"自定义窗口大小必须是 320×240 到 16384×16384。",
                            L"MCLC", MB_ICONWARNING);
                return 0;
            }
            g_settings.version_isolation =
                IsDlgButtonChecked(hwnd, ID_SET_ISOLATION) == BST_CHECKED;
            g_settings.priority = priority >= 0 && priority <= 2
                                      ? priority : 1;
            g_settings.window_mode = mode >= 0 && mode <= 2 ? mode : 0;
            if (mode == 1) {
                g_settings.width = w;
                g_settings.height = h;
            }
            set_setting_from_control(&g_settings.window_title, state->title);
            set_setting_from_control(&g_settings.custom_info, state->info);
            set_setting_from_control(&g_settings.jvm_args, state->jvm);
            set_setting_from_control(&g_settings.game_args, state->game);
            set_setting_from_control(&g_settings.pre_launch, state->pre);
            save_config();
            gui_logf(L"设置已保存。");
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wparam) == ID_SET_CANCEL ||
            LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void open_settings_window(void)
{
    WNDCLASSW wc;
    gui_logf(L"用户点击设置。");
    SettingsState state;
    RECT owner;
    RECT rect;
    MSG msg;
    int quit = 0;

    memset(&state, 0, sizeof(state));
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = settings_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = MCLC_SETTINGS_CLASS;
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }
    GetWindowRect(g_hwnd, &owner);
    rect.left = 0;
    rect.top = 0;
    rect.right = 560;
    rect.bottom = 430;
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, 0);
    EnableWindow(g_hwnd, FALSE);
    state.hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        MCLC_SETTINGS_CLASS, L"设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        owner.left + 40, owner.top + 20,
        rect.right - rect.left, rect.bottom - rect.top,
        g_hwnd, NULL, g_instance, &state);
    if (!state.hwnd) {
        EnableWindow(g_hwnd, TRUE);
        return;
    }
    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);
    while (IsWindow(state.hwnd)) {
        int result = GetMessageW(&msg, NULL, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                PostQuitMessage((int)msg.wParam);
                quit = 1;
            }
            break;
        }
        if (!IsDialogMessageW(state.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    if (!quit) {
        set_status(L"设置已更新。");
    }
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        create_controls(hwnd);
        if (g_log_read) {
            uintptr_t reader = _beginthreadex(NULL, 0, log_reader_thread,
                                              NULL, 0, NULL);
            if (reader) {
                CloseHandle((HANDLE)reader);
            }
        }
        refresh_log_view();
        gui_logf(L"MCLC GUI 已就绪。");
        SetTimer(hwnd, 1, 200, NULL);
        return 0;

    case WM_TIMER:
        if (wparam == 1 && g_log_dirty) {
            refresh_log_view();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_LAUNCH:
            start_launch();
            break;
        case ID_DOWNLOAD:
            open_download_picker();
            break;
        case ID_REFRESH:
            gui_logf(L"用户点击刷新本地版本列表。");
            refresh_versions();
            break;
        case ID_BROWSE_GAME:
            browse_game_dir();
            break;
        case ID_BROWSE_JAVA:
            browse_java();
            break;
        case ID_OPEN_DIR:
            open_game_dir();
            break;
        case ID_USER_BUTTON:
            open_user_window();
            break;
        case ID_SETTINGS:
            open_settings_window();
            break;
        case ID_EXPORT_LOG:
            export_log();
            break;
        case ID_VERSION:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                wchar_t *version = get_control_text(g_version);
                gui_logf(L"用户选择版本：%ls",
                         version && *version ? version : L"(空)");
                free(version);
            }
            break;
        case ID_JAVA:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                wchar_t *java = get_selected_java_path();
                gui_logf(L"用户选择 Java：%ls",
                         java ? java : L"自动选择");
                free(java);
            }
            break;
        case ID_DEMO:
            if (HIWORD(wparam) == BN_CLICKED) {
                int demo = IsDlgButtonChecked(g_hwnd, ID_DEMO) == BST_CHECKED;
                gui_logf(L"演示模式：%ls", demo ? L"开启" : L"关闭");
            }
            break;
        default:
            break;
        }
        return 0;

    case WM_APP_JOB_DONE: {
        int kind = (int)wparam;
        int ok = (int)lparam;
        char *installed = NULL;
        InterlockedExchange(&g_running, 0);
        mclc_progress_clear();
        set_busy(FALSE);

        if (kind == JOB_DOWNLOAD) {
            installed = (char *)InterlockedExchangePointer(
                (PVOID volatile *)&g_installed_version, NULL);
            if (g_progress) {
                if (g_progress_bar) {
                    SendMessageW(g_progress_bar, PBM_SETPOS,
                                 ok ? 100 : 0, 0);
                }
                if (g_progress_text) {
                    SetWindowTextW(g_progress_text,
                                   ok ? L"下载与校验完成"
                                      : L"下载失败，请查看日志");
                }
                {
                    MSG pending;
                    while (PeekMessageW(&pending, g_progress, WM_APP_PROGRESS,
                                        WM_APP_PROGRESS, PM_REMOVE)) {
                        free((void *)pending.lParam);
                    }
                }
                DestroyWindow(g_progress);
            }
        }

        if (ok) {
            if (kind == JOB_DOWNLOAD) {
                refresh_versions();
            }
            if (installed) {
                select_version_by_id(installed);
            }
            if (kind == JOB_DOWNLOAD) {
                set_status(L"下载与校验完成。");
            } else {
                set_status(L"Minecraft 已启动。");
            }
        } else {
            set_status(L"操作失败，请查看日志。");
        }
        free(installed);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        if ((HWND)lparam == g_log) {
            SetBkMode((HDC)wparam, OPAQUE);
            SetBkColor((HDC)wparam, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkMode((HDC)wparam, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        gui_logf(L"用户尝试关闭主窗口。");
        if (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
            if (MessageBoxW(hwnd,
                            L"任务正在进行。关闭窗口会中断任务，确定退出吗？",
                            L"MCLC", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 0;
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        mclc_progress_clear();
        g_log = NULL;
        InterlockedExchange(&g_running, 0);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line,
                   int show_command)
{
    WNDCLASSW window_class;
    RECT rect;
    int width;
    int height;
    int x;
    int y;
    int message_result;
    int com_ready;
    MSG message;

    (void)previous;
    (void)command_line;
    g_instance = instance;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if (!setlocale(LC_CTYPE, ".UTF-8")) {
        setlocale(LC_CTYPE, ".UTF8");
    }
    InitializeCriticalSection(&g_log_lock);
    g_log_lock_ready = 1;
    gui_logf(L"MCLC GUI 启动。");
    load_config();
    if (!g_config_path || !path_exists(g_config_path)) {
        save_config();
    }
    SetProcessDPIAware();
    com_ready = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    {
        INITCOMMONCONTROLSEX controls;
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES |
                         ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&controls);
    }
    setup_log_pipe();

    memset(&window_class, 0, sizeof(window_class));
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    window_class.lpszClassName = MCLC_GUI_CLASS;
    if (!RegisterClassW(&window_class)) {
        MessageBoxW(NULL, L"无法注册窗口类。", L"MCLC", MB_ICONERROR);
        if (com_ready) CoUninitialize();
        return 1;
    }

    rect.left = 0;
    rect.top = 0;
    rect.right = 760;
    rect.bottom = 610;
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                       WS_MINIMIZEBOX, FALSE, 0);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_hwnd = CreateWindowExW(
        0, MCLC_GUI_CLASS, MCLC_GUI_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, width, height, NULL, NULL, instance, NULL);
    if (!g_hwnd) {
        MessageBoxW(NULL, L"无法创建主窗口。", MCLC_GUI_TITLE, MB_ICONERROR);
        if (com_ready) CoUninitialize();
        return 1;
    }

    ShowWindow(g_hwnd, show_command ? show_command : SW_SHOW);
    UpdateWindow(g_hwnd);

    memset(&message, 0, sizeof(message));
    while ((message_result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(g_hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (com_ready) CoUninitialize();
    return message_result == 0 ? (int)message.wParam : 1;
}
