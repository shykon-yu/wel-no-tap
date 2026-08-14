#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef SOCKET (WSAAPI *wel_socket_fn)(int, int, int);
typedef SOCKET (WSAAPI *wel_wsasocketa_fn)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);
typedef SOCKET (WSAAPI *wel_wsa_socketw_fn)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
typedef int (WSAAPI *wel_bind_fn)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *wel_sendto_fn)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WSAAPI *wel_recvfrom_fn)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WSAAPI *wel_wsasendto_fn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *wel_wsarecvfrom_fn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, struct sockaddr *, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *wel_select_fn)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
typedef int (WSAAPI *wel_wsa_event_select_fn)(SOCKET, WSAEVENT, long);
typedef int (WSAAPI *wel_wsa_async_select_fn)(SOCKET, HWND, u_int, long);
typedef DWORD (WSAAPI *wel_wsa_wait_fn)(DWORD, const WSAEVENT *, BOOL, DWORD, BOOL);
typedef int (WSAAPI *wel_wsa_poll_fn)(LPWSAPOLLFD, ULONG, INT);
typedef int (WSAAPI *wel_closesocket_fn)(SOCKET);

static HMODULE g_hook_module;
static volatile LONG g_stopping;
static wchar_t g_trace_path[MAX_PATH];
static wel_socket_fn g_real_socket;
static wel_wsasocketa_fn g_real_wsa_socket_a;
static wel_wsa_socketw_fn g_real_wsa_socket_w;
static wel_bind_fn g_real_bind;
static wel_sendto_fn g_real_sendto;
static wel_recvfrom_fn g_real_recvfrom;
static wel_wsasendto_fn g_real_wsasendto;
static wel_wsarecvfrom_fn g_real_wsarecvfrom;
static wel_select_fn g_real_select;
static wel_wsa_event_select_fn g_real_wsa_event_select;
static wel_wsa_async_select_fn g_real_wsa_async_select;
static wel_wsa_wait_fn g_real_wsa_wait;
static wel_wsa_poll_fn g_real_wsa_poll;
static wel_closesocket_fn g_real_closesocket;

