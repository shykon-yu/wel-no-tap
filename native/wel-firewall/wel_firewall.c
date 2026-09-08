#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

#pragma comment(lib, "Advapi32.lib")

#define WEL_FIREWALL_SUCCESS 0
#define WEL_FIREWALL_INVALID_ARGUMENTS 2
#define WEL_FIREWALL_UAC_CANCELLED 10
#define WEL_FIREWALL_ELEVATION_FAILED 11
#define WEL_FIREWALL_ROOM_WARNING_BASE 40
#define WEL_FIREWALL_ROOM_UDP_WARNING 1
#define WEL_FIREWALL_ROOM_EDGE_WARNING 2
#define WEL_FIREWALL_ROOM_ICMP_WARNING 4
#define WEL_FIREWALL_WE8_BLOCK_REMAINS 31
#define WEL_FIREWALL_WE8_ALLOW_WARNING 32

typedef struct {
    const wchar_t *subnet;
    const wchar_t *edge_path;
    const wchar_t *game_path;
    int elevated;
    int self_test;
} wel_firewall_options;

static void append_netsh_log(const wchar_t *arguments, DWORD exit_code) {
    wchar_t local_app_data[MAX_PATH];
    wchar_t platform_directory[MAX_PATH];
    wchar_t log_directory[MAX_PATH];
    wchar_t log_path[MAX_PATH];
    wchar_t wide_line[4608];
    char utf8_line[9216];
    SYSTEMTIME time;
    HANDLE file;
    DWORD bytes_written;
    int utf8_length;

    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data)) == 0) return;
    if (_snwprintf_s(platform_directory, ARRAYSIZE(platform_directory), _TRUNCATE,
        L"%ls\\WELPlatform", local_app_data) < 0) return;
    if (_snwprintf_s(log_directory, ARRAYSIZE(log_directory), _TRUNCATE,
        L"%ls\\logs", platform_directory) < 0) return;
    if (_snwprintf_s(log_path, ARRAYSIZE(log_path), _TRUNCATE,
        L"%ls\\firewall.log", log_directory) < 0) return;

    CreateDirectoryW(platform_directory, NULL);
    CreateDirectoryW(log_directory, NULL);
    GetLocalTime(&time);
    if (_snwprintf_s(wide_line, ARRAYSIZE(wide_line), _TRUNCATE,
        L"[%04u-%02u-%02u %02u:%02u:%02u] exit=%lu netsh %ls\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        (unsigned long)exit_code, arguments != NULL ? arguments : L"(unavailable)") < 0) return;

    utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide_line, -1, utf8_line,
        (int)ARRAYSIZE(utf8_line), NULL, NULL);
    if (utf8_length <= 1) return;
    file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, utf8_line, (DWORD)(utf8_length - 1), &bytes_written, NULL);
    CloseHandle(file);
}

static int valid_subnet(const wchar_t *value) {
    const wchar_t *cursor = value;
    if (value == NULL || *value == L'\0') return 0;
    while (*cursor != L'\0') {
        if (!((*cursor >= L'0' && *cursor <= L'9') || *cursor == L'.' || *cursor == L'/')) return 0;
        ++cursor;
    }
    return 1;
}

static const wchar_t *find_text_case_insensitive(const wchar_t *text, const wchar_t *needle) {
    size_t needle_length;
    const wchar_t *cursor;
    if (text == NULL || needle == NULL || *needle == L'\0') return NULL;
    needle_length = wcslen(needle);
    for (cursor = text; *cursor != L'\0'; ++cursor) {
        if (_wcsnicmp(cursor, needle, needle_length) == 0) return cursor;
    }
    return NULL;
}

