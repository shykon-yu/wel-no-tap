#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int (WSAAPI *wel_bind_fn)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *wel_connect_fn)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *wel_sendto_fn)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WSAAPI *wel_send_fn)(SOCKET, const char *, int, int);
typedef int (WSAAPI *wel_wsaconnect_fn)(
    SOCKET,
    const struct sockaddr *,
    int,
    LPWSABUF,
    LPWSABUF,
    LPQOS,
    LPQOS
);
typedef int (WSAAPI *wel_wsasend_fn)(
    SOCKET,
    LPWSABUF,
    DWORD,
    LPDWORD,
    DWORD,
    LPWSAOVERLAPPED,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE
);
typedef int (WSAAPI *wel_wsasendto_fn)(
    SOCKET,
    LPWSABUF,
    DWORD,
    LPDWORD,
    DWORD,
    const struct sockaddr *,
    int,
    LPWSAOVERLAPPED,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE
);

static HMODULE g_hook_module = NULL;
static volatile LONG g_stopping = 0;
static IN_ADDR g_tap_address;
static IN_ADDR g_broadcast_address;
static IN_ADDR g_subnet_mask;
static DWORD g_interface_index = 0;
static wel_bind_fn g_real_bind = NULL;
static wel_connect_fn g_real_connect = NULL;
static wel_send_fn g_real_send = NULL;
static wel_sendto_fn g_real_sendto = NULL;
static wel_wsaconnect_fn g_real_wsaconnect = NULL;
static wel_wsasend_fn g_real_wsasend = NULL;
static wel_wsasendto_fn g_real_wsasendto = NULL;

static int read_ipv4_environment(const char *name, IN_ADDR *address) {
    char value[64];
    DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    unsigned long parsed;

    if (length == 0 || length >= sizeof(value)) return 0;
    parsed = inet_addr(value);
    if (parsed == INADDR_NONE) return 0;
    address->S_un.S_addr = parsed;
    return 1;
}

static int read_interface_environment(DWORD *interface_index) {
    char value[32];
    char *end = NULL;
    unsigned long parsed;
    DWORD length = GetEnvironmentVariableA("WEL_TAP_INTERFACE_INDEX", value, sizeof(value));

    if (length == 0 || length >= sizeof(value)) return 0;
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') return 0;
    *interface_index = (DWORD)parsed;
    return 1;
}

static int is_udp_socket(SOCKET socket_handle) {
    int socket_type = 0;
    int length = sizeof(socket_type);
    return getsockopt(socket_handle, SOL_SOCKET, SO_TYPE, (char *)&socket_type, &length) == 0 &&
        socket_type == SOCK_DGRAM;
}

static void select_tap_interface(SOCKET socket_handle) {
    DWORD network_index;
    if (g_interface_index == 0) return;
    network_index = htonl(g_interface_index);
    setsockopt(socket_handle, IPPROTO_IP, IP_UNICAST_IF, (const char *)&network_index, sizeof(network_index));
}

