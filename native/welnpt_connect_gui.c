#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define ID_GAME_PATH 1101
#define ID_RELAY 1102
#define ID_ROOM 1103
#define ID_LOGICAL_IP 1104
#define ID_ROLE 1105
#define ID_TOKEN 1106
#define ID_CHOOSE_GAME 1107
#define ID_START 1108
#define ID_STATUS 1109
#define ID_COPY_TOKEN 1110
#define WM_LAUNCH_COMPLETE (WM_APP + 1)

/* Temporary two-player test credential; replace before any public release. */
#define WELNPT_TEST_TOKEN L"WEL-P2-TEST-ONLY-20260814"

typedef struct launch_context {
    HWND window;
    wchar_t game_path[MAX_PATH];
    wchar_t hook_path[MAX_PATH];
    wchar_t log_path[MAX_PATH];
    wchar_t relay[MAX_PATH];
    wchar_t room[64];
    wchar_t logical_ip[64];
    wchar_t token[128];
    wchar_t status[1024];
    DWORD process_id;
    int is_host;
    int success;
} launch_context;

static HWND g_window;
static HWND g_game_path;
static HWND g_relay;
static HWND g_room;
static HWND g_logical_ip;
static HWND g_token;
static HWND g_role;
static HWND g_start;
static HWND g_status;
static HANDLE g_relay_job;

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

static int parse_port(const wchar_t *endpoint) {
    const wchar_t *separator = wcsrchr(endpoint, L':');
    unsigned long port;
    if (separator == NULL || separator[1] == L'\0') return 0;
    port = wcstoul(separator + 1, NULL, 10);
    return port > 0 && port <= 65535 ? (int)port : 0;
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

static int start_local_relay(const wchar_t *endpoint, wchar_t *error_text, size_t error_count) {
    wchar_t relay_path[MAX_PATH];
    wchar_t command_line[MAX_PATH + 32];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HANDLE job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    int port = parse_port(endpoint);

    if (port == 0) {
        wcsncpy_s(error_text, error_count, L"中继地址端口无效。", _TRUNCATE);
        return 0;
    }
    if (g_relay_job != NULL) return 1;
    if (!sibling_path(L"welnptrelay.exe", relay_path, ARRAYSIZE(relay_path))) {
        wcsncpy_s(error_text, error_count, L"没有找到 welnptrelay.exe。请把中继程序和 GUI 放在同一目录。", _TRUNCATE);
        return 0;
    }
    _snwprintf_s(command_line, ARRAYSIZE(command_line), _TRUNCATE, L"\"%ls\" %d", relay_path, port);
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(NULL, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
        NULL, NULL, &startup, &process)) {
        _snwprintf_s(error_text, error_count, _TRUNCATE, L"启动本机中继失败（Windows 错误 %lu）。", GetLastError());
        return 0;
    }
    job = CreateJobObjectW(NULL, NULL);
    if (job != NULL) {
        ZeroMemory(&limits, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, process.hProcess);
    }
    CloseHandle(process.hThread);
    g_relay_job = job;
    if (WaitForSingleObject(process.hProcess, 300) == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hProcess);
        if (g_relay_job != NULL) {
            CloseHandle(g_relay_job);
            g_relay_job = NULL;
        }
        _snwprintf_s(error_text, error_count, _TRUNCATE,
            L"本机中继启动后退出（代码 %lu），端口可能已被占用。", exit_code);
        return 0;
    }
    CloseHandle(process.hProcess);
    return 1;
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

static void show_status(const wchar_t *message) {
    SetWindowTextW(g_status, message);
}