static int rule_blocks_program(const wchar_t *rule, const wchar_t *program_path) {
    const wchar_t *app_start;
    const wchar_t *app_end;
    wchar_t stored_path[MAX_PATH * 2];
    wchar_t expanded_path[MAX_PATH * 2];
    size_t stored_length;

    if (rule == NULL || program_path == NULL) return 0;
    if (find_text_case_insensitive(rule, L"|Action=Block|") == NULL ||
        find_text_case_insensitive(rule, L"|Dir=In|") == NULL ||
        find_text_case_insensitive(rule, L"|Active=TRUE|") == NULL) return 0;
    app_start = find_text_case_insensitive(rule, L"|App=");
    if (app_start == NULL) return 0;
    app_start += 5;
    app_end = wcschr(app_start, L'|');
    stored_length = app_end == NULL ? wcslen(app_start) : (size_t)(app_end - app_start);
    if (stored_length == 0 || stored_length >= ARRAYSIZE(stored_path)) return 0;
    wmemcpy(stored_path, app_start, stored_length);
    stored_path[stored_length] = L'\0';
    {
        DWORD expanded_length = ExpandEnvironmentStringsW(stored_path, expanded_path, ARRAYSIZE(expanded_path));
        if (expanded_length == 0 || expanded_length > ARRAYSIZE(expanded_path)) {
            wcscpy_s(expanded_path, ARRAYSIZE(expanded_path), stored_path);
        }
    }
    return _wcsicmp(expanded_path, program_path) == 0;
}

