#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winhttp.h>

#define ID_GAME_PATH 1101
#define ID_ROOM 1103
#define ID_ROLE 1105
#define ID_CHOOSE_GAME 1107
#define ID_START 1108
#define ID_STATUS 1109
#define WM_LAUNCH_COMPLETE (WM_APP + 1)
#define WELNPT_DEFAULT_API_URL L"http://8.155.145.132:8082/api/v1"

typedef struct launch_context {
    HWND window;
    wchar_t game_path[MAX_PATH];
    wchar_t hook_path[MAX_PATH];
    wchar_t log_path[MAX_PATH];
    wchar_t relay[MAX_PATH];
    wchar_t room[64];
    wchar_t logical_ip[64];
    wchar_t token[256];
    wchar_t api_url[512];
    wchar_t username[256];
    wchar_t password[256];
    int room_id;
    wchar_t status[1024];
    DWORD process_id;
    int is_host;
    int success;
} launch_context;

static HWND g_window;
static HWND g_game_path;
static HWND g_room;
static HWND g_api_url;
static HWND g_username;
static HWND g_password;
static HWND g_role;
static HWND g_start;
static HWND g_status;

static int valid_ipv4(const wchar_t *value);
static int valid_room(const wchar_t *value);
static int valid_token(const wchar_t *value);

static int wide_to_utf8(const wchar_t *value, char *result, int count) {
    int length;
    if (value == NULL || result == NULL || count < 1) return 0;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result, count, NULL, NULL);
    return length > 0;
}

static int json_escape(const char *value, char *result, size_t count) {
    size_t source, target = 0;
    if (value == NULL || result == NULL || count == 0) return 0;
    for (source = 0; value[source] != '\0'; ++source) {
        unsigned char character = (unsigned char)value[source];
        const char *escaped = NULL;
        if (character == '\\') escaped = "\\\\";
        else if (character == '"') escaped = "\\\"";
        else if (character == '\r') escaped = "\\r";
        else if (character == '\n') escaped = "\\n";
        if (escaped != NULL) {
            size_t length = strlen(escaped);
            if (target + length + 1 > count) return 0;
            memcpy(result + target, escaped, length);
            target += length;
        } else {
            if (character < 0x20 || target + 2 > count) return 0;
            result[target++] = (char)character;
        }
    }
    result[target] = '\0';
    return 1;
}

static int json_string_value(const char *json, const char *key, char *result, size_t count) {
    char marker[96];
    const char *cursor;
    size_t target = 0;
    if (json == NULL || key == NULL || result == NULL || count == 0) return 0;
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "\"%s\"", key);
    cursor = strstr(json, marker);
    if (cursor == NULL) return 0;
    cursor = strchr(cursor + strlen(marker), ':');
    if (cursor == NULL) return 0;
    ++cursor;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
    if (*cursor++ != '"') return 0;
    while (*cursor != '\0' && *cursor != '"') {
        char character = *cursor++;
        if (character == '\\') {
            character = *cursor++;
            if (character == '\0') return 0;
            if (character == 'n') character = '\n';
            else if (character == 'r') character = '\r';
            else if (character == 't') character = '\t';
        }
        if (target + 1 >= count) return 0;
        result[target++] = character;
    }
    if (*cursor != '"') return 0;
    result[target] = '\0';
    return 1;
}

static int json_integer_value(const char *json, const char *key, int *result) {
    char marker[96];
    const char *cursor;
    char *end;
    long value;
    if (json == NULL || key == NULL || result == NULL) return 0;
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "\"%s\"", key);
    cursor = strstr(json, marker);
    if (cursor == NULL) return 0;
    cursor = strchr(cursor + strlen(marker), ':');
    if (cursor == NULL) return 0;
    value = strtol(cursor + 1, &end, 10);
    if (end == cursor + 1 || value < 1 || value > 65535) return 0;
    *result = (int)value;
    return 1;
}

