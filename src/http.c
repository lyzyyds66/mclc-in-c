#include "http.h"
#include "util.h"

#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

#define MCLC_MAX_HOST_CONNECTIONS 16
#define MCLC_USER_AGENT L"MCLC/1.0 (Minecraft C Launcher)"

typedef struct {
    wchar_t *host;
    unsigned short port;
    int secure;
    HINTERNET connection;
} HostConnection;

typedef struct {
    int initialized;
    HINTERNET session;
    HostConnection hosts[MCLC_MAX_HOST_CONNECTIONS];
    int host_count;
} HttpClient;

static HttpClient g_http;

static char *win32_error_text(DWORD code)
{
    wchar_t *buffer = NULL;
    DWORD n;
    char *text;
    size_t len;

    n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPWSTR)&buffer, 0, NULL);
    if (!n || !buffer) {
        return format_utf8("Windows 错误 %lu", (unsigned long)code);
    }
    while (n > 0 && (buffer[n - 1] == L'\r' || buffer[n - 1] == L'\n' ||
                     buffer[n - 1] == L' ')) {
        buffer[--n] = L'\0';
    }
    text = wide_to_utf8(buffer);
    LocalFree(buffer);
    if (!text) {
        return format_utf8("Windows 错误 %lu", (unsigned long)code);
    }
    len = strlen(text);
    if (len > 200) {
        text[200] = '\0';
    }
    return text;
}

static int http_init(char **error)
{
    DWORD protocols;

    if (g_http.initialized) {
        return 0;
    }
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    g_http.session = WinHttpOpen(
        MCLC_USER_AGENT,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_http.session)
#endif
    {
        g_http.session = WinHttpOpen(
            MCLC_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!g_http.session) {
        char *msg = win32_error_text(GetLastError());
        set_error(error, "无法初始化 WinHTTP: %s", msg);
        free(msg);
        return -1;
    }
    WinHttpSetTimeouts(g_http.session, 15000, 30000, 30000, 60000);
    {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(g_http.session, WINHTTP_OPTION_REDIRECT_POLICY,
                         &policy, sizeof(policy));
    }
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(g_http.session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                     &protocols, sizeof(protocols));
    g_http.initialized = 1;
    return 0;
}

static HINTERNET http_connect(const wchar_t *host, unsigned short port,
                              int secure, char **error)
{
    int i;
    HINTERNET connection;

    for (i = 0; i < g_http.host_count; i++) {
        HostConnection *hc = &g_http.hosts[i];
        if (hc->port == port && hc->secure == secure &&
            wcscmp(hc->host, host) == 0) {
            return hc->connection;
        }
    }

    connection = WinHttpConnect(g_http.session, host, port, 0);
    if (!connection) {
        char *msg = win32_error_text(GetLastError());
        set_error(error, "无法连接 %ls:%u: %s", host, (unsigned)port, msg);
        free(msg);
        return NULL;
    }
    if (g_http.host_count >= MCLC_MAX_HOST_CONNECTIONS) {
        WinHttpCloseHandle(g_http.hosts[0].connection);
        free(g_http.hosts[0].host);
        memmove(&g_http.hosts[0], &g_http.hosts[1],
                sizeof(HostConnection) * (MCLC_MAX_HOST_CONNECTIONS - 1));
        g_http.host_count--;
    }
    {
        HostConnection *hc = &g_http.hosts[g_http.host_count++];
        hc->host = xwcsdup(host);
        hc->port = port;
        hc->secure = secure;
        hc->connection = connection;
    }
    return connection;
}

/*
 * Open, send and receive a GET request. Returns a request handle positioned at
 * the response body, or NULL with *error set. The caller closes the handle.
 */
static int url_is_https(const wchar_t *url)
{
    static const wchar_t scheme[] = L"https:";
    size_t i;

    if (!url) {
        return 0;
    }
    for (i = 0; scheme[i]; i++) {
        wchar_t c = url[i];
        if (c >= L'A' && c <= L'Z') {
            c = (wchar_t)(c - L'A' + L'a');
        }
        if (c != scheme[i]) {
            return 0;
        }
    }
    return 1;
}

static HINTERNET http_get_request(const wchar_t *url, char **error)
{
    URL_COMPONENTS uc;
    wchar_t host[256];
    wchar_t *path = NULL;
    size_t path_len;
    HINTERNET connection;
    HINTERNET request = NULL;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    DWORD last_status = 0;
    DWORD last_error = 0;
    int secure;
    int attempt;

    if (http_init(error) != 0) {
        return NULL;
    }

    if (!url || !*url) {
        set_error(error, "URL 为空");
        return NULL;
    }

    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        char *msg = win32_error_text(GetLastError());
        set_error(error, "无效 URL: %ls (%s)", url, msg);
        free(msg);
        return NULL;
    }
    if (uc.dwHostNameLength == 0 || uc.dwHostNameLength >= 256) {
        set_error(error, "URL 主机名无效: %ls", url);
        return NULL;
    }
    wcsncpy(host, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = L'\0';

    path_len = (size_t)uc.dwUrlPathLength + (size_t)uc.dwExtraInfoLength;
    path = (wchar_t *)xmalloc((path_len + 2) * sizeof(wchar_t));
    if (uc.dwUrlPathLength) {
        memcpy(path, uc.lpszUrlPath, (size_t)uc.dwUrlPathLength * sizeof(wchar_t));
    }
    if (uc.dwExtraInfoLength) {
        memcpy(path + uc.dwUrlPathLength, uc.lpszExtraInfo,
               (size_t)uc.dwExtraInfoLength * sizeof(wchar_t));
    }
    path[path_len] = L'\0';
    if (path[0] != L'/') {
        memmove(path + 1, path, (path_len + 1) * sizeof(wchar_t));
        path[0] = L'/';
    }

    secure = url_is_https(url);
    connection = http_connect(host, (unsigned short)uc.nPort, secure, error);
    if (!connection) {
        free(path);
        return NULL;
    }

    for (attempt = 0; attempt < 4; attempt++) {
        request = WinHttpOpenRequest(
            connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            char *msg = win32_error_text(GetLastError());
            set_error(error, "创建 HTTP 请求失败: %s", msg);
            free(msg);
            free(path);
            return NULL;
        }
        WinHttpAddRequestHeaders(
            request,
            L"User-Agent: " MCLC_USER_AGENT L"\r\nAccept: */*\r\n",
            (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, NULL)) {
            status = 0;
            status_size = sizeof(status);
            if (WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                    WINHTTP_NO_HEADER_INDEX)) {
                last_status = status;
                if (status >= 200 && status < 300) {
                    free(path);
                    return request;
                }
                if (status != 0 && status != 429 && status < 500) {
                    set_error(error, "HTTP %lu: %ls", (unsigned long)status, url);
                    WinHttpCloseHandle(request);
                    free(path);
                    return NULL;
                }
            } else {
                last_error = GetLastError();
            }
        } else {
            last_error = GetLastError();
        }

        WinHttpCloseHandle(request);
        request = NULL;
        if (attempt < 3) {
            Sleep(700u * (DWORD)(attempt + 1));
        }
    }

    if (last_status) {
        set_error(error, "下载失败: %ls（最后一次 HTTP 状态 %lu）",
                  url, (unsigned long)last_status);
    } else if (last_error) {
        char *msg = win32_error_text(last_error);
        set_error(error, "下载失败: %ls (%s)", url, msg);
        free(msg);
    } else {
        set_error(error, "下载失败: %ls（网络错误）", url);
    }
    free(path);
    return NULL;
}