static int registry_key_has_program_block(HKEY root, const wchar_t *key_path, const wchar_t *program_path) {
    HKEY key;
    DWORD index = 0;
    LONG status;
    if (RegOpenKeyExW(root, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    for (;;) {
        wchar_t value_name[512];
        BYTE value_data[16384];
        DWORD value_name_length = ARRAYSIZE(value_name);
        DWORD value_data_length = sizeof(value_data) - sizeof(wchar_t);
        DWORD value_type = 0;
        status = RegEnumValueW(key, index++, value_name, &value_name_length, NULL,
            &value_type, value_data, &value_data_length);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || (value_type != REG_SZ && value_type != REG_EXPAND_SZ)) continue;
        value_data[value_data_length] = 0;
        value_data[value_data_length + 1] = 0;
        if (rule_blocks_program((const wchar_t *)value_data, program_path)) {
            RegCloseKey(key);
            return 1;
        }
    }
    RegCloseKey(key);
    return 0;
}

static int rule_is_active_inbound_allow(const wchar_t *rule, const wchar_t *rule_name) {
    wchar_t name_marker[512];

    if (rule == NULL || rule_name == NULL || *rule_name == L'\0') return 0;
    if (_snwprintf_s(name_marker, ARRAYSIZE(name_marker), _TRUNCATE,
        L"|Name=%ls|", rule_name) < 0) return 0;
    return find_text_case_insensitive(rule, name_marker) != NULL &&
        find_text_case_insensitive(rule, L"|Action=Allow|") != NULL &&
        find_text_case_insensitive(rule, L"|Dir=In|") != NULL &&
        find_text_case_insensitive(rule, L"|Active=TRUE|") != NULL;
}

static int registry_key_has_active_inbound_allow(HKEY root, const wchar_t *key_path, const wchar_t *rule_name) {
    HKEY key;
    DWORD index = 0;
    LONG status;

    if (RegOpenKeyExW(root, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    for (;;) {
        wchar_t value_name[512];
        BYTE value_data[16384];
        DWORD value_name_length = ARRAYSIZE(value_name);
        DWORD value_data_length = sizeof(value_data) - sizeof(wchar_t);
        DWORD value_type = 0;

        status = RegEnumValueW(key, index++, value_name, &value_name_length, NULL,
            &value_type, value_data, &value_data_length);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || (value_type != REG_SZ && value_type != REG_EXPAND_SZ)) continue;
        value_data[value_data_length] = 0;
        value_data[value_data_length + 1] = 0;
        if (rule_is_active_inbound_allow((const wchar_t *)value_data, rule_name)) {
            RegCloseKey(key);
            return 1;
        }
    }
    RegCloseKey(key);
    return 0;
}

static int has_active_inbound_allow_rule(const wchar_t *rule_name) {
    const wchar_t *keys[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules",
        L"SOFTWARE\\Policies\\Microsoft\\WindowsFirewall\\FirewallRules"
    };
    size_t index;

    for (index = 0; index < ARRAYSIZE(keys); ++index) {
        if (registry_key_has_active_inbound_allow(HKEY_LOCAL_MACHINE, keys[index], rule_name)) return 1;
    }
    return 0;
}

static int has_active_inbound_program_block(const wchar_t *program_path) {
    const wchar_t *keys[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules",
        L"SOFTWARE\\Policies\\Microsoft\\WindowsFirewall\\FirewallRules"
    };
    size_t index;
    for (index = 0; index < ARRAYSIZE(keys); ++index) {
        if (registry_key_has_program_block(HKEY_LOCAL_MACHINE, keys[index], program_path)) return 1;
    }
    return 0;
}

static int parse_options(int argc, wchar_t **argv, wel_firewall_options *options) {
    int index;
    ZeroMemory(options, sizeof(*options));
    for (index = 1; index < argc; ++index) {
        if (wcscmp(argv[index], L"--elevated") == 0) options->elevated = 1;
        else if (wcscmp(argv[index], L"--self-test") == 0) options->self_test = 1;
        else if (wcscmp(argv[index], L"--subnet") == 0 && index + 1 < argc) options->subnet = argv[++index];
        else if (wcscmp(argv[index], L"--edge") == 0 && index + 1 < argc) options->edge_path = argv[++index];
        else if (wcscmp(argv[index], L"--game") == 0 && index + 1 < argc) options->game_path = argv[++index];
        else return 0;
    }
    if (options->self_test) return 1;
    if (options->game_path != NULL && *options->game_path != L'\0') return 1;
    return valid_subnet(options->subnet) && options->edge_path != NULL && *options->edge_path != L'\0';
}

static int run_netsh(const wchar_t *arguments) {
    wchar_t system_directory[MAX_PATH];
    wchar_t command_line[4096];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;

    if (GetSystemDirectoryW(system_directory, ARRAYSIZE(system_directory)) == 0) {
        append_netsh_log(arguments, GetLastError());
        return 0;
    }
    if (_snwprintf_s(command_line, ARRAYSIZE(command_line), _TRUNCATE,
        L"\"%ls\\netsh.exe\" %ls", system_directory, arguments) < 0) {
        append_netsh_log(arguments, ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
        NULL, NULL, &startup, &process)) {
        append_netsh_log(arguments, GetLastError());
        return 0;
    }
    if (WaitForSingleObject(process.hProcess, 15000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        append_netsh_log(arguments, WAIT_TIMEOUT);
        return 0;
    }
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    append_netsh_log(arguments, exit_code);
    return exit_code == 0;
}

static int dotted_subnet_to_cidr(const wchar_t *subnet, wchar_t *cidr, size_t cidr_count) {
    wchar_t network[64];
    const wchar_t *separator;
    size_t network_length;
    unsigned int a, b, c, d;
    unsigned int m1, m2, m3, m4;
    unsigned long mask;
    int prefix = 0;
    int zero_seen = 0;
    int bit;

    if (subnet == NULL) return 0;
    separator = wcschr(subnet, L'/');
    if (separator == NULL) return 0;
    network_length = (size_t)(separator - subnet);
    if (network_length == 0 || network_length >= ARRAYSIZE(network)) return 0;
    wmemcpy(network, subnet, network_length);
    network[network_length] = L'\0';
    if (swscanf_s(network, L"%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (swscanf_s(separator + 1, L"%u.%u.%u.%u", &m1, &m2, &m3, &m4) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255 ||
        m1 > 255 || m2 > 255 || m3 > 255 || m4 > 255) return 0;
    mask = (m1 << 24) | (m2 << 16) | (m3 << 8) | m4;
    for (bit = 31; bit >= 0; --bit) {
        if ((mask & (1UL << bit)) != 0) {
            if (zero_seen) return 0;
            ++prefix;
        } else {
            zero_seen = 1;
        }
    }
    return _snwprintf_s(cidr, cidr_count, _TRUNCATE,
        L"%u.%u.%u.%u/%d", a, b, c, d, prefix) >= 0;
}

static int add_udp_inbound_rule(const wchar_t *subnet) {
    wchar_t command[4096];
    wchar_t cidr[80];

    if (dotted_subnet_to_cidr(subnet, cidr, ARRAYSIZE(cidr))) {
        _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
            L"advfirewall firewall add rule name=\"WEL room UDP inbound\" dir=in action=allow protocol=udp remoteip=%ls enable=yes profile=any",
            cidr);
        if (run_netsh(command)) return 1;
    }

    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall add rule name=\"WEL room UDP inbound\" dir=in action=allow protocol=udp remoteip=%ls enable=yes profile=any",
        subnet);
    if (run_netsh(command)) return 1;

    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall add rule name=\"WEL room UDP inbound\" dir=in action=allow protocol=udp enable=yes profile=any");
    if (run_netsh(command)) return 1;

    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall add rule name=\"WEL room UDP inbound\" dir=in action=allow protocol=UDP");
    return run_netsh(command);
}