static int api_request(const wchar_t *base_url, const wchar_t *endpoint, const wchar_t *method,
    const char *body, const char *bearer, DWORD *status, char **response_body, wchar_t *error_text, size_t error_count) {
    wchar_t url[512], host[256], path[512], full_path[768];
    URL_COMPONENTS components;
    HINTERNET session = NULL, connection = NULL, request = NULL;
    DWORD flags = 0, value, available, received, used = 0;
    char *response = NULL;
    int ok = 0;

    if (base_url == NULL || endpoint == NULL || method == NULL || status == NULL || response_body == NULL) return 0;
    *response_body = NULL;
    wcsncpy_s(url, ARRAYSIZE(url), base_url, _TRUNCATE);
    ZeroMemory(&components, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = (DWORD)-1;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &components)) goto failed;
    if (components.dwHostNameLength >= ARRAYSIZE(host) || components.dwUrlPathLength >= ARRAYSIZE(path)) goto failed;
    wcsncpy_s(host, ARRAYSIZE(host), components.lpszHostName, components.dwHostNameLength);
    wcsncpy_s(path, ARRAYSIZE(path), components.lpszUrlPath, components.dwUrlPathLength);
    _snwprintf_s(full_path, ARRAYSIZE(full_path), _TRUNCATE, L"%ls%ls", path, endpoint);
    if (components.nScheme == INTERNET_SCHEME_HTTPS) flags = WINHTTP_FLAG_SECURE;
    session = WinHttpOpen(L"WEL-NoTap/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (session == NULL) goto failed;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    connection = WinHttpConnect(session, host, components.nPort, 0);
    if (connection == NULL) goto failed;
    request = WinHttpOpenRequest(connection, method, full_path, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request == NULL) goto failed;
    if (!WinHttpAddRequestHeadersW(request, L"Content-Type: application/json\r\nAccept: application/json\r\n", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto failed;
    if (bearer != NULL && bearer[0] != '\0') {
        wchar_t authorization[2304];
        wchar_t bearer_wide[2048];
        int length = MultiByteToWideChar(CP_UTF8, 0, bearer, -1, bearer_wide, ARRAYSIZE(bearer_wide));
        if (length <= 0) goto failed;
        _snwprintf_s(authorization, ARRAYSIZE(authorization), _TRUNCATE, L"Authorization: Bearer %ls\r\n", bearer_wide);
        if (!WinHttpAddRequestHeadersW(request, authorization, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto failed;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)(body != NULL ? body : ""), body != NULL ? (DWORD)strlen(body) : 0,
        body != NULL ? (DWORD)strlen(body) : 0, 0) || !WinHttpReceiveResponse(request, NULL)) goto failed;
    value = sizeof(*status);
    if (!WinHttpQueryInfo(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, status, &value, NULL)) goto failed;
    for (;;) {
        char *expanded;
        if (!WinHttpQueryDataAvailable(request, &available)) goto failed;
        if (available == 0) break;
        expanded = response == NULL
            ? (char *)HeapAlloc(GetProcessHeap(), 0, used + available + 1)
            : (char *)HeapReAlloc(GetProcessHeap(), 0, response, used + available + 1);
        if (expanded == NULL) goto failed;
        response = expanded;
        if (!WinHttpReadData(request, response + used, available, &received)) goto failed;
        used += received;
        if (received == 0) break;
    }
    if (response == NULL) {
        response = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1);
        if (response == NULL) goto failed;
    } else response[used] = '\0';
    *response_body = response;
    response = NULL;
    ok = 1;
    goto cleanup;

failed:
    if (error_text != NULL && error_count > 0) {
        _snwprintf_s(error_text, error_count, _TRUNCATE, L"网络请求失败（Windows 错误 %lu）。", GetLastError());
    }
cleanup:
    if (response != NULL) HeapFree(GetProcessHeap(), 0, response);
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    return ok;
}

static void api_free_body(char *body) {
    if (body != NULL) HeapFree(GetProcessHeap(), 0, body);
}

static int api_login_and_join(launch_context *context, wchar_t *error_text, size_t error_count) {
    char username[768], password[768], escaped_username[1536], escaped_password[1536];
    char request_body[3328], *response = NULL, token[2048], relay[512], community[128], ip[64];
    wchar_t join_endpoint[64];
    wchar_t local_error[256];
    DWORD status = 0;
    int relay_port = 0;
    if (!wide_to_utf8(context->username, username, sizeof(username)) || !wide_to_utf8(context->password, password, sizeof(password)) ||
        !json_escape(username, escaped_username, sizeof(escaped_username)) || !json_escape(password, escaped_password, sizeof(escaped_password))) {
        wcsncpy_s(error_text, error_count, L"账号或密码包含无法处理的字符。", _TRUNCATE);
        return 0;
    }
    _snprintf_s(request_body, sizeof(request_body), _TRUNCATE, "{\"username\":\"%s\",\"password\":\"%s\"}", escaped_username, escaped_password);
    if (!api_request(context->api_url, L"/auth/login", L"POST", request_body, NULL, &status, &response, local_error, ARRAYSIZE(local_error))) {
        wcsncpy_s(error_text, error_count, local_error, _TRUNCATE);
        return 0;
    }
    if (status != 200 || !json_string_value(response, "token", token, sizeof(token))) {
        char api_error[512] = "";
        if (json_string_value(response, "error", api_error, sizeof(api_error))) {
            wchar_t wide_error[512];
            MultiByteToWideChar(CP_UTF8, 0, api_error, -1, wide_error, ARRAYSIZE(wide_error));
            wcsncpy_s(error_text, error_count, wide_error, _TRUNCATE);
        } else wcsncpy_s(error_text, error_count, L"账号登录失败，请检查账号密码或平台地址。", _TRUNCATE);
        api_free_body(response);
        return 0;
    }
    api_free_body(response);
    response = NULL;
    _snwprintf_s(join_endpoint, ARRAYSIZE(join_endpoint), _TRUNCATE, L"/notap/rooms/%d/join", context->room_id);
    if (!api_request(context->api_url, join_endpoint, L"POST", "{}", token, &status, &response, local_error, ARRAYSIZE(local_error))) {
        wcsncpy_s(error_text, error_count, local_error, _TRUNCATE);
        return 0;
    }
    if (status != 200 || !json_string_value(response, "relay_host", relay, sizeof(relay)) ||
        !json_integer_value(response, "relay_port", &relay_port) || !json_string_value(response, "virtual_ip", ip, sizeof(ip)) ||
        !json_string_value(response, "community", community, sizeof(community)) || !json_string_value(response, "relay_token", token, sizeof(token))) {
        char api_error[512] = "";
        if (json_string_value(response, "error", api_error, sizeof(api_error))) {
            wchar_t wide_error[512];
            MultiByteToWideChar(CP_UTF8, 0, api_error, -1, wide_error, ARRAYSIZE(wide_error));
            wcsncpy_s(error_text, error_count, wide_error, _TRUNCATE);
        } else wcsncpy_s(error_text, error_count, L"进入无网卡房间失败，中继参数不完整。", _TRUNCATE);
        api_free_body(response);
        return 0;
    }
    api_free_body(response);
    _snwprintf_s(context->relay, ARRAYSIZE(context->relay), _TRUNCATE, L"%hs:%d", relay, relay_port);
    MultiByteToWideChar(CP_UTF8, 0, community, -1, context->room, ARRAYSIZE(context->room));
    MultiByteToWideChar(CP_UTF8, 0, ip, -1, context->logical_ip, ARRAYSIZE(context->logical_ip));
    MultiByteToWideChar(CP_UTF8, 0, token, -1, context->token, ARRAYSIZE(context->token));
    return valid_room(context->room) && valid_ipv4(context->logical_ip) && valid_token(context->token);
}

static void set_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static int file_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void containing_directory(wchar_t *path) {
    wchar_t *separator = wcsrchr(path, L'\\');
    if (separator == NULL) separator = wcsrchr(path, L'/');
    if (separator != NULL) *separator = L'\0';
}

static int sibling_path(const wchar_t *name, wchar_t *result, size_t count) {
    wchar_t directory[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, directory, ARRAYSIZE(directory));
    if (length == 0 || length >= ARRAYSIZE(directory)) return 0;
    containing_directory(directory);
    return _snwprintf_s(result, count, _TRUNCATE, L"%ls\\%ls", directory, name) > 0 && file_exists(result);
}

static int desktop_directory(wchar_t *result, size_t count) {
    wchar_t desktop[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desktop) != S_OK) return 0;
    return wcsncpy_s(result, count, desktop, _TRUNCATE) == 0;
}

