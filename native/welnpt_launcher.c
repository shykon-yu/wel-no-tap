#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct {
    const wchar_t *game_path;
    const wchar_t *hook_path;
    const wchar_t *relay;
    const wchar_t *room;
    const wchar_t *logical_ip;
    const wchar_t *token;
    const wchar_t *log_path;
    const wchar_t *direct_peer_ip;
    const wchar_t *direct_agent_port;
    const wchar_t *direct_hook_port;
    int self_test;
} launch_options;

static int parse_options(int argc, wchar_t **argv, launch_options *options) {
    int index;
    ZeroMemory(options, sizeof(*options));
    for (index = 1; index < argc; ++index) {
        if (wcscmp(argv[index], L"--game") == 0 && index + 1 < argc) options->game_path = argv[++index];
        else if (wcscmp(argv[index], L"--hook") == 0 && index + 1 < argc) options->hook_path = argv[++index];
        else if (wcscmp(argv[index], L"--relay") == 0 && index + 1 < argc) options->relay = argv[++index];
        else if (wcscmp(argv[index], L"--room") == 0 && index + 1 < argc) options->room = argv[++index];
        else if (wcscmp(argv[index], L"--logical-ip") == 0 && index + 1 < argc) options->logical_ip = argv[++index];
        else if (wcscmp(argv[index], L"--token") == 0 && index + 1 < argc) options->token = argv[++index];
        else if (wcscmp(argv[index], L"--log") == 0 && index + 1 < argc) options->log_path = argv[++index];
        else if (wcscmp(argv[index], L"--direct-peer-ip") == 0 && index + 1 < argc) options->direct_peer_ip = argv[++index];
        else if (wcscmp(argv[index], L"--direct-agent-port") == 0 && index + 1 < argc) options->direct_agent_port = argv[++index];
        else if (wcscmp(argv[index], L"--direct-hook-port") == 0 && index + 1 < argc) options->direct_hook_port = argv[++index];
        else if (wcscmp(argv[index], L"--self-test") == 0) options->self_test = 1;
        else return 0;
    }
    return options->self_test || (options->game_path != NULL && options->hook_path != NULL);
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
    wchar_t *separator;
    if (directory == NULL) return NULL;
    separator = wcsrchr(directory, L'\\');
    if (separator == NULL) separator = wcsrchr(directory, L'/');
    if (separator == NULL) {
        free(directory);
        return NULL;
    }
    *separator = L'\0';
    return directory;
}