static void trace_line(const char *format, ...) {
    char message[1024];
    char line[1152];
    va_list arguments;
    HANDLE file;
    DWORD written;
    int length;

    if (g_trace_path[0] == L'\0') return;
    va_start(arguments, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
    va_end(arguments);
    length = _snprintf_s(line, sizeof(line), _TRUNCATE, "{\"tick\":%lu,%s}\r\n",
        (unsigned long)GetTickCount(), message);
    if (length <= 0) return;
    file = CreateFileW(g_trace_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, (DWORD)length, &written, NULL);
    CloseHandle(file);
}

static void sockaddr_text(const struct sockaddr *address, int address_length, char *result, size_t result_size) {
    const struct sockaddr_in *ipv4;
    char ip[INET_ADDRSTRLEN];
    if (address == NULL || address_length < (int)sizeof(struct sockaddr_in) || address->sa_family != AF_INET) {
        strcpy_s(result, result_size, "null");
        return;
    }
    ipv4 = (const struct sockaddr_in *)address;
    if (InetNtopA(AF_INET, (PVOID)&ipv4->sin_addr, ip, ARRAYSIZE(ip)) == NULL) {
        strcpy_s(ip, ARRAYSIZE(ip), "invalid");
    }
    _snprintf_s(result, result_size, _TRUNCATE, "%s:%u", ip, (unsigned)ntohs(ipv4->sin_port));
}

static int socket_is_udp(SOCKET socket_handle) {
    int socket_type = 0;
    int size = sizeof(socket_type);
    return getsockopt(socket_handle, SOL_SOCKET, SO_TYPE, (char *)&socket_type, &size) == 0 &&
        socket_type == SOCK_DGRAM;
}

static const char *protocol_name(int protocol) {
    return protocol == IPPROTO_UDP ? "udp" : protocol == IPPROTO_TCP ? "tcp" : "other";
}

static SOCKET WSAAPI wel_socket(int family, int type, int protocol) {
    SOCKET result = g_real_socket(family, type, protocol);
    trace_line("\"api\":\"socket\",\"socket\":%llu,\"family\":%d,\"type\":%d,\"protocol\":\"%s\",\"error\":%d",
        (unsigned __int64)result, family, type, protocol_name(protocol), result == INVALID_SOCKET ? WSAGetLastError() : 0);
    return result;
}

static SOCKET WSAAPI wel_wsa_socket_a(int family, int type, int protocol, LPWSAPROTOCOL_INFOA info, GROUP group, DWORD flags) {
    SOCKET result = g_real_wsa_socket_a(family, type, protocol, info, group, flags);
    trace_line("\"api\":\"WSASocketA\",\"socket\":%llu,\"family\":%d,\"type\":%d,\"protocol\":\"%s\",\"flags\":%lu,\"error\":%d",
        (unsigned __int64)result, family, type, protocol_name(protocol), (unsigned long)flags,
        result == INVALID_SOCKET ? WSAGetLastError() : 0);
    return result;
}

static SOCKET WSAAPI wel_wsa_socket_w(int family, int type, int protocol, LPWSAPROTOCOL_INFOW info, GROUP group, DWORD flags) {
    SOCKET result = g_real_wsa_socket_w(family, type, protocol, info, group, flags);
    trace_line("\"api\":\"WSASocketW\",\"socket\":%llu,\"family\":%d,\"type\":%d,\"protocol\":\"%s\",\"flags\":%lu,\"error\":%d",
        (unsigned __int64)result, family, type, protocol_name(protocol), (unsigned long)flags,
        result == INVALID_SOCKET ? WSAGetLastError() : 0);
    return result;
}

static int WSAAPI wel_bind(SOCKET socket_handle, const struct sockaddr *address, int address_length) {
    char endpoint[80];
    int result = g_real_bind(socket_handle, address, address_length);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    sockaddr_text(address, address_length, endpoint, sizeof(endpoint));
    trace_line("\"api\":\"bind\",\"socket\":%llu,\"udp\":%s,\"address\":\"%s\",\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, socket_is_udp(socket_handle) ? "true" : "false", endpoint, result, error);
    return result;
}

static int WSAAPI wel_sendto(SOCKET socket_handle, const char *buffer, int length, int flags,
    const struct sockaddr *destination, int destination_length) {
    char endpoint[80];
    int result = g_real_sendto(socket_handle, buffer, length, flags, destination, destination_length);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    sockaddr_text(destination, destination_length, endpoint, sizeof(endpoint));
    trace_line("\"api\":\"sendto\",\"socket\":%llu,\"udp\":%s,\"target\":\"%s\",\"length\":%d,\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, socket_is_udp(socket_handle) ? "true" : "false", endpoint, length, result, error);
    return result;
}

static int WSAAPI wel_wsasendto(SOCKET socket_handle, LPWSABUF buffers, DWORD buffer_count, LPDWORD bytes_sent,
    DWORD flags, const struct sockaddr *destination, int destination_length, LPWSAOVERLAPPED overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    char endpoint[80];
    DWORD requested = 0;
    DWORD index;
    int result;
    int error;
    for (index = 0; index < buffer_count; ++index) requested += buffers[index].len;
    result = g_real_wsasendto(socket_handle, buffers, buffer_count, bytes_sent, flags, destination,
        destination_length, overlapped, completion);
    error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    sockaddr_text(destination, destination_length, endpoint, sizeof(endpoint));
    trace_line("\"api\":\"WSASendTo\",\"socket\":%llu,\"udp\":%s,\"target\":\"%s\",\"length\":%lu,\"overlapped\":%s,\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, socket_is_udp(socket_handle) ? "true" : "false", endpoint,
        (unsigned long)requested, overlapped != NULL ? "true" : "false", result, error);
    return result;
}

static int WSAAPI wel_recvfrom(SOCKET socket_handle, char *buffer, int length, int flags,
    struct sockaddr *source, int *source_length) {
    char endpoint[80];
    int result = g_real_recvfrom(socket_handle, buffer, length, flags, source, source_length);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    sockaddr_text(source, source_length == NULL ? 0 : *source_length, endpoint, sizeof(endpoint));
    trace_line("\"api\":\"recvfrom\",\"socket\":%llu,\"udp\":%s,\"source\":\"%s\",\"length\":%d,\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, socket_is_udp(socket_handle) ? "true" : "false", endpoint, length, result, error);
    return result;
}

static int WSAAPI wel_wsarecvfrom(SOCKET socket_handle, LPWSABUF buffers, DWORD buffer_count, LPDWORD bytes_received,
    LPDWORD flags, struct sockaddr *source, LPINT source_length, LPWSAOVERLAPPED overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    char endpoint[80];
    int result = g_real_wsarecvfrom(socket_handle, buffers, buffer_count, bytes_received, flags, source,
        source_length, overlapped, completion);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    sockaddr_text(source, source_length == NULL ? 0 : *source_length, endpoint, sizeof(endpoint));
    trace_line("\"api\":\"WSARecvFrom\",\"socket\":%llu,\"udp\":%s,\"source\":\"%s\",\"received\":%lu,\"overlapped\":%s,\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, socket_is_udp(socket_handle) ? "true" : "false", endpoint,
        bytes_received == NULL ? 0UL : (unsigned long)*bytes_received, overlapped != NULL ? "true" : "false", result, error);
    return result;
}

static int WSAAPI wel_select(int ignored, fd_set *read_set, fd_set *write_set, fd_set *except_set, const struct timeval *timeout) {
    u_int read_count = read_set == NULL ? 0 : read_set->fd_count;
    u_int write_count = write_set == NULL ? 0 : write_set->fd_count;
    u_int except_count = except_set == NULL ? 0 : except_set->fd_count;
    int result = g_real_select(ignored, read_set, write_set, except_set, timeout);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    trace_line("\"api\":\"select\",\"readCount\":%u,\"writeCount\":%u,\"exceptCount\":%u,\"result\":%d,\"error\":%d",
        read_count, write_count, except_count, result, error);
    return result;
}

static int WSAAPI wel_wsa_event_select(SOCKET socket_handle, WSAEVENT event_handle, long network_events) {
    int result = g_real_wsa_event_select(socket_handle, event_handle, network_events);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    trace_line("\"api\":\"WSAEventSelect\",\"socket\":%llu,\"udp\":%s,\"events\":%ld,\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, socket_is_udp(socket_handle) ? "true" : "false", network_events, result, error);
    return result;
}

static int WSAAPI wel_wsa_async_select(SOCKET socket_handle, HWND window, u_int message, long network_events) {
    int result = g_real_wsa_async_select(socket_handle, window, message, network_events);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    trace_line("\"api\":\"WSAAsyncSelect\",\"socket\":%llu,\"udp\":%s,\"message\":%u,\"events\":%ld,\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, socket_is_udp(socket_handle) ? "true" : "false", message, network_events, result, error);
    return result;
}

static DWORD WSAAPI wel_wsa_wait(DWORD count, const WSAEVENT *events, BOOL wait_all, DWORD timeout, BOOL alertable) {
    DWORD result = g_real_wsa_wait(count, events, wait_all, timeout, alertable);
    trace_line("\"api\":\"WSAWaitForMultipleEvents\",\"count\":%lu,\"waitAll\":%s,\"timeout\":%lu,\"result\":%lu",
        (unsigned long)count, wait_all ? "true" : "false", (unsigned long)timeout, (unsigned long)result);
    return result;
}

static int WSAAPI wel_wsa_poll(LPWSAPOLLFD fds, ULONG count, INT timeout) {
    int result = g_real_wsa_poll(fds, count, timeout);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    trace_line("\"api\":\"WSAPoll\",\"count\":%lu,\"timeout\":%d,\"result\":%d,\"error\":%d",
        (unsigned long)count, timeout, result, error);
    return result;
}

static int WSAAPI wel_closesocket(SOCKET socket_handle) {
    int result = g_real_closesocket(socket_handle);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    trace_line("\"api\":\"closesocket\",\"socket\":%llu,\"result\":%d,\"error\":%d",
        (unsigned __int64)socket_handle, result, error);
    return result;
}

static void patch_import_slot(PULONG_PTR slot, ULONG_PTR replacement) {
    DWORD old_protection;
    if (*slot == replacement || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) return;
    *slot = replacement;
    VirtualProtect(slot, sizeof(*slot), old_protection, &old_protection);
}

static void patch_named_import(PULONG_PTR slot, const char *name) {
    if (strcmp(name, "socket") == 0 && g_real_socket != NULL) patch_import_slot(slot, (ULONG_PTR)wel_socket);
    else if (strcmp(name, "WSASocketA") == 0 && g_real_wsa_socket_a != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_a);
    else if (strcmp(name, "WSASocketW") == 0 && g_real_wsa_socket_w != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_w);
    else if (strcmp(name, "bind") == 0 && g_real_bind != NULL) patch_import_slot(slot, (ULONG_PTR)wel_bind);
    else if (strcmp(name, "sendto") == 0 && g_real_sendto != NULL) patch_import_slot(slot, (ULONG_PTR)wel_sendto);
    else if (strcmp(name, "recvfrom") == 0 && g_real_recvfrom != NULL) patch_import_slot(slot, (ULONG_PTR)wel_recvfrom);
    else if (strcmp(name, "WSASendTo") == 0 && g_real_wsasendto != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsasendto);
    else if (strcmp(name, "WSARecvFrom") == 0 && g_real_wsarecvfrom != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsarecvfrom);
    else if (strcmp(name, "select") == 0 && g_real_select != NULL) patch_import_slot(slot, (ULONG_PTR)wel_select);
    else if (strcmp(name, "WSAEventSelect") == 0 && g_real_wsa_event_select != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsa_event_select);
    else if (strcmp(name, "WSAAsyncSelect") == 0 && g_real_wsa_async_select != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsa_async_select);
    else if (strcmp(name, "WSAWaitForMultipleEvents") == 0 && g_real_wsa_wait != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsa_wait);
    else if (strcmp(name, "WSAPoll") == 0 && g_real_wsa_poll != NULL) patch_import_slot(slot, (ULONG_PTR)wel_wsa_poll);
    else if (strcmp(name, "closesocket") == 0 && g_real_closesocket != NULL) patch_import_slot(slot, (ULONG_PTR)wel_closesocket);
}

static void patch_address_import(PULONG_PTR slot) {
    if (*slot == (ULONG_PTR)g_real_socket) patch_import_slot(slot, (ULONG_PTR)wel_socket);
    else if (*slot == (ULONG_PTR)g_real_wsa_socket_a) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_a);
    else if (*slot == (ULONG_PTR)g_real_wsa_socket_w) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_w);
    else if (*slot == (ULONG_PTR)g_real_bind) patch_import_slot(slot, (ULONG_PTR)wel_bind);
    else if (*slot == (ULONG_PTR)g_real_sendto) patch_import_slot(slot, (ULONG_PTR)wel_sendto);
    else if (*slot == (ULONG_PTR)g_real_recvfrom) patch_import_slot(slot, (ULONG_PTR)wel_recvfrom);
    else if (*slot == (ULONG_PTR)g_real_wsasendto) patch_import_slot(slot, (ULONG_PTR)wel_wsasendto);
    else if (*slot == (ULONG_PTR)g_real_wsarecvfrom) patch_import_slot(slot, (ULONG_PTR)wel_wsarecvfrom);
    else if (*slot == (ULONG_PTR)g_real_select) patch_import_slot(slot, (ULONG_PTR)wel_select);
    else if (*slot == (ULONG_PTR)g_real_wsa_event_select) patch_import_slot(slot, (ULONG_PTR)wel_wsa_event_select);
    else if (*slot == (ULONG_PTR)g_real_wsa_async_select) patch_import_slot(slot, (ULONG_PTR)wel_wsa_async_select);
    else if (*slot == (ULONG_PTR)g_real_wsa_wait) patch_import_slot(slot, (ULONG_PTR)wel_wsa_wait);
    else if (*slot == (ULONG_PTR)g_real_wsa_poll) patch_import_slot(slot, (ULONG_PTR)wel_wsa_poll);
    else if (*slot == (ULONG_PTR)g_real_closesocket) patch_import_slot(slot, (ULONG_PTR)wel_closesocket);
}

static void patch_module_imports(HMODULE module) {
    PIMAGE_DOS_HEADER dos_header;
    PIMAGE_NT_HEADERS nt_headers;
    PIMAGE_IMPORT_DESCRIPTOR imports;
    DWORD import_rva;

    if (module == NULL || module == g_hook_module) return;
    __try {
        dos_header = (PIMAGE_DOS_HEADER)module;
        if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) return;
        nt_headers = (PIMAGE_NT_HEADERS)((BYTE *)module + dos_header->e_lfanew);
        if (nt_headers->Signature != IMAGE_NT_SIGNATURE) return;
        import_rva = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (import_rva == 0) return;
        imports = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)module + import_rva);
        while (imports->Name != 0) {
            const char *library = (const char *)module + imports->Name;
            PIMAGE_THUNK_DATA thunk;
            PIMAGE_THUNK_DATA names;
            if (_stricmp(library, "ws2_32.dll") != 0 && _stricmp(library, "wsock32.dll") != 0) {
                ++imports;
                continue;
            }
            thunk = (PIMAGE_THUNK_DATA)((BYTE *)module + imports->FirstThunk);
            names = imports->OriginalFirstThunk == 0 ? NULL :
                (PIMAGE_THUNK_DATA)((BYTE *)module + imports->OriginalFirstThunk);
            while (thunk->u1.Function != 0) {
                PULONG_PTR slot = (PULONG_PTR)&thunk->u1.Function;
                if (names != NULL && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                    PIMAGE_IMPORT_BY_NAME imported = (PIMAGE_IMPORT_BY_NAME)((BYTE *)module + names->u1.AddressOfData);
                    patch_named_import(slot, (const char *)imported->Name);
                } else {
                    patch_address_import(slot);
                }
                if (names != NULL) ++names;
                ++thunk;
            }
            ++imports;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}

static void patch_all_modules(void) {
    HMODULE modules[512];
    DWORD required = 0;
    DWORD count;
    DWORD index;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &required)) return;
    count = required / sizeof(HMODULE);
    if (count > ARRAYSIZE(modules)) count = ARRAYSIZE(modules);
    for (index = 0; index < count; ++index) patch_module_imports(modules[index]);
}

static DWORD WINAPI module_watch_thread(LPVOID unused) {
    (void)unused;
    while (InterlockedCompareExchange(&g_stopping, 0, 0) == 0) {
        patch_all_modules();
        Sleep(250);
    }
    return 0;
}

static void signal_ready(void) {
    char name[128];
    DWORD length = GetEnvironmentVariableA("WEL_NOTAP_READY_EVENT", name, sizeof(name));
    HANDLE event;
    if (length == 0 || length >= sizeof(name)) return;
    event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (event == NULL) return;
    SetEvent(event);
    CloseHandle(event);
}

static int initialize_hook(void) {
    HMODULE winsock = GetModuleHandleW(L"ws2_32.dll");
    HANDLE worker;
    DWORD path_length = GetEnvironmentVariableW(L"WEL_NOTAP_TRACE_PATH", g_trace_path, ARRAYSIZE(g_trace_path));
    if (path_length == 0 || path_length >= ARRAYSIZE(g_trace_path)) return 0;
    if (winsock == NULL) winsock = LoadLibraryW(L"ws2_32.dll");
    if (winsock == NULL) return 0;

    g_real_socket = (wel_socket_fn)GetProcAddress(winsock, "socket");
    g_real_wsa_socket_a = (wel_wsasocketa_fn)GetProcAddress(winsock, "WSASocketA");
    g_real_wsa_socket_w = (wel_wsa_socketw_fn)GetProcAddress(winsock, "WSASocketW");
    g_real_bind = (wel_bind_fn)GetProcAddress(winsock, "bind");
    g_real_sendto = (wel_sendto_fn)GetProcAddress(winsock, "sendto");
    g_real_recvfrom = (wel_recvfrom_fn)GetProcAddress(winsock, "recvfrom");
    g_real_wsasendto = (wel_wsasendto_fn)GetProcAddress(winsock, "WSASendTo");
    g_real_wsarecvfrom = (wel_wsarecvfrom_fn)GetProcAddress(winsock, "WSARecvFrom");
    g_real_select = (wel_select_fn)GetProcAddress(winsock, "select");
    g_real_wsa_event_select = (wel_wsa_event_select_fn)GetProcAddress(winsock, "WSAEventSelect");
    g_real_wsa_async_select = (wel_wsa_async_select_fn)GetProcAddress(winsock, "WSAAsyncSelect");
    g_real_wsa_wait = (wel_wsa_wait_fn)GetProcAddress(winsock, "WSAWaitForMultipleEvents");
    g_real_wsa_poll = (wel_wsa_poll_fn)GetProcAddress(winsock, "WSAPoll");
    g_real_closesocket = (wel_closesocket_fn)GetProcAddress(winsock, "closesocket");
    if (g_real_bind == NULL || g_real_sendto == NULL || g_real_recvfrom == NULL ||
        g_real_select == NULL || g_real_closesocket == NULL) return 0;

    patch_module_imports(GetModuleHandleW(NULL));
    trace_line("\"api\":\"hook-ready\",\"mode\":\"observe-only\"");
    worker = CreateThread(NULL, 0, module_watch_thread, NULL, 0, NULL);
    if (worker != NULL) CloseHandle(worker);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hook_module = instance;
        DisableThreadLibraryCalls(instance);
        if (initialize_hook()) signal_ready();
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stopping, 1);
    }
    return TRUE;
}