static int bind_udp_to_tap(SOCKET socket_handle, unsigned short port) {
    struct sockaddr_in local_address;
    if (g_real_bind == NULL) return SOCKET_ERROR;

    ZeroMemory(&local_address, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_port = port;
    local_address.sin_addr = g_tap_address;
    select_tap_interface(socket_handle);
    return g_real_bind(socket_handle, (const struct sockaddr *)&local_address, sizeof(local_address));
}

static void ensure_udp_bound_to_tap(SOCKET socket_handle) {
    struct sockaddr_in local_address;
    int length = sizeof(local_address);

    if (!is_udp_socket(socket_handle)) return;
    ZeroMemory(&local_address, sizeof(local_address));
    if (getsockname(socket_handle, (struct sockaddr *)&local_address, &length) != 0) {
        bind_udp_to_tap(socket_handle, 0);
        return;
    }
    select_tap_interface(socket_handle);
    if (local_address.sin_family == AF_INET && local_address.sin_addr.S_un.S_addr == INADDR_ANY) {
        bind_udp_to_tap(socket_handle, local_address.sin_port);
    }
}

static int is_room_destination(const struct sockaddr *destination, int destination_length) {
    const struct sockaddr_in *ipv4;
    unsigned long destination_network;
    unsigned long tap_network;

    if (destination == NULL || destination_length < (int)sizeof(struct sockaddr_in) ||
        destination->sa_family != AF_INET) return 0;
    ipv4 = (const struct sockaddr_in *)destination;
    destination_network = ipv4->sin_addr.S_un.S_addr & g_subnet_mask.S_un.S_addr;
    tap_network = g_tap_address.S_un.S_addr & g_subnet_mask.S_un.S_addr;
    return destination_network == tap_network;
}

static int is_discovery_destination(const struct sockaddr *destination, int destination_length) {
    const struct sockaddr_in *ipv4;
    if (destination == NULL || destination_length < (int)sizeof(struct sockaddr_in) ||
        destination->sa_family != AF_INET) return 0;
    ipv4 = (const struct sockaddr_in *)destination;
    return ipv4->sin_port == htons(5739) && ipv4->sin_addr.S_un.S_addr == INADDR_BROADCAST;
}

static void directed_discovery_destination(
    const struct sockaddr *destination,
    struct sockaddr_in *directed_destination
) {
    CopyMemory(directed_destination, destination, sizeof(*directed_destination));
    directed_destination->sin_addr = g_broadcast_address;
}

static int WSAAPI wel_bind(SOCKET socket_handle, const struct sockaddr *address, int address_length) {
    struct sockaddr_in tap_address;
    const struct sockaddr_in *ipv4;

    if (g_real_bind == NULL) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    if (address == NULL || address_length < (int)sizeof(struct sockaddr_in) ||
        address->sa_family != AF_INET || !is_udp_socket(socket_handle)) {
        return g_real_bind(socket_handle, address, address_length);
    }

    ipv4 = (const struct sockaddr_in *)address;
    if (ipv4->sin_addr.S_un.S_addr == htonl(INADDR_LOOPBACK)) {
        return g_real_bind(socket_handle, address, address_length);
    }
    CopyMemory(&tap_address, ipv4, sizeof(tap_address));
    tap_address.sin_addr = g_tap_address;
    select_tap_interface(socket_handle);
    return g_real_bind(socket_handle, (const struct sockaddr *)&tap_address, sizeof(tap_address));
}

static int WSAAPI wel_connect(
    SOCKET socket_handle,
    const struct sockaddr *destination,
    int destination_length
) {
    if (is_udp_socket(socket_handle) && is_room_destination(destination, destination_length)) {
        ensure_udp_bound_to_tap(socket_handle);
    }
    return g_real_connect(socket_handle, destination, destination_length);
}

static int WSAAPI wel_wsaconnect(
    SOCKET socket_handle,
    const struct sockaddr *destination,
    int destination_length,
    LPWSABUF caller_data,
    LPWSABUF callee_data,
    LPQOS socket_qos,
    LPQOS group_qos
) {
    if (is_udp_socket(socket_handle) && is_room_destination(destination, destination_length)) {
        ensure_udp_bound_to_tap(socket_handle);
    }
    return g_real_wsaconnect(socket_handle, destination, destination_length,
        caller_data, callee_data, socket_qos, group_qos);
}

static int WSAAPI wel_sendto(
    SOCKET socket_handle,
    const char *buffer,
    int buffer_length,
    int flags,
    const struct sockaddr *destination,
    int destination_length
) {
    struct sockaddr_in directed_destination;
    if (!is_discovery_destination(destination, destination_length)) {
        if (is_udp_socket(socket_handle) && is_room_destination(destination, destination_length)) {
            ensure_udp_bound_to_tap(socket_handle);
        }
        return g_real_sendto(socket_handle, buffer, buffer_length, flags, destination, destination_length);
    }

    ensure_udp_bound_to_tap(socket_handle);
    directed_discovery_destination(destination, &directed_destination);
    return g_real_sendto(
        socket_handle,
        buffer,
        buffer_length,
        flags,
        (const struct sockaddr *)&directed_destination,
        sizeof(directed_destination)
    );
}

static int WSAAPI wel_send(
    SOCKET socket_handle,
    const char *buffer,
    int buffer_length,
    int flags
) {
    struct sockaddr_storage destination;
    int destination_length = sizeof(destination);

    ZeroMemory(&destination, sizeof(destination));
    if (is_udp_socket(socket_handle) &&
        getpeername(socket_handle, (struct sockaddr *)&destination, &destination_length) == 0 &&
        is_room_destination((const struct sockaddr *)&destination, destination_length)) {
        ensure_udp_bound_to_tap(socket_handle);
    }
    return g_real_send(socket_handle, buffer, buffer_length, flags);
}

static int WSAAPI wel_wsasend(
    SOCKET socket_handle,
    LPWSABUF buffers,
    DWORD buffer_count,
    LPDWORD bytes_sent,
    DWORD flags,
    LPWSAOVERLAPPED overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion_routine
) {
    struct sockaddr_storage destination;
    int destination_length = sizeof(destination);

    ZeroMemory(&destination, sizeof(destination));
    if (is_udp_socket(socket_handle) &&
        getpeername(socket_handle, (struct sockaddr *)&destination, &destination_length) == 0 &&
        is_room_destination((const struct sockaddr *)&destination, destination_length)) {
        ensure_udp_bound_to_tap(socket_handle);
    }
    return g_real_wsasend(socket_handle, buffers, buffer_count, bytes_sent, flags,
        overlapped, completion_routine);
}

static int WSAAPI wel_wsasendto(
    SOCKET socket_handle,
    LPWSABUF buffers,
    DWORD buffer_count,
    LPDWORD bytes_sent,
    DWORD flags,
    const struct sockaddr *destination,
    int destination_length,
    LPWSAOVERLAPPED overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion_routine
) {
    struct sockaddr_in directed_destination;
    if (!is_discovery_destination(destination, destination_length)) {
        if (is_udp_socket(socket_handle) && is_room_destination(destination, destination_length)) {
            ensure_udp_bound_to_tap(socket_handle);
        }
        return g_real_wsasendto(socket_handle, buffers, buffer_count, bytes_sent, flags,
            destination, destination_length, overlapped, completion_routine);
    }

    ensure_udp_bound_to_tap(socket_handle);
    directed_discovery_destination(destination, &directed_destination);
    return g_real_wsasendto(socket_handle, buffers, buffer_count, bytes_sent, flags,
        (const struct sockaddr *)&directed_destination, sizeof(directed_destination),
        overlapped, completion_routine);
}

static void patch_import_slot(PULONG_PTR slot, ULONG_PTR replacement) {
    DWORD old_protection;
    if (*slot == replacement) return;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) return;
    *slot = replacement;
    VirtualProtect(slot, sizeof(*slot), old_protection, &old_protection);
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
            const char *library_name = (const char *)module + imports->Name;
            PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)((BYTE *)module + imports->FirstThunk);
            PIMAGE_THUNK_DATA names;
            if (_stricmp(library_name, "ws2_32.dll") != 0 && _stricmp(library_name, "wsock32.dll") != 0) {
                ++imports;
                continue;
            }
            names = imports->OriginalFirstThunk == 0
                ? NULL
                : (PIMAGE_THUNK_DATA)((BYTE *)module + imports->OriginalFirstThunk);
            while (thunk->u1.Function != 0) {
                PULONG_PTR slot = (PULONG_PTR)&thunk->u1.Function;
                if (names != NULL && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                    PIMAGE_IMPORT_BY_NAME import_name = (PIMAGE_IMPORT_BY_NAME)((BYTE *)module + names->u1.AddressOfData);
                    if (strcmp((const char *)import_name->Name, "bind") == 0) {
                        patch_import_slot(slot, (ULONG_PTR)wel_bind);
                    } else if (strcmp((const char *)import_name->Name, "connect") == 0) {
                        patch_import_slot(slot, (ULONG_PTR)wel_connect);
                    } else if (strcmp((const char *)import_name->Name, "send") == 0) {
                        patch_import_slot(slot, (ULONG_PTR)wel_send);
                    } else if (strcmp((const char *)import_name->Name, "sendto") == 0) {
                        patch_import_slot(slot, (ULONG_PTR)wel_sendto);
                    } else if (strcmp((const char *)import_name->Name, "WSAConnect") == 0) {
                        patch_import_slot(slot, (ULONG_PTR)wel_wsaconnect);
                    } else if (strcmp((const char *)import_name->Name, "WSASend") == 0) {
                        patch_import_slot(slot, (ULONG_PTR)wel_wsasend);
                    } else if (strcmp((const char *)import_name->Name, "WSASendTo") == 0) {
                        patch_import_slot(slot, (ULONG_PTR)wel_wsasendto);
                    }
                } else {
                    if (*slot == (ULONG_PTR)g_real_bind) patch_import_slot(slot, (ULONG_PTR)wel_bind);
                    else if (*slot == (ULONG_PTR)g_real_connect) patch_import_slot(slot, (ULONG_PTR)wel_connect);
                    else if (*slot == (ULONG_PTR)g_real_send) patch_import_slot(slot, (ULONG_PTR)wel_send);
                    else if (*slot == (ULONG_PTR)g_real_sendto) patch_import_slot(slot, (ULONG_PTR)wel_sendto);
                    else if (*slot == (ULONG_PTR)g_real_wsaconnect) patch_import_slot(slot, (ULONG_PTR)wel_wsaconnect);
                    else if (*slot == (ULONG_PTR)g_real_wsasend) patch_import_slot(slot, (ULONG_PTR)wel_wsasend);
                    else if (*slot == (ULONG_PTR)g_real_wsasendto) patch_import_slot(slot, (ULONG_PTR)wel_wsasendto);
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
    DWORD index;
    DWORD count;

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

static int initialize_hook(void) {
    HMODULE winsock = GetModuleHandleW(L"ws2_32.dll");
    HANDLE worker;

    if (!read_ipv4_environment("WEL_TAP_IP", &g_tap_address) ||
        !read_ipv4_environment("WEL_BROADCAST_IP", &g_broadcast_address) ||
        !read_ipv4_environment("WEL_SUBNET_MASK", &g_subnet_mask) ||
        !read_interface_environment(&g_interface_index)) return 0;
    if (winsock == NULL) winsock = LoadLibraryW(L"ws2_32.dll");
    if (winsock == NULL) return 0;

    g_real_bind = (wel_bind_fn)GetProcAddress(winsock, "bind");
    g_real_connect = (wel_connect_fn)GetProcAddress(winsock, "connect");
    g_real_send = (wel_send_fn)GetProcAddress(winsock, "send");
    g_real_sendto = (wel_sendto_fn)GetProcAddress(winsock, "sendto");
    g_real_wsaconnect = (wel_wsaconnect_fn)GetProcAddress(winsock, "WSAConnect");
    g_real_wsasend = (wel_wsasend_fn)GetProcAddress(winsock, "WSASend");
    g_real_wsasendto = (wel_wsasendto_fn)GetProcAddress(winsock, "WSASendTo");
    if (g_real_bind == NULL || g_real_connect == NULL || g_real_send == NULL ||
        g_real_sendto == NULL || g_real_wsaconnect == NULL || g_real_wsasend == NULL ||
        g_real_wsasendto == NULL) return 0;

    patch_module_imports(GetModuleHandleW(NULL));
    worker = CreateThread(NULL, 0, module_watch_thread, NULL, 0, NULL);
    if (worker != NULL) CloseHandle(worker);
    return 1;
}

static void signal_hook_ready(void) {
    char event_name[128];
    DWORD length = GetEnvironmentVariableA("WEL_HOOK_READY_EVENT", event_name, sizeof(event_name));
    HANDLE event;
    if (length == 0 || length >= sizeof(event_name)) return;
    event = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name);
    if (event == NULL) return;
    SetEvent(event);
    CloseHandle(event);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hook_module = instance;
        DisableThreadLibraryCalls(instance);
        if (initialize_hook()) signal_hook_ready();
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stopping, 1);
    }
    return TRUE;
}