static int inject_hook(HANDLE process, HANDLE primary_thread, const wchar_t *hook_path,
    DWORD *error, const char **stage, LPVOID *apc_remote_path) {
    SIZE_T path_size = (wcslen(hook_path) + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, NULL, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HMODULE kernel32 = GetModuleHandleW(L"Kernel32.dll");
    FARPROC load_library;
    HANDLE thread;
    DWORD module_handle = 0;
    DWORD wait_result;

    *error = ERROR_SUCCESS;
    *stage = "VirtualAllocEx";
    *apc_remote_path = NULL;
    if (remote_path == NULL) {
        *error = GetLastError();
        return 0;
    }
    if (kernel32 == NULL) {
        *stage = "GetModuleHandleW(Kernel32.dll)";
        *error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    *stage = "WriteProcessMemory";
    if (!WriteProcessMemory(process, remote_path, hook_path, path_size, NULL)) {
        *error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    *stage = "GetProcAddress(LoadLibraryW)";
    load_library = GetProcAddress(kernel32, "LoadLibraryW");
    if (load_library == NULL) {
        *error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    *stage = "CreateRemoteThread";
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library, remote_path, 0, NULL);
    if (thread == NULL) {
        *error = GetLastError();
        if (*error == ERROR_ACCESS_DENIED) {
            *stage = "QueueUserAPC(LoadLibraryW)";
            if (QueueUserAPC((PAPCFUNC)load_library, primary_thread, (ULONG_PTR)remote_path) != 0) {
                *apc_remote_path = remote_path;
                *error = ERROR_SUCCESS;
                return 1;
            }
            *error = GetLastError();
        }
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    *stage = "WaitForSingleObject(LoadLibraryW)";
    wait_result = WaitForSingleObject(thread, 10000);
    if (wait_result != WAIT_OBJECT_0) {
        *error = wait_result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        CloseHandle(thread);
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    *stage = "GetExitCodeThread(LoadLibraryW)";
    if (!GetExitCodeThread(thread, &module_handle)) {
        *error = GetLastError();
        CloseHandle(thread);
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    if (module_handle == 0) {
        *stage = "LoadLibraryW(welnpt.dll)";
        *error = ERROR_MOD_NOT_FOUND;
        return 0;
    }
    return 1;
}

int wmain(int argc, wchar_t **argv) {
    launch_options options;
    wchar_t game[MAX_PATH];
    wchar_t hook[MAX_PATH];
    wchar_t relay[256];
    wchar_t room[64];
    wchar_t logical_ip[64];
    wchar_t token[256];
    wchar_t log_path[MAX_PATH];
    wchar_t ready_name[96];
    wchar_t *command_line;
    wchar_t *working_directory;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HANDLE ready_event;
    DWORD injection_error;
    const char *injection_stage;
    LPVOID apc_remote_path;
    int resumed_for_apc = 0;

    if (!parse_options(argc, argv, &options)) {
        fputs("Usage: welnptgame --game <WE8.exe> --hook <welnpt.dll> --relay <host:port> --room <name> --logical-ip <ip> --token <token> --log <file>\n", stderr);
        return 2;
    }
    if (options.self_test) {
        puts("SELF-TEST OK");
        return 0;
    }
    if (GetFullPathNameW(options.game_path, ARRAYSIZE(game), game, NULL) == 0 ||
        GetFileAttributesW(game) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Game executable not found: %ls\n", options.game_path);
        return 3;
    }
    if (GetFullPathNameW(options.hook_path, ARRAYSIZE(hook), hook, NULL) == 0 ||
        GetFileAttributesW(hook) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Hook module not found: %ls\n", options.hook_path);
        return 4;
    }
    if (options.relay != NULL) wcsncpy_s(relay, ARRAYSIZE(relay), options.relay, _TRUNCATE);
    else if (GetEnvironmentVariableW(L"WEL_NOTAP_RELAY", relay, ARRAYSIZE(relay)) == 0) return 2;
    if (options.room != NULL) wcsncpy_s(room, ARRAYSIZE(room), options.room, _TRUNCATE);
    else if (GetEnvironmentVariableW(L"WEL_NOTAP_ROOM", room, ARRAYSIZE(room)) == 0) return 2;
    if (options.logical_ip != NULL) wcsncpy_s(logical_ip, ARRAYSIZE(logical_ip), options.logical_ip, _TRUNCATE);
    else if (GetEnvironmentVariableW(L"WEL_NOTAP_LOGICAL_IP", logical_ip, ARRAYSIZE(logical_ip)) == 0) return 2;
    if (options.token != NULL) wcsncpy_s(token, ARRAYSIZE(token), options.token, _TRUNCATE);
    else if (GetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", token, ARRAYSIZE(token)) == 0) return 2;
    if (options.log_path != NULL) wcsncpy_s(log_path, ARRAYSIZE(log_path), options.log_path, _TRUNCATE);
    else if (GetEnvironmentVariableW(L"WEL_NOTAP_LOG_PATH", log_path, ARRAYSIZE(log_path)) == 0) return 2;

    _snwprintf_s(ready_name, ARRAYSIZE(ready_name), _TRUNCATE, L"Local\\WELNoTapReady-%lu-%lu",
        (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    ready_event = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (ready_event == NULL) return 5;
    SetEnvironmentVariableW(L"WEL_NOTAP_RELAY", relay);
    SetEnvironmentVariableW(L"WEL_NOTAP_ROOM", room);
    SetEnvironmentVariableW(L"WEL_NOTAP_LOGICAL_IP", logical_ip);
    SetEnvironmentVariableW(L"WEL_NOTAP_TOKEN", token);
    SetEnvironmentVariableW(L"WEL_NOTAP_LOG_PATH", log_path);
    SetEnvironmentVariableW(L"WEL_NOTAP_READY_EVENT", ready_name);
    if (options.direct_agent_port != NULL && options.direct_hook_port != NULL) {
        SetEnvironmentVariableW(L"WEL_NOTAP_DIRECT_AGENT_PORT", options.direct_agent_port);
        SetEnvironmentVariableW(L"WEL_NOTAP_DIRECT_HOOK_PORT", options.direct_hook_port);
        if (options.direct_peer_ip != NULL) {
            SetEnvironmentVariableW(L"WEL_NOTAP_DIRECT_PEER_IP", options.direct_peer_ip);
        }
    }

    command_line = quoted_command_line(game);
    working_directory = game_directory(game);
    if (command_line == NULL || working_directory == NULL) {
        if (command_line != NULL) HeapFree(GetProcessHeap(), 0, command_line);
        if (working_directory != NULL) free(working_directory);
        CloseHandle(ready_event);
        return 5;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(game, command_line, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_DEFAULT_ERROR_MODE,
        NULL, working_directory, &startup, &process)) {
        fwprintf(stderr, L"CreateProcess failed: Windows error %lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, command_line);
        free(working_directory);
        CloseHandle(ready_event);
        return 6;
    }
    HeapFree(GetProcessHeap(), 0, command_line);
    free(working_directory);

    if (!inject_hook(process.hProcess, process.hThread, hook, &injection_error, &injection_stage, &apc_remote_path)) {
        fprintf(stderr, "Hook module injection failed at %s: Windows error %lu\n",
            injection_stage, (unsigned long)injection_error);
        TerminateProcess(process.hProcess, 7);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return 7;
    }
    if (apc_remote_path != NULL) {
        if (ResumeThread(process.hThread) == (DWORD)-1) {
            fprintf(stderr, "ResumeThread for APC injection failed: Windows error %lu\n", GetLastError());
            TerminateProcess(process.hProcess, 9);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(ready_event);
            return 9;
        }
        resumed_for_apc = 1;
    }
    if (WaitForSingleObject(ready_event, 15000) != WAIT_OBJECT_0) {
        fputs(apc_remote_path != NULL
            ? "Hook module did not initialize through QueueUserAPC\n"
            : "Hook module did not initialize through CreateRemoteThread\n", stderr);
        TerminateProcess(process.hProcess, 8);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return 8;
    }
    if (apc_remote_path != NULL) VirtualFreeEx(process.hProcess, apc_remote_path, 0, MEM_RELEASE);
    CloseHandle(ready_event);
    if (!resumed_for_apc && ResumeThread(process.hThread) == (DWORD)-1) {
        fprintf(stderr, "ResumeThread failed: Windows error %lu\n", GetLastError());
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 9;
    }
    printf("STARTED pid=%lu injection=%s\n", (unsigned long)process.dwProcessId,
        apc_remote_path != NULL ? "apc" : "remote-thread");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
