#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define ID_GAME_PATH 1001
#define ID_CHOOSE_GAME 1002
#define ID_ROLE 1003
#define ID_START 1004
#define ID_OPEN_RESULTS 1005
#define ID_STATUS 1006
#define WM_LAUNCH_COMPLETE (WM_APP + 1)

typedef struct {
    HWND window;
    wchar_t game_path[MAX_PATH];
    wchar_t hook_path[MAX_PATH];
    wchar_t trace_path[MAX_PATH];
    wchar_t status[768];
    DWORD process_id;
    int success;
} launch_context;

static HWND g_window;
static HWND g_game_path;
static HWND g_role;
static HWND g_start;
static HWND g_status;
static wchar_t g_last_trace[MAX_PATH];

static void set_control_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static void show_status(const wchar_t *message) {
    SetWindowTextW(g_status, message);
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

static int sibling_hook_path(wchar_t *result, size_t count) {
    wchar_t directory[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, directory, ARRAYSIZE(directory));
    if (length == 0 || length >= ARRAYSIZE(directory)) return 0;
    containing_directory(directory);
    return _snwprintf_s(result, count, _TRUNCATE, L"%ls\\welnpttrace.dll", directory) > 0 &&
        file_exists(result);
}

static int desktop_directory(wchar_t *result, size_t count) {
    wchar_t desktop[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desktop) != S_OK) return 0;
    return wcsncpy_s(result, count, desktop, _TRUNCATE) == 0;
}

