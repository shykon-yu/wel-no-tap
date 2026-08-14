#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct {
    const wchar_t *game_path;
    const wchar_t *hook_path;
    const wchar_t *trace_path;
    int self_test;
} launch_options;

static int parse_options(int argc, wchar_t **argv, launch_options *options) {
    int index;
    ZeroMemory(options, sizeof(*options));
    for (index = 1; index < argc; ++index) {
        if (wcscmp(argv[index], L"--game") == 0 && index + 1 < argc) options->game_path = argv[++index];
        else if (wcscmp(argv[index], L"--hook") == 0 && index + 1 < argc) options->hook_path = argv[++index];
        else if (wcscmp(argv[index], L"--trace") == 0 && index + 1 < argc) options->trace_path = argv[++index];
        else if (wcscmp(argv[index], L"--self-test") == 0) options->self_test = 1;
        else return 0;
    }
    return options->self_test ||
        (options->game_path != NULL && options->hook_path != NULL && options->trace_path != NULL);
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

static int inject_hook(HANDLE process, const wchar_t *hook_path) {
    SIZE_T path_size = (wcslen(hook_path) + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, NULL, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HMODULE kernel32 = GetModuleHandleW(L"Kernel32.dll");
    FARPROC load_library;
    HANDLE thread;
    DWORD module_handle = 0;

    if (remote_path == NULL || kernel32 == NULL) return 0;
    if (!WriteProcessMemory(process, remote_path, hook_path, path_size, NULL)) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    load_library = GetProcAddress(kernel32, "LoadLibraryW");
    if (load_library == NULL) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library, remote_path, 0, NULL);
    if (thread == NULL) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    if (WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0) GetExitCodeThread(thread, &module_handle);
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    return module_handle != 0;
}

int wmain(int argc, wchar_t **argv) {
    launch_options options;
    wchar_t game[MAX_PATH];
    wchar_t hook[MAX_PATH];
    wchar_t ready_name[96];
    wchar_t *command_line;
    wchar_t *working_directory;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HANDLE ready_event;

    if (!parse_options(argc, argv, &options)) {
        fputs("Usage: welnptgame --game <WE8.exe> --hook <welnpttrace.dll> --trace <trace.jsonl>\n", stderr);
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
        fwprintf(stderr, L"Trace module not found: %ls\n", options.hook_path);
        return 4;
    }

    _snwprintf_s(ready_name, ARRAYSIZE(ready_name), _TRUNCATE, L"Local\\WELNoTapReady-%lu-%lu",
        (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    ready_event = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (ready_event == NULL) return 5;
    SetEnvironmentVariableW(L"WEL_NOTAP_TRACE_PATH", options.trace_path);
    SetEnvironmentVariableW(L"WEL_NOTAP_READY_EVENT", ready_name);

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

    if (!inject_hook(process.hProcess, hook)) {
        fprintf(stderr, "Trace module injection failed: Windows error %lu\n", GetLastError());
        TerminateProcess(process.hProcess, 7);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return 7;
    }
    if (WaitForSingleObject(ready_event, 5000) != WAIT_OBJECT_0) {
        fputs("Trace module did not initialize\n", stderr);
        TerminateProcess(process.hProcess, 8);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return 8;
    }
    CloseHandle(ready_event);
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        fprintf(stderr, "ResumeThread failed: Windows error %lu\n", GetLastError());
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 9;
    }
    printf("STARTED pid=%lu\n", (unsigned long)process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