static int make_log_path(const wchar_t *role, wchar_t *result, size_t count) {
    wchar_t desktop[MAX_PATH];
    wchar_t computer[64] = L"PC";
    DWORD computer_length = ARRAYSIZE(computer);
    SYSTEMTIME now;
    if (!desktop_directory(desktop, ARRAYSIZE(desktop))) return 0;
    GetComputerNameW(computer, &computer_length);
    GetLocalTime(&now);
    return _snwprintf_s(result, count, _TRUNCATE,
        L"%ls\\WEL-NoTap-%ls-%ls-%04u%02u%02u-%02u%02u%02u.jsonl",
        desktop, role, computer, now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond) > 0;
}

static wchar_t *quoted(const wchar_t *value) {
    size_t length = wcslen(value);
    wchar_t *result = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (length + 3) * sizeof(wchar_t));
    if (result == NULL) return NULL;
    _snwprintf_s(result, length + 3, _TRUNCATE, L"\"%ls\"", value);
    return result;
}

static wchar_t *game_directory(const wchar_t *game_path) {
    wchar_t *directory = _wcsdup(game_path);
    if (directory == NULL) return NULL;
    containing_directory(directory);
    if (directory[0] == L'\0') {
        free(directory);
        return NULL;
    }
    return directory;
}