static int make_trace_path(const wchar_t *role, wchar_t *result, size_t count) {
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

static wchar_t *quoted_command_line(const wchar_t *application) {
    size_t length = wcslen(application);
    wchar_t *command = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (length + 3) * sizeof(wchar_t));
    if (command == NULL) return NULL;
    _snwprintf_s(command, length + 3, _TRUNCATE, L"\"%ls\"", application);
    return command;
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

static int inject_hook(HANDLE process, const wchar_t *hook_path, DWORD *error) {
    SIZE_T path_size = (wcslen(hook_path) + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, NULL, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HMODULE kernel32 = GetModuleHandleW(L"Kernel32.dll");
    FARPROC load_library;
    HANDLE thread;
    DWORD module_handle = 0;
    DWORD waited;

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
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library, remote_path, 0, NULL);
    if (thread == NULL) {
        *error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    waited = WaitForSingleObject(thread, 10000);
    if (waited == WAIT_OBJECT_0) GetExitCodeThread(thread, &module_handle);
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
    SetEnvironmentVariableW(L"WEL_NOTAP_TRACE_PATH", context->trace_path);
    SetEnvironmentVariableW(L"WEL_NOTAP_READY_EVENT", ready_name);
    command_line = quoted_command_line(context->game_path);
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
    child_started = 1;
    if (!inject_hook(process.hProcess, context->hook_path, &error)) goto failed_process;
    if (WaitForSingleObject(ready_event, 5000) != WAIT_OBJECT_0) {
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
        L"观测已开始，WE8 已启动（PID %lu）。\r\n\r\n"
        L"请按正常流程完成建主、搜索、连接和联机。测试结束后退出游戏，结果文件会保存在桌面：\r\n%ls",
        (unsigned long)context->process_id, context->trace_path);
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
        L"启动观测失败（Windows 错误 %lu）。\r\n"
        L"请确认 WE8.exe 路径正确、welnpttrace.dll 与本工具在同一目录，并关闭安全软件拦截后重试。",
        (unsigned long)error);
done:
    if (ready_event != NULL) CloseHandle(ready_event);
    if (command_line != NULL) HeapFree(GetProcessHeap(), 0, command_line);
    if (working_directory != NULL) free(working_directory);
    if (!child_started && context->trace_path[0] != L'\0') DeleteFileW(context->trace_path);
    PostMessageW(context->window, WM_LAUNCH_COMPLETE, 0, (LPARAM)context);
    return 0;
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

static void start_observation(void) {
    launch_context *context;
    wchar_t role[32];
    HANDLE thread;

    context = (launch_context *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context));
    if (context == NULL) return;
    context->window = g_window;
    GetWindowTextW(g_game_path, context->game_path, ARRAYSIZE(context->game_path));
    if (!file_exists(context->game_path)) {
        show_status(L"请先选择有效的 WE8.exe。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    if (!sibling_hook_path(context->hook_path, ARRAYSIZE(context->hook_path))) {
        show_status(L"没有找到 welnpttrace.dll。请完整解压构建产物，并保持 DLL 与本工具在同一目录。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    GetWindowTextW(g_role, role, ARRAYSIZE(role));
    if (!make_trace_path(wcsstr(role, L"Client") != NULL ? L"Client-B" : L"Host-A",
        context->trace_path, ARRAYSIZE(context->trace_path))) {
        show_status(L"无法创建桌面结果文件路径。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    EnableWindow(g_start, FALSE);
    show_status(L"正在启动 WE8 并加载观测模块，请稍候...");
    thread = CreateThread(NULL, 0, launch_thread, context, 0, NULL);
    if (thread == NULL) {
        EnableWindow(g_start, TRUE);
        show_status(L"无法创建观测线程。 ");
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    CloseHandle(thread);
}

static void open_results(void) {
    wchar_t directory[MAX_PATH];
    if (g_last_trace[0] != L'\0') {
        wcsncpy_s(directory, ARRAYSIZE(directory), g_last_trace, _TRUNCATE);
        containing_directory(directory);
    } else if (!desktop_directory(directory, ARRAYSIZE(directory))) {
        return;
    }
    ShellExecuteW(g_window, L"open", directory, NULL, NULL, SW_SHOWNORMAL);
}

static void create_controls(HWND window) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND control;
    control = CreateWindowExW(0, L"STATIC", L"WEL 无虚拟网卡观测", WS_CHILD | WS_VISIBLE,
        24, 18, 560, 30, window, NULL, NULL, NULL);
    set_control_font(control, font);
    control = CreateWindowExW(0, L"STATIC", L"游戏路径", WS_CHILD | WS_VISIBLE,
        24, 62, 100, 22, window, NULL, NULL, NULL);
    set_control_font(control, font);
    g_game_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        24, 86, 490, 27, window, (HMENU)ID_GAME_PATH, NULL, NULL);
    set_control_font(g_game_path, font);
    control = CreateWindowExW(0, L"BUTTON", L"选择 WE8", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        524, 85, 100, 29, window, (HMENU)ID_CHOOSE_GAME, NULL, NULL);
    set_control_font(control, font);
    control = CreateWindowExW(0, L"STATIC", L"本机角色", WS_CHILD | WS_VISIBLE,
        24, 130, 100, 22, window, NULL, NULL, NULL);
    set_control_font(control, font);
    g_role = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        24, 153, 200, 120, window, (HMENU)ID_ROLE, NULL, NULL);
    set_control_font(g_role, font);
    SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)L"Host-A（建主机）");
    SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)L"Client-B（搜索加入）");
    SendMessageW(g_role, CB_SETCURSEL, 0, 0);
    g_start = CreateWindowExW(0, L"BUTTON", L"开始观测并启动 WE8", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        240, 151, 190, 34, window, (HMENU)ID_START, NULL, NULL);
    set_control_font(g_start, font);
    control = CreateWindowExW(0, L"BUTTON", L"打开结果目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        442, 151, 150, 34, window, (HMENU)ID_OPEN_RESULTS, NULL, NULL);
    set_control_font(control, font);
    g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"选择本机角色和 WE8.exe 后开始观测。主客机都需要运行本工具。",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        24, 205, 600, 120, window, (HMENU)ID_STATUS, NULL, NULL);
    set_control_font(g_status, font);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_CREATE:
            g_window = window;
            create_controls(window);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w_param)) {
                case ID_CHOOSE_GAME: choose_game(); return 0;
                case ID_START: start_observation(); return 0;
                case ID_OPEN_RESULTS: open_results(); return 0;
            }
            break;
        case WM_LAUNCH_COMPLETE: {
            launch_context *context = (launch_context *)l_param;
            show_status(context->status);
            if (context->success) wcsncpy_s(g_last_trace, ARRAYSIZE(g_last_trace), context->trace_path, _TRUNCATE);
            EnableWindow(g_start, TRUE);
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
    window_class.lpszClassName = L"WELNoTapObservationWindow";
    if (!RegisterClassExW(&window_class)) return 1;
    window = CreateWindowExW(0, window_class.lpszClassName, L"WEL 无虚拟网卡观测工具",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 670, 380, NULL, NULL, instance, NULL);
    if (window == NULL) return 2;
    ShowWindow(window, show_command);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}
