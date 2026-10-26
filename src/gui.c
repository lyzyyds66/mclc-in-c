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
#define MCLC_GUI_TITLE L"MCLC - Minecraft Java 启动器"
#define WM_APP_JOB_DONE (WM_APP + 1)

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
    ID_VERIFY,
    ID_LAUNCH,
    ID_INSTALL_LATEST,
    ID_OPEN_DIR,
    ID_LOG,
    ID_STATUS
};

enum {
    JOB_LAUNCH = 1,
    JOB_INSTALL_LATEST = 2
};

typedef struct {
    HWND hwnd;
    int kind;
    wchar_t *game_dir;
    char *version_id;
    char *username;
    wchar_t *java;
    int memory_mb;
    int demo;
    int verify;
} GuiJob;

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
static HWND g_verify;
static HWND g_launch;
static HWND g_install_latest;
static HWND g_open_dir;
static HWND g_log;
static HWND g_status;
static HFONT g_font;
static HFONT g_log_font;
static HANDLE g_log_read;
static volatile LONG g_running;
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
        set_status(L"没有本地版本。可以直接输入官方版本号后启动，或点“安装最新正式版”。");
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
    if (local && path_exists(local)) {
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
    char *version_id;
    int rc;

    version_id = ensure_version_json(job->game_dir, job->version_id,
                                     job->verify, error);
    if (!version_id) {
        return -1;
    }
    if (version_load(job->game_dir, version_id, &version, error) != 0) {
        free(version_id);
        return -1;
    }
    printf("[*] 版本: %s（Java %d+，类型 %s）\n", version.id,
           version.java_major,
           version.version_type ? version.version_type : "release");
    if (install_version(&version, job->game_dir, job->verify, error) != 0) {
        version_free(&version);
        free(version_id);
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
    free(version_id);
    return rc < 0 ? -1 : 0;
}

static int job_install_latest(GuiJob *job, char **error)
{
    RemoteManifest manifest;
    const RemoteVersion *entry;
    Version version;
    char *version_id = NULL;
    int rc = -1;

    if (remote_manifest_fetch(&manifest, error) != 0) {
        return -1;
    }
    entry = remote_find(&manifest, "latest");
    if (!entry) {
        set_error(error, "Mojang 版本清单中没有 latest 正式版");
        remote_manifest_free(&manifest);
        return -1;
    }
    version_id = xstrdup(entry->id);
    printf("[*] 最新正式版: %s\n", entry->id);
    if (remote_install_json(job->game_dir, entry, 0, job->verify,
                            error) != 0) {
        goto done;
    }
    remote_manifest_free(&manifest);
    memset(&manifest, 0, sizeof(manifest));

    if (version_load(job->game_dir, version_id, &version, error) != 0) {
        goto done;
    }
    printf("[*] 安装 %s（Java %d+）\n", version.id, version.java_major);
    if (install_version(&version, job->game_dir, job->verify, error) != 0) {
        version_free(&version);
        goto done;
    }
    printf("[完成] %s 已安装。\n", version.id);
    {
        char *copy = xstrdup(version.id);
        char *old = (char *)InterlockedExchangePointer(
            (PVOID volatile *)&g_installed_version, copy);
        free(old);
    }
    version_free(&version);
    rc = 0;

done:
    free(version_id);
    remote_manifest_free(&manifest);
    return rc;
}

static unsigned __stdcall job_thread(void *parameter)
{
    GuiJob *job = (GuiJob *)parameter;
    char *error = NULL;
    int rc;

    if (job->kind == JOB_INSTALL_LATEST) {
        rc = job_install_latest(job, &error);
    } else {
        rc = job_launch(job, &error);
    }

    if (rc < 0) {
        fprintf(stderr, "[错误] %s\n",
                error ? error : "未知错误");
    } else if (job->kind == JOB_INSTALL_LATEST) {
        fprintf(stderr, "[完成] 安装完成。\n");
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
    EnableWindow(g_verify, !busy);
    EnableWindow(g_launch, !busy);
    EnableWindow(g_install_latest, !busy);
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
    make_label(L"留空自动查找 Java", 500, 97, 160, 22);

    make_label(L"Java", 16, 135, 78, 22);
    g_java = make_control(L"EDIT", L"",
                          WS_BORDER | ES_AUTOHSCROLL,
                          WS_EX_CLIENTEDGE, 100, 132, 540, 26, ID_JAVA);
    g_browse_java = make_control(L"BUTTON", L"浏览...", BS_PUSHBUTTON,
                                 0, 648, 132, 96, 26, ID_BROWSE_JAVA);

    g_demo = make_control(L"BUTTON", L"演示模式",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                          104, 170, 100, 24, ID_DEMO);
    g_verify = make_control(L"BUTTON", L"校验已有文件 SHA-1",
                            BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                            240, 170, 190, 24, ID_VERIFY);

    g_launch = make_control(L"BUTTON", L"启动游戏", BS_PUSHBUTTON | WS_TABSTOP,
                            0, 100, 202, 140, 34, ID_LAUNCH);
    g_install_latest = make_control(L"BUTTON", L"安装最新正式版",
                                    BS_PUSHBUTTON | WS_TABSTOP, 0,
                                    252, 202, 170, 34, ID_INSTALL_LATEST);
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
        SetWindowTextW(g_java, path);
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

static void start_job(GuiJob *job, const wchar_t *status)
{
    HANDLE thread;

    if (!job) {
        return;
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
        return;
    }
    CloseHandle(thread);
}

static GuiJob *base_job(int kind)
{
    GuiJob *job = (GuiJob *)xcalloc(1, sizeof(GuiJob));
    wchar_t *user;
    wchar_t *java;
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

    job->verify = (IsDlgButtonChecked(g_hwnd, ID_VERIFY) == BST_CHECKED);
    if (kind == JOB_INSTALL_LATEST) {
        job->username = xstrdup("Steve");
        job->memory_mb = 2048;
        return job;
    }

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

    java = get_control_text(g_java);
    job->java = *java ? xwcsdup(java) : NULL;
    free(java);
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
    start_job(job, L"正在检查并下载游戏文件...");
}

static void start_install_latest(void)
{
    GuiJob *job;

    if (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
        return;
    }

    job = base_job(JOB_INSTALL_LATEST);
    if (!job) {
        return;
    }
    start_job(job, L"正在获取最新正式版信息...");
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
        case ID_INSTALL_LATEST:
            start_install_latest();
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
        set_busy(FALSE);
        if (kind == JOB_INSTALL_LATEST) {
            installed = (char *)InterlockedExchangePointer(
                (PVOID volatile *)&g_installed_version, NULL);
        }
        if (ok) {
            if (kind == JOB_INSTALL_LATEST || kind == JOB_LAUNCH) {
                refresh_versions();
            }
            if (installed) {
                select_version_by_id(installed);
            }
            if (kind == JOB_INSTALL_LATEST) {
                set_status(L"安装完成。");
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
