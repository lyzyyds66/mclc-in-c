/*
 * MCLC GUI - native Win32 front-end.
 *
 * The GUI reuses the same version/install/launch core as the CLI. Core
 * progress output is redirected into a pipe and rendered in the log control;
 * no console window is created.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "util.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MCLC_GUI_CLASS L"MCLCGuiWindow"
#define MCLC_PROGRESS_CLASS L"MCLCDownloadProgress"
#define MCLC_PICKER_CLASS L"MCLCDownloadPicker"
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
    ID_DOWNLOAD,
    ID_OPEN_DIR,
    ID_LOG,
    ID_STATUS,

    ID_PICK_CATEGORY = 300,
    ID_PICK_LIST,
    ID_PICK_CONFIRM
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
    wchar_t *java;
    int memory_mb;
    int demo;
    int verify;
} GuiJob;

typedef struct {
    wchar_t *path;
    wchar_t *label;
} JavaEntry;

static JavaEntry *g_java_entries;
static size_t g_java_entry_count;

static HINSTANCE g_instance;
static HWND g_hwnd;
static HWND g_game_dir;
static HWND g_browse_game;
static HWND g_version;
static HWND g_refresh;
static HWND g_user;
static HWND g_memory;
static HWND g_java;
static HWND g_browse_java;
static HWND g_demo;
static HWND g_launch;
static HWND g_download;
static HWND g_open_dir;
static HWND g_log;
static HWND g_status;
static HWND g_progress;
static HWND g_progress_bar;
static HWND g_progress_text;
static HFONT g_font;
static HFONT g_log_font;
static HANDLE g_log_read;
static volatile LONG g_running;
static volatile LONG g_picker_loading;
static char *g_installed_version;

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

static void append_log_text(const wchar_t *text)
{
    int len;

    if (!g_log || !text || !*text) {
        return;
    }
    len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETREADONLY, FALSE, 0);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_log, EM_SETREADONLY, TRUE, 0);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
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
    append_log_text(out);

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
    _close(fd);
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
    free(job->java);
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

static int job_launch(GuiJob *job, char **error)
{
    Version version;
    LaunchOptions launch;
    int rc;

    if (version_load(job->game_dir, job->version_id, &version, error) != 0) {
        char *detail = (error && *error) ? xstrdup(*error) : NULL;
        set_error(error, "请先点击“下载游戏”安装该版本。%s%s",
                  detail ? " " : "", detail ? detail : "");
        free(detail);
        return -1;
    }

    memset(&launch, 0, sizeof(launch));
    launch.game_dir = job->game_dir;
    launch.username = job->username;
    launch.java = job->java;
    launch.memory_mb = job->memory_mb;
    launch.demo = job->demo;
    launch.detach = 1;
    launch.gui_mode = 1;
    rc = launch_version(&launch, &version, error);
    version_free(&version);
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
        fprintf(stderr, "[错误] %s\n",
                error ? error : "未知错误");
    } else if (job->kind == JOB_DOWNLOAD) {
        fprintf(stderr, "[完成] 下载与校验完成。\n");
    } else {
        fprintf(stderr, "[完成] Minecraft 已启动。\n");
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
    EnableWindow(g_user, !busy);
    EnableWindow(g_memory, !busy);
    EnableWindow(g_java, !busy);
    EnableWindow(g_browse_java, !busy);
    EnableWindow(g_demo, !busy);
    EnableWindow(g_launch, !busy);
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

    make_label(L"用户名", 16, 97, 78, 22);
    g_user = make_control(L"EDIT", L"Steve",
                          WS_BORDER | ES_AUTOHSCROLL,
                          WS_EX_CLIENTEDGE, 100, 94, 220, 26, ID_USER);
    make_label(L"内存 MB", 340, 97, 64, 22);
    g_memory = make_control(L"EDIT", L"2048",
                            WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                            WS_EX_CLIENTEDGE, 408, 94, 80, 26, ID_MEMORY);
    make_label(L"Java 支持多版本选择", 500, 97, 180, 22);

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
    g_download = make_control(L"BUTTON", L"下载游戏",
                              BS_PUSHBUTTON | WS_TABSTOP, 0,
                              252, 202, 170, 34, ID_DOWNLOAD);
    g_open_dir = make_control(L"BUTTON", L"打开游戏目录",
                              BS_PUSHBUTTON | WS_TABSTOP, 0,
                              434, 202, 140, 34, ID_OPEN_DIR);

    make_label(L"日志", 16, 246, 78, 22);
    g_log = make_control(
        L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        WS_EX_CLIENTEDGE, 16, 268, 728, 290, ID_LOG);
    if (g_log) {
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_log_font, TRUE);
        SendMessageW(g_log, EM_SETLIMITTEXT, 4u * 1024u * 1024u, 0);
    }
    g_status = make_control(L"STATIC", L"就绪。", SS_LEFT, 0,
                            16, 572, 728, 22, ID_STATUS);
    if (g_status) {
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
    }

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

    memset(&browse, 0, sizeof(browse));
    browse.hwndOwner = g_hwnd;
    browse.pszDisplayName = display_name;
    browse.lpszTitle = L"选择 Minecraft 游戏目录（.minecraft）";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    pidl = SHBrowseForFolderW(&browse);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) {
            SetWindowTextW(g_game_dir, path);
            refresh_versions();
        }
        CoTaskMemFree(pidl);
    }
}

static void browse_java(void)
{
    OPENFILENAMEW dialog;
    wchar_t path[MAX_PATH];

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
        if (entry < 0) {
            MessageBoxW(g_hwnd, L"所选文件不是有效的 java.exe。",
                        L"MCLC", MB_ICONWARNING);
        } else {
            select_java_entry(entry);
        }
    }
}

static void open_game_dir(void)
{
    wchar_t *dir = get_game_dir_value();
    HINSTANCE result;

    if (!dir) {
        MessageBoxW(g_hwnd, L"游戏目录无效。", L"MCLC", MB_ICONERROR);
        return;
    }
    mkdir_p(dir);
    result = ShellExecuteW(g_hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        MessageBoxW(g_hwnd, L"无法打开游戏目录。", L"MCLC", MB_ICONERROR);
    }
    free(dir);
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
    InterlockedExchange(&g_running, 1);
    set_busy(TRUE);
    set_status(status);
    thread = (HANDLE)_beginthreadex(NULL, 0, job_thread, job, 0, NULL);
    if (!thread) {
        InterlockedExchange(&g_running, 0);
        set_busy(FALSE);
        set_status(L"无法创建后台线程。");
        free_job(job);
        MessageBoxW(g_hwnd, L"无法创建后台线程。", L"MCLC", MB_ICONERROR);
        return 0;
    }
    CloseHandle(thread);
    return 1;
}

static GuiJob *base_job(int kind)
{
    GuiJob *job = (GuiJob *)xcalloc(1, sizeof(GuiJob));
    wchar_t *user;
    wchar_t *memory_text;
    int memory;

    job->hwnd = g_hwnd;
    job->kind = kind;
    job->game_dir = get_game_dir_value();
    if (!job->game_dir) {
        MessageBoxW(g_hwnd, L"游戏目录无效。", L"MCLC", MB_ICONERROR);
        free_job(job);
        return NULL;
    }
    if (mkdir_p(job->game_dir) != 0) {
        MessageBoxW(g_hwnd, L"无法创建游戏目录。", L"MCLC", MB_ICONERROR);
        free_job(job);
        return NULL;
    }

    if (kind == JOB_DOWNLOAD) {
        job->verify = 1;
        job->username = xstrdup("Steve");
        job->memory_mb = 2048;
        return job;
    }
    job->verify = 0;

    user = get_control_text(g_user);
    if (!*user) {
        free(user);
        user = xwcsdup(L"Steve");
    }
    job->username = wide_to_utf8(user);
    free(user);
    if (!job->username) {
        MessageBoxW(g_hwnd, L"用户名编码无效。", L"MCLC", MB_ICONERROR);
        free_job(job);
        return NULL;
    }

    memory_text = get_control_text(g_memory);
    memory = _wtoi(memory_text);
    free(memory_text);
    if (memory < 256 || memory > 65536) {
        MessageBoxW(g_hwnd, L"内存必须是 256 到 65536 之间的 MB 数。",
                    L"MCLC", MB_ICONERROR);
        free_job(job);
        return NULL;
    }
    job->memory_mb = memory;

    job->java = get_selected_java_path();
    job->demo = (IsDlgButtonChecked(g_hwnd, ID_DEMO) == BST_CHECKED);
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
        MessageBoxW(g_hwnd, L"请选择或输入版本号。", L"MCLC", MB_ICONWARNING);
        free_job(job);
        return;
    }
    job->version_id = id;
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
    if (!create_progress_window(version_id)) {
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
    if (InterlockedCompareExchange(&g_picker_loading, 1, 0) != 0) {
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
        SetWindowTextW(state.status, L"无法启动版本清单加载线程。");
        set_status(L"无法获取版本清单。");
    } else {
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
        set_status(L"已取消下载。");
    }
    remote_manifest_free(&state.manifest);
    free(state.selected);
    free(state.selected_url);
    free(state.selected_sha1);
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
        append_log_text(L"MCLC GUI 已就绪。\r\n");
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_LAUNCH:
            start_launch();
            break;
        case ID_DOWNLOAD:
            open_download_picker();
            break;
        case ID_REFRESH:
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
    SetProcessDPIAware();
    com_ready = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    {
        INITCOMMONCONTROLSEX controls;
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_PROGRESS_CLASS;
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