static int valid_ipv4(const wchar_t *value) {
    unsigned int a, b, c, d;
    wchar_t tail;
    int count = swscanf_s(value, L"%u.%u.%u.%u%c", &a, &b, &c, &d, &tail, 1);
    return count == 4 && a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

static int valid_room(const wchar_t *value) {
    size_t index;
    size_t length = wcslen(value);
    if (length == 0 || length >= 32) return 0;
    for (index = 0; index < length; ++index) {
        wchar_t character = value[index];
        if (!((character >= L'a' && character <= L'z') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= L'0' && character <= L'9') || character == L'-' || character == L'_')) return 0;
    }
    return 1;
}

static int valid_token(const wchar_t *value) {
    size_t index;
    size_t length = wcslen(value);
    if (length < 8 || length >= 128) return 0;
    for (index = 0; index < length; ++index) {
        if (value[index] < 33 || value[index] > 126) return 0;
    }
    return 1;
}

static int inject_hook(HANDLE process, const wchar_t *hook_path, DWORD *error) {
    SIZE_T path_size = (wcslen(hook_path) + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, NULL, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HMODULE kernel32 = GetModuleHandleW(L"Kernel32.dll");
    FARPROC load_library;
    HANDLE thread;
    DWORD module_handle = 0;
    DWORD wait_result;

    if (remote_path == NULL || kernel32 == NULL) {
        *error = GetLastError();
        return 0;
    }
    if (!WriteProcessMemory(process, remote_path, hook_path, path_size, NULL)) {
        *error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    load_library = GetProcAddress(kernel32, "LoadLibraryW");
    if (load_library == NULL) {
        *error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library,
        remote_path, 0, NULL);
    if (thread == NULL) {
        *error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    wait_result = WaitForSingleObject(thread, 10000);
    if (wait_result == WAIT_OBJECT_0) GetExitCodeThread(thread, &module_handle);
    else *error = ERROR_TIMEOUT;
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    return module_handle != 0;
}

static DWORD WINAPI launch_thread(LPVOID parameter) {
    launch_context *context = (launch_context *)parameter;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t ready_name[96];
    wchar_t *command_line = NULL;
    wchar_t *working_directory = NULL;
    HANDLE ready_event = NULL;
    DWORD error = ERROR_SUCCESS;
    int child_started = 0;

    _snwprintf_s(ready_name, ARRAYSIZE(ready_name), _TRUNCATE, L"Local\\WELNoTapReady-%lu-%lu",
        (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    ready_event = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (ready_event == NULL) {
        error = GetLastError();
        goto failed;
    }
    SetEnvironmentVariableW(L"WEL_NOTAP_RELAY", context->relay);
    SetEnvironmentVariableW(L"WEL_NOTAP_ROOM", context->room);
    SetEnvironmentVariableW(L"WEL_NOTAP_LOGICAL_IP", context->logical_ip);
    SetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", context->token);
    SetEnvironmentVariableW(L"WEL_NOTAP_LOG_PATH", context->log_path);
    SetEnvironmentVariableW(L"WEL_NOTAP_READY_EVENT", ready_name);
    command_line = quoted(context->game_path);
    working_directory = game_directory(context->game_path);
    if (command_line == NULL || working_directory == NULL) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto failed;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(context->game_path, command_line, NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_DEFAULT_ERROR_MODE, NULL, working_directory, &startup, &process)) {
        error = GetLastError();
        goto failed;
    }
    SetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", NULL);
    child_started = 1;
    if (!inject_hook(process.hProcess, context->hook_path, &error)) goto failed_process;
    if (WaitForSingleObject(ready_event, 10000) != WAIT_OBJECT_0) {
        error = ERROR_TIMEOUT;
        goto failed_process;
    }
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        error = GetLastError();
        goto failed_process;
    }
    context->process_id = process.dwProcessId;
    context->success = 1;
    _snwprintf_s(context->status, ARRAYSIZE(context->status), _TRUNCATE,
        L"无网卡测试已启动，WE8 PID %lu。\r\n\r\n"
        L"角色：%ls\r\n逻辑 IP：%ls\r\n中继：%ls\r\n房间：%ls\r\n\r\n"
        L"请按正常流程完成建主、搜索、加入和比赛。日志：\r\n%ls\r\n\r\n"
        L"云端模式可直接测试；使用本机中继模式时请保持本窗口打开。",
        (unsigned long)context->process_id,
        context->is_host ? L"主机" : L"客机",
        context->logical_ip, context->relay, context->room, context->log_path);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    goto done;

failed_process:
    TerminateProcess(process.hProcess, error == ERROR_SUCCESS ? 1 : error);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
failed:
    if (error == ERROR_SUCCESS) error = GetLastError();
    _snwprintf_s(context->status, ARRAYSIZE(context->status), _TRUNCATE,
        L"无网卡版本启动失败（Windows 错误 %lu）。\r\n"
        L"请确认 WE8.exe、welnpt.dll、房间地址和逻辑 IP 正确，并确认两端使用同一个房间名。",
        (unsigned long)error);
done:
    SetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", NULL);
    if (ready_event != NULL) CloseHandle(ready_event);
    if (command_line != NULL) HeapFree(GetProcessHeap(), 0, command_line);
    if (working_directory != NULL) free(working_directory);
    if (!child_started && context->log_path[0] != L'\0') DeleteFileW(context->log_path);
    PostMessageW(context->window, WM_LAUNCH_COMPLETE, 0, (LPARAM)context);
    return 0;
}

static DWORD WINAPI api_launch_thread(LPVOID parameter) {
    launch_context *context = (launch_context *)parameter;
    wchar_t error_text[512] = L"";
    if (!api_login_and_join(context, error_text, ARRAYSIZE(error_text))) {
        _snwprintf_s(context->status, ARRAYSIZE(context->status), _TRUNCATE,
            L"无网卡房间连接失败。\r\n\r\n%ls\r\n\r\n"
            L"请确认账号密码正确，并确认 Go 后端已配置 No-TAP 中继。",
            error_text[0] != L'\0' ? error_text : L"服务端没有返回有效的房间凭据。");
        PostMessageW(context->window, WM_LAUNCH_COMPLETE, 0, (LPARAM)context);
        return 0;
    }
    return launch_thread(context);
}

static void show_status(const wchar_t *message) {
    SetWindowTextW(g_status, message);
}

static void choose_game(void) {
    OPENFILENAMEW dialog;
    wchar_t path[MAX_PATH] = L"";
    GetWindowTextW(g_game_path, path, ARRAYSIZE(path));
    ZeroMemory(&dialog, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"WE8 游戏程序 (WE8.exe)\0WE8.exe\0Windows 程序 (*.exe)\0*.exe\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) SetWindowTextW(g_game_path, path);
}

static void start_test(void) {
    launch_context *context;
    wchar_t role[64];
    wchar_t api_url[ARRAYSIZE(context->api_url)];
    wchar_t username[ARRAYSIZE(context->username)];
    wchar_t password[ARRAYSIZE(context->password)];
    LRESULT room_id;
    HANDLE thread;
    int is_host;

    context = (launch_context *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context));
    if (context == NULL) return;
    context->window = g_window;
    GetWindowTextW(g_game_path, context->game_path, ARRAYSIZE(context->game_path));
    GetWindowTextW(g_api_url, api_url, ARRAYSIZE(api_url));
    GetWindowTextW(g_username, username, ARRAYSIZE(username));
    GetWindowTextW(g_password, password, ARRAYSIZE(password));
    GetWindowTextW(g_role, role, ARRAYSIZE(role));
    is_host = wcsstr(role, L"主机") != NULL;
    context->is_host = is_host;
    room_id = SendMessageW(g_room, CB_GETITEMDATA, SendMessageW(g_room, CB_GETCURSEL, 0, 0), 0);
    if (!file_exists(context->game_path)) {
        show_status(L"请先选择有效的 WE8.exe。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    if (!sibling_path(L"welnpt.dll", context->hook_path, ARRAYSIZE(context->hook_path))) {
        show_status(L"没有找到 welnpt.dll。请完整解压无网卡测试包。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    if (api_url[0] == L'\0' || username[0] == L'\0' || password[0] == L'\0' || room_id < 1 || room_id > 3) {
        show_status(L"请填写 API 地址、账号、密码，并选择无网卡房间。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    wcsncpy_s(context->api_url, ARRAYSIZE(context->api_url), api_url, _TRUNCATE);
    wcsncpy_s(context->username, ARRAYSIZE(context->username), username, _TRUNCATE);
    wcsncpy_s(context->password, ARRAYSIZE(context->password), password, _TRUNCATE);
    context->room_id = (int)room_id;
    if (!make_log_path(is_host ? L"Host-A" : L"Client-B", context->log_path, ARRAYSIZE(context->log_path))) {
        show_status(L"无法创建桌面日志路径。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    EnableWindow(g_start, FALSE);
    show_status(L"正在登录账号并申请无网卡房间，请稍候...");
    thread = CreateThread(NULL, 0, api_launch_thread, context, 0, NULL);
    if (thread == NULL) {
        EnableWindow(g_start, TRUE);
        show_status(L"无法创建启动线程。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    CloseHandle(thread);
}

static void create_controls(HWND window) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND control;
    control = CreateWindowExW(0, L"STATIC", L"WEL 无虚拟网卡联机测试", WS_CHILD | WS_VISIBLE,
        24, 18, 620, 30, window, NULL, NULL, NULL);
    set_font(control, font);
    control = CreateWindowExW(0, L"STATIC", L"WE8.exe", WS_CHILD | WS_VISIBLE,
        24, 58, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_game_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        24, 82, 515, 27, window, (HMENU)ID_GAME_PATH, NULL, NULL);
    set_font(g_game_path, font);
    control = CreateWindowExW(0, L"BUTTON", L"选择游戏", WS_CHILD | WS_VISIBLE,
        548, 81, 100, 29, window, (HMENU)ID_CHOOSE_GAME, NULL, NULL);
    set_font(control, font);
    control = CreateWindowExW(0, L"STATIC", L"Go API 地址", WS_CHILD | WS_VISIBLE,
        24, 124, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_api_url = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", WELNPT_DEFAULT_API_URL,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 24, 148, 440, 27, window, NULL, NULL, NULL);
    set_font(g_api_url, font);
    control = CreateWindowExW(0, L"STATIC", L"账号", WS_CHILD | WS_VISIBLE,
        24, 190, 55, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_username = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 80, 186, 210, 27, window, NULL, NULL, NULL);
    set_font(g_username, font);
    control = CreateWindowExW(0, L"STATIC", L"密码", WS_CHILD | WS_VISIBLE,
        310, 190, 55, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_password = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD, 366, 186, 210, 27, window, NULL, NULL, NULL);
    set_font(g_password, font);
    control = CreateWindowExW(0, L"STATIC", L"本机角色", WS_CHILD | WS_VISIBLE,
        24, 232, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_role = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        24, 256, 220, 100, window, (HMENU)ID_ROLE, NULL, NULL);
    set_font(g_role, font);
    SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)L"主机");
    SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)L"客机");
    SendMessageW(g_role, CB_SETCURSEL, 0, 0);
    control = CreateWindowExW(0, L"STATIC", L"无网卡房间", WS_CHILD | WS_VISIBLE,
        280, 232, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_room = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        280, 256, 300, 110, window, (HMENU)ID_ROOM, NULL, NULL);
    set_font(g_room, font);
    SendMessageW(g_room, CB_ADDSTRING, 0, (LPARAM)L"01 - 10.122.1.0/24");
    SendMessageW(g_room, CB_SETITEMDATA, 0, 1);
    SendMessageW(g_room, CB_ADDSTRING, 0, (LPARAM)L"02 - 10.122.2.0/24");
    SendMessageW(g_room, CB_SETITEMDATA, 1, 2);
    SendMessageW(g_room, CB_ADDSTRING, 0, (LPARAM)L"03 - 10.122.3.0/24");
    SendMessageW(g_room, CB_SETITEMDATA, 2, 3);
    SendMessageW(g_room, CB_SETCURSEL, 0, 0);
    g_start = CreateWindowExW(0, L"BUTTON", L"登录并启动 WE8", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        24, 386, 220, 34, window, (HMENU)ID_START, NULL, NULL);
    set_font(g_start, font);
    g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"登录后选择同一个无网卡房间。服务端会为主机和客机分别分配 10.122.x.x 逻辑地址。\r\n"
        L"此版本不安装 TAP/n2n，不修改系统 IP 或路由；中继参数由 Go 后端下发。\r\n"
        L"请在两台电脑上分别使用自己的 Laravel 账号登录。",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        24, 440, 724, 145, window, (HMENU)ID_STATUS, NULL, NULL);
    set_font(g_status, font);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_CREATE:
            g_window = window;
            create_controls(window);
            return 0;
        case WM_COMMAND:
            if (LOWORD(w_param) == ID_CHOOSE_GAME) {
                choose_game();
                return 0;
            }
            if (LOWORD(w_param) == ID_START) {
                start_test();
                return 0;
            }
            break;
        case WM_LAUNCH_COMPLETE: {
            launch_context *context = (launch_context *)l_param;
            show_status(context->status);
            if (context->success) EnableWindow(g_start, FALSE);
            else EnableWindow(g_start, TRUE);
            SecureZeroMemory(context->token, sizeof(context->token));
            SecureZeroMemory(context->password, sizeof(context->password));
            HeapFree(GetProcessHeap(), 0, context);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show_command) {
    WNDCLASSEXW window_class;
    MSG message;
    HWND window;
    (void)previous;
    (void)command_line;
    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"WELNoTapConnectWindow";
    if (!RegisterClassExW(&window_class)) return 1;
    window = CreateWindowExW(0, window_class.lpszClassName, L"WEL 无虚拟网卡联机测试",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 650, NULL, NULL, instance, NULL);
    if (window == NULL) return 2;
    ShowWindow(window, show_command);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}