static int add_room_rules(const wel_firewall_options *options) {
    wchar_t command[4096];
    int warnings = 0;
    const wchar_t *remove_rules[] = {
        L"WEL game discovery UDP 5739 inbound",
        L"WEL room UDP inbound",
        L"WEL room UDP outbound",
        L"WEL room ICMPv4 inbound",
        L"WEL room ICMPv4 outbound",
        L"WEL n2n edge inbound"
    };
    size_t index;

    for (index = 0; index < ARRAYSIZE(remove_rules); ++index) {
        _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
            L"advfirewall firewall delete rule name=\"%ls\"", remove_rules[index]);
        run_netsh(command);
    }

    if (!add_udp_inbound_rule(options->subnet) ||
        !has_active_inbound_allow_rule(L"WEL room UDP inbound")) {
        warnings |= WEL_FIREWALL_ROOM_UDP_WARNING;
    }

    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall add rule name=\"WEL n2n edge inbound\" dir=in action=allow program=\"%ls\" enable=yes profile=any",
        options->edge_path);
    if (!run_netsh(command)) {
        _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
            L"firewall add allowedprogram program=\"%ls\" name=\"WEL n2n edge inbound\" mode=ENABLE scope=ALL profile=ALL",
            options->edge_path);
        if (!run_netsh(command)) warnings |= WEL_FIREWALL_ROOM_EDGE_WARNING;
    }
    if (!has_active_inbound_allow_rule(L"WEL n2n edge inbound")) {
        warnings |= WEL_FIREWALL_ROOM_EDGE_WARNING;
    }

    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall add rule name=\"WEL room ICMPv4 inbound\" dir=in action=allow protocol=icmpv4:any,any remoteip=%ls enable=yes profile=any",
        options->subnet);
    if (!run_netsh(command) || !has_active_inbound_allow_rule(L"WEL room ICMPv4 inbound")) {
        warnings |= WEL_FIREWALL_ROOM_ICMP_WARNING;
    }

    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall add rule name=\"WEL room ICMPv4 outbound\" dir=out action=allow protocol=icmpv4:any,any remoteip=%ls enable=yes profile=any",
        options->subnet);
    if (!run_netsh(command)) warnings |= WEL_FIREWALL_ROOM_ICMP_WARNING;
    return warnings == 0 ? WEL_FIREWALL_SUCCESS : WEL_FIREWALL_ROOM_WARNING_BASE | warnings;
}

static int add_game_rule(const wel_firewall_options *options) {
    wchar_t command[4096];

    if (options->game_path == NULL || *options->game_path == L'\0') return WEL_FIREWALL_SUCCESS;

    /* Reset only rules attached to this exact executable. This removes a
       previous "Block" choice, which otherwise overrides every Allow rule. */
    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall delete rule name=all dir=in program=\"%ls\"", options->game_path);
    run_netsh(command);
    if (has_active_inbound_program_block(options->game_path)) return WEL_FIREWALL_WE8_BLOCK_REMAINS;

    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
        L"advfirewall firewall add rule name=\"WEL WE8 inbound\" dir=in action=allow program=\"%ls\" protocol=any enable=yes profile=any",
        options->game_path);
    if (!run_netsh(command)) {
        _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
            L"firewall add allowedprogram program=\"%ls\" name=\"WEL WE8 inbound\" mode=ENABLE scope=ALL profile=ALL",
            options->game_path);
        if (!run_netsh(command)) return WEL_FIREWALL_WE8_ALLOW_WARNING;
    }
    if (!has_active_inbound_allow_rule(L"WEL WE8 inbound")) return WEL_FIREWALL_WE8_ALLOW_WARNING;
    return WEL_FIREWALL_SUCCESS;
}