static int http_content_length(HINTERNET request, int64_t *out)
{
    wchar_t buffer[64];
    DWORD size = sizeof(buffer);
    wchar_t *end = NULL;

    *out = -1;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX, buffer, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return -1;
    }
    buffer[63] = L'\0';
    *out = (int64_t)wcstoll(buffer, &end, 10);
    if (end == buffer || *out < 0) {
        *out = -1;
        return -1;
    }
    return 0;
}

int http_download(const wchar_t *url, const wchar_t *destination,
                  int show_progress, int64_t expected_size, char **error)
{
    HINTERNET request;
    wchar_t *temporary;
    FILE *file;
    unsigned char buffer[65536];
    int64_t total = 0;
    int64_t content_length = -1;
    int64_t progress_total;
    DWORD read;
    ULONGLONG started = GetTickCount64();
    ULONGLONG last_update = 0;
    const wchar_t *name;
    char *name_utf8 = NULL;
    int rc = -1;

    request = http_get_request(url, error);
    if (!request) {
        return -1;
    }
    if (ensure_parent_dir(destination) != 0) {
        set_error(error, "无法创建目录: %ls", destination);
        WinHttpCloseHandle(request);
        return -1;
    }

    temporary = (wchar_t *)xmalloc((wcslen(destination) + 6) * sizeof(wchar_t));
    wcscpy(temporary, destination);
    wcscat(temporary, L".part");
    DeleteFileW(temporary);
    file = _wfopen(temporary, L"wb");
    if (!file) {
        set_error(error, "无法写入文件: %ls", temporary);
        free(temporary);
        WinHttpCloseHandle(request);
        return -1;
    }

    http_content_length(request, &content_length);
    progress_total = content_length > 0 ? content_length : expected_size;
    if (show_progress) {
        name = wcsrchr(destination, L'\\');
        name = name ? name + 1 : destination;
        name_utf8 = wide_to_utf8(name);
        printf("  下载 %s", name_utf8 ? name_utf8 : "文件");
        fflush(stdout);
    }

    for (;;) {
        read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) {
            char *msg = win32_error_text(GetLastError());
            set_error(error, "读取 HTTP 数据失败: %s", msg);
            free(msg);
            goto cleanup;
        }
        if (read == 0) {
            break;
        }
        if (fwrite(buffer, 1, read, file) != read) {
            set_error(error, "写入文件失败: %ls", temporary);
            goto cleanup;
        }
        total += read;
        if (show_progress) {
            ULONGLONG now = GetTickCount64();
            if (now - last_update >= 100 || total == progress_total) {
                last_update = now;
                if (progress_total > 0) {
                    double pct = (double)total * 100.0 / (double)progress_total;
                    printf("\r  下载 %s  %5.1f%%  (%.1f/%.1f MB)",
                           name_utf8 ? name_utf8 : "文件", pct,
                           (double)total / 1048576.0,
                           (double)progress_total / 1048576.0);
                } else {
                    printf("\r  下载 %s  %.1f MB",
                           name_utf8 ? name_utf8 : "文件",
                           (double)total / 1048576.0);
                }
                fflush(stdout);
            }
        }
    }

    if (fflush(file) != 0) {
        set_error(error, "刷新文件失败: %ls", temporary);
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = NULL;
        set_error(error, "关闭文件失败: %ls", temporary);
        goto cleanup;
    }
    file = NULL;

    if (expected_size > 0 && total != expected_size) {
        set_error(error, "文件大小不符: %ls（期望 %lld，实际 %lld）",
                  destination, (long long)expected_size, (long long)total);
        goto cleanup;
    }
    if (content_length > 0 && total != content_length) {
        set_error(error, "下载被截断: %ls（期望 %lld，实际 %lld）",
                  destination, (long long)content_length, (long long)total);
        goto cleanup;
    }
    if (!MoveFileExW(temporary, destination,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        char *msg = win32_error_text(GetLastError());
        set_error(error, "无法保存文件 %ls: %s", destination, msg);
        free(msg);
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (file) {
        fclose(file);
    }
    if (rc != 0) {
        DeleteFileW(temporary);
    }
    if (show_progress) {
        if (rc == 0) {
            double seconds = (double)(GetTickCount64() - started) / 1000.0;
            printf("\r  \xE2\x9C\x93 %s  %.1f MB  %.1fs\n",
                   name_utf8 ? name_utf8 : "文件",
                   (double)total / 1048576.0, seconds);
        } else {
            printf("\n");
        }
    }
    free(name_utf8);
    free(temporary);
    WinHttpCloseHandle(request);
    return rc;
}

int http_get_string(const wchar_t *url, char **data, size_t *size,
                    size_t max_size, char **error)
{
    HINTERNET request;
    StrBuf sb;
    unsigned char buffer[16384];
    DWORD read;

    if (data) {
        *data = NULL;
    }
    if (size) {
        *size = 0;
    }
    request = http_get_request(url, error);
    if (!request) {
        return -1;
    }
    sb_init(&sb);
    for (;;) {
        read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) {
            char *msg = win32_error_text(GetLastError());
            set_error(error, "读取 HTTP 数据失败: %s", msg);
            free(msg);
            sb_free(&sb);
            WinHttpCloseHandle(request);
            return -1;
        }
        if (read == 0) {
            break;
        }
        if (max_size && sb.len + read > max_size) {
            set_error(error, "HTTP 响应过大（限制 %zu 字节）", max_size);
            sb_free(&sb);
            WinHttpCloseHandle(request);
            return -1;
        }
        if (sb_append_n(&sb, (const char *)buffer, read) != 0) {
            set_error(error, "内存不足");
            sb_free(&sb);
            WinHttpCloseHandle(request);
            return -1;
        }
    }
    WinHttpCloseHandle(request);
    if (size) {
        *size = sb.len;
    }
    if (data) {
        *data = sb_take(&sb);
    } else {
        sb_free(&sb);
    }
    return 0;
}