static void copy_token(void) {
    int length = GetWindowTextLengthW(g_token);
    HGLOBAL memory;
    wchar_t *text;
    if (length <= 0) {
        show_status(L"测试令牌为空，无法复制。 ");
        return;
    }
    memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(length + 1) * sizeof(wchar_t));
    if (memory == NULL) {
        show_status(L"复制测试令牌失败。 ");
        return;
    }
    text = (wchar_t *)GlobalLock(memory);
    if (text == NULL) {
        GlobalFree(memory);
        show_status(L"复制测试令牌失败。 ");
        return;
    }
    GetWindowTextW(g_token, text, length + 1);
    GlobalUnlock(memory);
    if (!OpenClipboard(g_window)) {
        GlobalFree(memory);
        show_status(L"无法打开剪贴板，请重试。 ");
        return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
        GlobalFree(memory);
        CloseClipboard();
        show_status(L"复制测试令牌失败。 ");
        return;
    }
    CloseClipboard();
    show_status(L"测试令牌已复制。主机和客机使用同一个令牌即可。 ");
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
    wchar_t relay[ARRAYSIZE(context->relay)];
    wchar_t room[ARRAYSIZE(context->room)];
    wchar_t logical_ip[ARRAYSIZE(context->logical_ip)];
    wchar_t token[ARRAYSIZE(context->token)];
    wchar_t error_text[256];
    HANDLE thread;
    int is_host;
    int use_local_relay;

    context = (launch_context *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context));
    if (context == NULL) return;
    context->window = g_window;
    GetWindowTextW(g_game_path, context->game_path, ARRAYSIZE(context->game_path));
    GetWindowTextW(g_relay, relay, ARRAYSIZE(relay));
    GetWindowTextW(g_room, room, ARRAYSIZE(room));
    GetWindowTextW(g_logical_ip, logical_ip, ARRAYSIZE(logical_ip));
    GetWindowTextW(g_token, token, ARRAYSIZE(token));
    GetWindowTextW(g_role, role, ARRAYSIZE(role));
    is_host = wcsstr(role, L"主机") != NULL;
    use_local_relay = wcsstr(role, L"本机中继") != NULL;
    context->is_host = is_host;
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
    if (wcslen(relay) >= ARRAYSIZE(context->relay) || wcslen(room) >= ARRAYSIZE(context->room) ||
        wcslen(logical_ip) >= ARRAYSIZE(context->logical_ip) || parse_port(relay) == 0 ||
        !valid_room(room) || !valid_ipv4(logical_ip) || !valid_token(token)) {
        show_status(L"请检查中继地址、房间名、逻辑 IP 和测试令牌。令牌需要 8-127 个 ASCII 字符。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    if (!is_host && (logical_ip[0] == L'\0')) {
        show_status(L"客机必须填写与主机不同的逻辑 IP。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    wcsncpy_s(context->relay, ARRAYSIZE(context->relay), relay, _TRUNCATE);
    wcsncpy_s(context->room, ARRAYSIZE(context->room), room, _TRUNCATE);
    wcsncpy_s(context->logical_ip, ARRAYSIZE(context->logical_ip), logical_ip, _TRUNCATE);
    wcsncpy_s(context->token, ARRAYSIZE(context->token), token, _TRUNCATE);
    if (!make_log_path(is_host ? L"Host-A" : L"Client-B", context->log_path, ARRAYSIZE(context->log_path))) {
        show_status(L"无法创建桌面日志路径。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    if (use_local_relay) SetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", token);
    if (use_local_relay && !start_local_relay(relay, error_text, ARRAYSIZE(error_text))) {
        SetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", NULL);
        show_status(error_text);
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    EnableWindow(g_start, FALSE);
    show_status(L"正在启动 WE8 并加载无网卡 Socket Hook，请稍候...");
    thread = CreateThread(NULL, 0, launch_thread, context, 0, NULL);
    if (thread == NULL) {
        SetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", NULL);
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
    control = CreateWindowExW(0, L"STATIC", L"本机角色", WS_CHILD | WS_VISIBLE,
        24, 124, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_role = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        24, 148, 190, 120, window, (HMENU)ID_ROLE, NULL, NULL);
    set_font(g_role, font);
    SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)L"主机（云端中继）");
    SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)L"客机（云端中继）");
    SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)L"主机（本机中继）");
    SendMessageW(g_role, CB_SETCURSEL, 0, 0);
    control = CreateWindowExW(0, L"STATIC", L"中继地址", WS_CHILD | WS_VISIBLE,
        240, 124, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_relay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8.155.145.132:22333",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 240, 148, 408, 27, window, (HMENU)ID_RELAY, NULL, NULL);
    set_font(g_relay, font);
    control = CreateWindowExW(0, L"STATIC", L"房间名", WS_CHILD | WS_VISIBLE,
        24, 190, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_room = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"wel-test-room", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        24, 214, 190, 27, window, (HMENU)ID_ROOM, NULL, NULL);
    set_font(g_room, font);
    control = CreateWindowExW(0, L"STATIC", L"本机逻辑 IP", WS_CHILD | WS_VISIBLE,
        240, 190, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_logical_ip = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10.250.1.1",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 240, 214, 180, 27, window, (HMENU)ID_LOGICAL_IP, NULL, NULL);
    set_font(g_logical_ip, font);
    control = CreateWindowExW(0, L"STATIC", L"测试令牌", WS_CHILD | WS_VISIBLE,
        24, 256, 110, 22, window, NULL, NULL, NULL);
    set_font(control, font);
    g_token = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", WELNPT_TEST_TOKEN,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
        24, 280, 420, 27, window, (HMENU)ID_TOKEN, NULL, NULL);
    set_font(g_token, font);
    control = CreateWindowExW(0, L"BUTTON", L"复制令牌", WS_CHILD | WS_VISIBLE,
        456, 276, 100, 34, window, (HMENU)ID_COPY_TOKEN, NULL, NULL);
    set_font(control, font);
    g_start = CreateWindowExW(0, L"BUTTON", L"启动无网卡联机", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        566, 276, 182, 34, window, (HMENU)ID_START, NULL, NULL);
    set_font(g_start, font);
    g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"云端测试：两端保持 8.155.145.132:22333，房间名和测试令牌相同。\r\n"
        L"主机逻辑 IP 使用 10.250.1.1，客机使用 10.250.1.2。\r\n"
        L"此版本不安装 TAP/n2n，不修改系统 IP 或路由。",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        24, 334, 724, 145, window, (HMENU)ID_STATUS, NULL, NULL);
    set_font(g_status, font);
}