static int apply_rules(const wel_firewall_options *options) {
    int result = WEL_FIREWALL_SUCCESS;
    if (options->subnet != NULL && options->edge_path != NULL) {
        result = add_room_rules(options);
        if (result != WEL_FIREWALL_SUCCESS) return result;
    }
    return add_game_rule(options);
}

static int elevate_self(const wel_firewall_options *options) {
    wchar_t executable[MAX_PATH];
    wchar_t parameters[4096];
    SHELLEXECUTEINFOW execute;
    DWORD exit_code = WEL_FIREWALL_ELEVATION_FAILED;
    int game_block_exists = options->game_path != NULL &&
        has_active_inbound_program_block(options->game_path);

    if (GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable)) == 0) return WEL_FIREWALL_ELEVATION_FAILED;
    if (options->game_path != NULL && options->subnet != NULL && options->edge_path != NULL) {
        if (_snwprintf_s(parameters, ARRAYSIZE(parameters), _TRUNCATE,
            L"--elevated --subnet \"%ls\" --edge \"%ls\" --game \"%ls\"",
            options->subnet, options->edge_path, options->game_path) < 0) return WEL_FIREWALL_INVALID_ARGUMENTS;
    } else if (options->game_path != NULL) {
        if (_snwprintf_s(parameters, ARRAYSIZE(parameters), _TRUNCATE,
            L"--elevated --game \"%ls\"", options->game_path) < 0) return WEL_FIREWALL_INVALID_ARGUMENTS;
    } else {
        if (_snwprintf_s(parameters, ARRAYSIZE(parameters), _TRUNCATE,
            L"--elevated --subnet \"%ls\" --edge \"%ls\"", options->subnet, options->edge_path) < 0) {
            return WEL_FIREWALL_INVALID_ARGUMENTS;
        }
    }

    ZeroMemory(&execute, sizeof(execute));
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = executable;
    execute.lpParameters = parameters;
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        if (game_block_exists) return WEL_FIREWALL_WE8_BLOCK_REMAINS;
        return GetLastError() == ERROR_CANCELLED ? WEL_FIREWALL_UAC_CANCELLED : WEL_FIREWALL_ELEVATION_FAILED;
    }
    if (execute.hProcess == NULL) {
        return game_block_exists ? WEL_FIREWALL_WE8_BLOCK_REMAINS : WEL_FIREWALL_ELEVATION_FAILED;
    }
    if (WaitForSingleObject(execute.hProcess, 30000) != WAIT_OBJECT_0) {
        CloseHandle(execute.hProcess);
        return game_block_exists ? WEL_FIREWALL_WE8_BLOCK_REMAINS : WEL_FIREWALL_ELEVATION_FAILED;
    }
    GetExitCodeProcess(execute.hProcess, &exit_code);
    CloseHandle(execute.hProcess);
    return (int)exit_code;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show_command) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wel_firewall_options options;
    int result;
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show_command;

    if (argv == NULL) return WEL_FIREWALL_INVALID_ARGUMENTS;
    if (!parse_options(argc, argv, &options)) {
        LocalFree(argv);
        return WEL_FIREWALL_INVALID_ARGUMENTS;
    }
    if (options.self_test) {
        wchar_t cidr[80];
        result = dotted_subnet_to_cidr(L"10.222.0.0/255.255.0.0", cidr, ARRAYSIZE(cidr)) &&
            wcscmp(cidr, L"10.222.0.0/16") == 0
            ? WEL_FIREWALL_SUCCESS
            : WEL_FIREWALL_INVALID_ARGUMENTS;
        LocalFree(argv);
        return result;
    }
    /* Always use the proven Win7 runas path on the first invocation. Windows
       reuses an already elevated token without showing a second UAC prompt. */
    result = options.elevated ? apply_rules(&options) : elevate_self(&options);
    LocalFree(argv);
    return result;
}