static void apply_role_defaults(void) {
    int selection = (int)SendMessageW(g_role, CB_GETCURSEL, 0, 0);
    if (selection == 1) {
        SetWindowTextW(g_relay, L"8.155.145.132:22333");
        SetWindowTextW(g_logical_ip, L"10.250.1.2");
    } else if (selection == 2) {
        SetWindowTextW(g_relay, L"127.0.0.1:22333");
        SetWindowTextW(g_logical_ip, L"10.250.1.1");
    } else {
        SetWindowTextW(g_relay, L"8.155.145.132:22333");
        SetWindowTextW(g_logical_ip, L"10.250.1.1");
    }
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
            if (LOWORD(w_param) == ID_COPY_TOKEN) {
                copy_token();
                return 0;
            }
            if (LOWORD(w_param) == ID_ROLE && HIWORD(w_param) == CBN_SELCHANGE) {
                apply_role_defaults();
                return 0;
            }
            break;
        case WM_LAUNCH_COMPLETE: {
            launch_context *context = (launch_context *)l_param;
            show_status(context->status);
            if (context->success) EnableWindow(g_start, FALSE);
            else EnableWindow(g_start, TRUE);
            SecureZeroMemory(context->token, sizeof(context->token));
            HeapFree(GetProcessHeap(), 0, context);
            return 0;
        }
        case WM_DESTROY:
            if (g_relay_job != NULL) {
                CloseHandle(g_relay_job);
                g_relay_job = NULL;
            }
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
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 545, NULL, NULL, instance, NULL);
    if (window == NULL) return 2;
    ShowWindow(window, show_command);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}
