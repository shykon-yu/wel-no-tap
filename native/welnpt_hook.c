#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "welnpt_protocol.h"

#define WELNPT_MAX_SOCKETS 64
#define WELNPT_MAX_QUEUED_DATAGRAMS 4096
#define WELNPT_HEARTBEAT_MS 2000
#define WELNPT_ICE_SESSION_TIMEOUT_MS 12000
#define WELNPT_MODULE_SCAN_MS 1000
#define WELNPT_DATAGRAM_POOL_SIZE 1024
#define WELNPT_STATS_INTERVAL_MS 5000
#define WELNPT_GAME_JOIN_PAYLOAD_LENGTH 64
#define WELNPT_GAME_ACCEPT_PAYLOAD_LENGTH 84
#define WELNPT_ICE_STATE_PREFIX "WELICESTATE:"
#define WELNPT_ICE_AGENT_PREFIX "WELICEAGENT:"
#define WELNPT_ICE_PEER_PREFIX "WELICEPEER:"
#define WELNPT_ICE_REMOTE_SET_PREFIX "WELICEREMOTESET"
#define WELNPT_TRANSPORT_STATE_PREFIX "WELTRANSPORT:"
#define WELNPT_GAME_PEER_PREFIX "WELGAMEPEER:"
#define WELNPT_GAME_PATH_PENDING 0
#define WELNPT_GAME_PATH_DIRECT 1
#define WELNPT_GAME_PATH_RELAY 2

typedef SOCKET (WSAAPI *wel_socket_fn)(int, int, int);
typedef SOCKET (WSAAPI *wel_wsasocketa_fn)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);
typedef SOCKET (WSAAPI *wel_wsasocketw_fn)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
typedef int (WSAAPI *wel_bind_fn)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *wel_getsockname_fn)(SOCKET, struct sockaddr *, int *);
typedef int (WSAAPI *wel_sendto_fn)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WSAAPI *wel_recvfrom_fn)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WSAAPI *wel_wsasendto_fn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
    const struct sockaddr *, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *wel_wsarecvfrom_fn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
    struct sockaddr *, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *wel_closesocket_fn)(SOCKET);

typedef struct virtual_datagram {
    struct virtual_datagram *next;
    struct sockaddr_in source;
    int length;
    char payload[WELNPT_MAX_PAYLOAD];
} virtual_datagram;

typedef struct virtual_socket {
    int active;
    SOCKET handle;
    SOCKET light_transport;
    unsigned short logical_port;
    unsigned short light_local_port;
    unsigned short light_map_port;
    virtual_datagram *head;
    virtual_datagram *tail;
    unsigned queued;
} virtual_socket;

static HMODULE g_hook_module;
static volatile LONG g_stopping;
static volatile LONG g_next_port;
static volatile LONG g_sequence;
static CRITICAL_SECTION g_state_lock;
static CRITICAL_SECTION g_queue_lock;
static CRITICAL_SECTION g_log_lock;
static int g_locks_initialized;
static int g_diagnostic_log_enabled;
static virtual_socket g_sockets[WELNPT_MAX_SOCKETS];
static unsigned char g_port_index[65536];
static virtual_datagram g_datagram_pool[WELNPT_DATAGRAM_POOL_SIZE];
static virtual_datagram *g_datagram_free;
static SOCKET g_transport = INVALID_SOCKET;
static SOCKET g_host_transport = INVALID_SOCKET;
static struct sockaddr_in g_host_address;
static unsigned short g_host_port;
static int g_light_mode;
static struct sockaddr_in g_relay_address;
static SOCKET g_direct_transport = INVALID_SOCKET;
static struct sockaddr_in g_direct_agent_address;
static uint32_t g_direct_peer_ip;
static uint32_t g_direct_transaction_peer_ip;
static unsigned short g_direct_transaction_join_port;
static volatile LONG g_direct_transaction_generation;
static unsigned short g_direct_hook_port;
static volatile LONG g_direct_connected;
static volatile LONG g_game_path;
static ULONGLONG g_ice_decision_deadline;
static ULONGLONG g_ice_decision_started;
static ULONGLONG g_ice_session_deadline;
static volatile LONG g_stat_queue_drops;
static volatile LONG g_stat_pool_drops;
static volatile LONG g_stat_max_queue;
static volatile LONG g_stat_send_direct;
static volatile LONG g_stat_send_relay;
static volatile LONG g_stat_recv_direct;
static volatile LONG g_stat_recv_relay;
static volatile LONG64 g_stat_last_log_tick;
static uint32_t g_logical_ip;
static char g_room[WELNPT_ROOM_LENGTH];
static wchar_t g_log_path[MAX_PATH];
static wel_socket_fn g_real_socket;
static wel_wsasocketa_fn g_real_wsa_socket_a;
static wel_wsasocketw_fn g_real_wsa_socket_w;
static wel_bind_fn g_real_bind;
static wel_getsockname_fn g_real_getsockname;
static wel_sendto_fn g_real_sendto;
static wel_recvfrom_fn g_real_recvfrom;
static wel_wsasendto_fn g_real_wsasendto;
static wel_wsarecvfrom_fn g_real_wsarecvfrom;
static wel_closesocket_fn g_real_closesocket;

static int report_game_peer(uint32_t target_ip, unsigned short join_port,
    unsigned short observed_source_port, unsigned short observed_target_port);
static void notify_light_socket_close(unsigned short logical_port);
static void log_line_impl(const char *format, ...);
#define log_line(...) do { \
    if (g_diagnostic_log_enabled) log_line_impl(__VA_ARGS__); \
} while (0)

static int parse_environment_port(const char *name, unsigned short *port) {
    char value[16];
    char *end = NULL;
    unsigned long parsed;
    DWORD length;
    if (name == NULL || port == NULL) return 0;
    length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) return 0;
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > 65535) return 0;
    *port = (unsigned short)parsed;
    return 1;
}

static void maybe_log_stats(void) {
    ULONGLONG now;
    LONG64 previous;
    if (!g_diagnostic_log_enabled) return;
    now = GetTickCount64();
    previous = InterlockedCompareExchange64(&g_stat_last_log_tick, 0, 0);
    if (previous != 0 && now - (ULONGLONG)previous < WELNPT_STATS_INTERVAL_MS) return;
    if (InterlockedCompareExchange64(&g_stat_last_log_tick, (LONG64)now, previous) != previous) return;
    log_line("\"api\":\"hook-stats\",\"sendDirect\":%ld,\"sendRelay\":%ld,\"recvDirect\":%ld,\"recvRelay\":%ld,\"queueDrops\":%ld,\"poolDrops\":%ld,\"maxQueue\":%ld",
        InterlockedExchange(&g_stat_send_direct, 0), InterlockedExchange(&g_stat_send_relay, 0),
        InterlockedExchange(&g_stat_recv_direct, 0), InterlockedExchange(&g_stat_recv_relay, 0),
        InterlockedExchange(&g_stat_queue_drops, 0), InterlockedExchange(&g_stat_pool_drops, 0),
        InterlockedCompareExchange(&g_stat_max_queue, 0, 0));
}

static void tune_transport_socket(SOCKET socket) {
    int buffer_size = 1024 * 1024;
    if (socket == INVALID_SOCKET) return;
    /* Keep bursts in kernel space so the receive worker is less likely to
       overrun the fixed virtual-datagram pool during menu transitions. */
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer_size, sizeof(buffer_size));
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char *)&buffer_size, sizeof(buffer_size));
}

static void notify_agent_transport_state(const char *state) {
    char message[64];
    int length;
    if (g_direct_transport == INVALID_SOCKET || g_direct_agent_address.sin_port == 0 || state == NULL) return;
    length = _snprintf_s(message, sizeof(message), _TRUNCATE, "%s%s", WELNPT_TRANSPORT_STATE_PREFIX, state);
    if (length > 0) g_real_sendto(g_direct_transport, message, length, 0,
        (const struct sockaddr *)&g_direct_agent_address, sizeof(g_direct_agent_address));
}

static void reset_game_session(const char *reason) {
    LONG generation;
    int had_session;
    EnterCriticalSection(&g_state_lock);
    had_session = g_direct_transaction_peer_ip != 0 || g_direct_transaction_join_port != 0;
    generation = InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0);
    g_direct_peer_ip = 0;
    g_direct_transaction_peer_ip = 0;
    g_direct_transaction_join_port = 0;
    g_ice_decision_deadline = 0;
    g_ice_decision_started = 0;
    g_ice_session_deadline = 0;
    LeaveCriticalSection(&g_state_lock);
    if (!had_session) return;
    InterlockedExchange(&g_direct_connected, 0);
    InterlockedExchange(&g_game_path, WELNPT_GAME_PATH_PENDING);
    notify_agent_transport_state("pending");
    log_line("\"api\":\"session-state\",\"state\":\"WAIT_JOIN\",\"generation\":%lu,\"reason\":\"%s\"",
        (unsigned long)generation, reason);
}

static void lock_relay_for_decision(const char *reason) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG elapsed = g_ice_decision_started != 0 && now >= g_ice_decision_started
        ? now - g_ice_decision_started : 0;
    if (InterlockedCompareExchange(&g_game_path, WELNPT_GAME_PATH_RELAY,
        WELNPT_GAME_PATH_PENDING) == WELNPT_GAME_PATH_PENDING) {
        g_ice_decision_deadline = 0;
        g_ice_session_deadline = 0;
        log_line("\"api\":\"ice-decision\",\"result\":\"relay\",\"reason\":\"%s\",\"elapsedMs\":%llu,\"windowMs\":%d,\"generation\":%lu",
            reason, (unsigned __int64)elapsed, 0,
            (unsigned long)InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0));
        log_line("\"api\":\"transport-lock\",\"path\":\"relay\",\"reason\":\"%s\"", reason);
        notify_agent_transport_state("relay");
        log_line("\"api\":\"session-state\",\"state\":\"SESSION_ACTIVE\",\"generation\":%lu,\"path\":\"relay\",\"reason\":\"%s\"",
            (unsigned long)InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0), reason);
    }
}

static void check_ice_decision_deadline(void) {
    ULONGLONG now = GetTickCount64();
    if (InterlockedCompareExchange(&g_game_path, 0, 0) != WELNPT_GAME_PATH_PENDING) return;
    if (g_ice_decision_deadline != 0 && now >= g_ice_decision_deadline) {
        lock_relay_for_decision("decision-timeout");
        g_ice_decision_deadline = 0;
    } else if (g_ice_session_deadline != 0 && now >= g_ice_session_deadline) {
        lock_relay_for_decision("remote-sdp-timeout");
        g_ice_session_deadline = 0;
    }
}

static int payload_has_prefix(const char *payload, int length, const unsigned char *prefix, size_t prefix_length) {
    return payload != NULL && length >= (int)prefix_length && memcmp(payload, prefix, prefix_length) == 0;
}

typedef enum welnpt_payload_kind {
    WELNPT_PAYLOAD_DATA = 0,
    WELNPT_PAYLOAD_JOIN,
    WELNPT_PAYLOAD_ACCEPT,
    WELNPT_PAYLOAD_SEARCH,
    WELNPT_PAYLOAD_DATA_64,
    WELNPT_PAYLOAD_DATA_84,
} welnpt_payload_kind;

static welnpt_payload_kind classify_payload(const char *payload, int length) {
    static const unsigned char prefix[] = { 0xe7, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char accept_prefix[] = { 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00 };
    if (length == WELNPT_GAME_JOIN_PAYLOAD_LENGTH && payload_has_prefix(payload, length, prefix, sizeof(prefix))) {
        return WELNPT_PAYLOAD_JOIN;
    }
    if (length == WELNPT_GAME_ACCEPT_PAYLOAD_LENGTH && payload_has_prefix(payload, length, accept_prefix, sizeof(accept_prefix))) {
        return WELNPT_PAYLOAD_ACCEPT;
    }
    if (length == WELNPT_GAME_JOIN_PAYLOAD_LENGTH) return WELNPT_PAYLOAD_DATA_64;
    if (length == WELNPT_GAME_ACCEPT_PAYLOAD_LENGTH) return WELNPT_PAYLOAD_DATA_84;
    if (length == 24 && payload_has_prefix(payload, length, prefix, sizeof(prefix))) return WELNPT_PAYLOAD_SEARCH;
    return WELNPT_PAYLOAD_DATA;
}

static welnpt_payload_kind classify_control_payload(const char *payload, int length) {
    if (length != 24 && length != WELNPT_GAME_JOIN_PAYLOAD_LENGTH &&
        length != WELNPT_GAME_ACCEPT_PAYLOAD_LENGTH) return WELNPT_PAYLOAD_DATA;
    return classify_payload(payload, length);
}

static const char *payload_kind_name(welnpt_payload_kind kind) {
    switch (kind) {
    case WELNPT_PAYLOAD_JOIN: return "join";
    case WELNPT_PAYLOAD_ACCEPT: return "accept";
    case WELNPT_PAYLOAD_SEARCH: return "search";
    case WELNPT_PAYLOAD_DATA_64: return "data-64";
    case WELNPT_PAYLOAD_DATA_84: return "data-84";
    default: return "data";
    }
}

static void payload_fingerprint(const char *payload, int length, char *head, size_t head_size,
    unsigned long *hash) {
    static const char hex[] = "0123456789abcdef";
    unsigned long value = 2166136261UL;
    size_t count = 0;
    int index;
    if (!g_diagnostic_log_enabled) {
        if (head_size > 0) head[0] = '\0';
        if (hash != NULL) *hash = 0;
        return;
    }
    if (head_size > 0) head[0] = '\0';
    if (payload == NULL || length <= 0 || head_size < 3) {
        if (hash != NULL) *hash = value;
        return;
    }
    count = (size_t)length;
    if (count > 24) count = 24;
    if (count > (head_size - 1) / 2) count = (head_size - 1) / 2;
    for (index = 0; index < length; ++index) value = (value ^ (unsigned char)payload[index]) * 16777619UL;
    for (index = 0; index < (int)count; ++index) {
        head[index * 2] = hex[((unsigned char)payload[index] >> 4) & 0x0f];
        head[index * 2 + 1] = hex[(unsigned char)payload[index] & 0x0f];
    }
    head[count * 2] = '\0';
    if (hash != NULL) *hash = value;
}

static void log_line_impl(const char *format, ...) {
    char message[768];
    char line[896];
    va_list arguments;
    HANDLE file;
    DWORD written;
    int length;

    if (!g_diagnostic_log_enabled || g_log_path[0] == L'\0' || !g_locks_initialized) return;
    va_start(arguments, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
    va_end(arguments);
    length = _snprintf_s(line, sizeof(line), _TRUNCATE, "{\"tick\":%lu,%s}\r\n",
        (unsigned long)GetTickCount(), message);
    if (length <= 0) return;
    EnterCriticalSection(&g_log_lock);
    file = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, line, (DWORD)length, &written, NULL);
        CloseHandle(file);
    }
    LeaveCriticalSection(&g_log_lock);
}

static virtual_socket *find_socket_locked(SOCKET handle) {
    size_t index;
    for (index = 0; index < ARRAYSIZE(g_sockets); ++index) {
        if (g_sockets[index].active && g_sockets[index].handle == handle) return &g_sockets[index];
    }
    return NULL;
}

static virtual_socket *find_socket_by_port_locked(unsigned short port) {
    unsigned index;
    if (port == 0) return NULL;
    index = g_port_index[port];
    if (index == 0 || index > ARRAYSIZE(g_sockets)) return NULL;
    --index;
    if (!g_sockets[index].active || g_sockets[index].logical_port != port) return NULL;
    return &g_sockets[index];
}

static int port_in_use_locked(unsigned short port, SOCKET except_handle) {
    virtual_socket *state;
    if (port == 0) return 0;
    state = find_socket_by_port_locked(port);
    return state != NULL && state->handle != except_handle;
}

static unsigned short allocate_port_locked(SOCKET handle) {
    unsigned attempts;
    for (attempts = 0; attempts < 16384; ++attempts) {
        unsigned short port = (unsigned short)(49152 +
            ((unsigned long)InterlockedIncrement(&g_next_port) % 16384));
        if (!port_in_use_locked(port, handle)) return port;
    }
    return 0;
}

static virtual_socket *register_socket(SOCKET handle, int family, int type) {
    size_t index;
    virtual_socket *state = NULL;
    if (handle == INVALID_SOCKET || family != AF_INET || type != SOCK_DGRAM) return NULL;
    EnterCriticalSection(&g_state_lock);
    for (index = 0; index < ARRAYSIZE(g_sockets); ++index) {
        if (!g_sockets[index].active) {
            state = &g_sockets[index];
            ZeroMemory(state, sizeof(*state));
            state->active = 1;
            state->handle = handle;
            if (g_light_mode && type == SOCK_DGRAM && family == AF_INET) {
                struct sockaddr_in local;
                state->light_transport = g_real_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (state->light_transport != INVALID_SOCKET) {
                    ZeroMemory(&local, sizeof(local));
                    local.sin_family = AF_INET;
                    local.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
                    local.sin_port = 0;
                    if (g_real_bind(state->light_transport, (const struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) {
                        g_real_closesocket(state->light_transport);
                        state->light_transport = INVALID_SOCKET;
                    } else {
                        struct sockaddr_in bound;
                        int bound_length = sizeof(bound);
                        u_long nonblocking = 1;
                        if (g_real_getsockname(state->light_transport,
                            (struct sockaddr *)&bound, &bound_length) == 0) {
                            state->light_local_port = ntohs(bound.sin_port);
                        }
                        ioctlsocket(state->light_transport, FIONBIO, &nonblocking);
                    }
                }
            }
            break;
        }
    }
    LeaveCriticalSection(&g_state_lock);
    if (state != NULL) log_line("\"api\":\"socket\",\"socket\":%llu",
        (unsigned __int64)handle);
    return state;
}

static int virtual_socket_port(SOCKET handle, unsigned short *port) {
    virtual_socket *state;
    int found = 0;
    unsigned short local_port = 0;
    int announce = 0;
    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state != NULL) {
        if (state->logical_port == 0) state->logical_port = allocate_port_locked(handle);
        if (state->logical_port != 0) {
            g_port_index[state->logical_port] = (unsigned char)((state - g_sockets) + 1);
        }
        *port = state->logical_port;
        found = state->logical_port != 0;
        if (found && g_light_mode && state->light_transport != INVALID_SOCKET) {
            local_port = state->light_local_port;
            if (local_port != 0 && state->light_map_port != state->logical_port) {
                state->light_map_port = state->logical_port;
                announce = 1;
            }
        }
    }
    LeaveCriticalSection(&g_state_lock);
    if (announce && found && g_light_mode && local_port != 0 && g_host_transport != INVALID_SOCKET) {
        welnpt_host_frame map;
        welnpt_initialize_host_frame(&map, WELNPT_HOST_FRAME_MAP);
        map.source_port = htons(*port);
        map.target_port = htons(local_port);
        g_real_sendto(g_host_transport, (const char *)&map, sizeof(map), 0,
            (const struct sockaddr *)&g_host_address, sizeof(g_host_address));
    }
    return found;
}

static void initialize_datagram_pool(void) {
    size_t index;
    g_datagram_free = NULL;
    for (index = ARRAYSIZE(g_datagram_pool); index > 0; --index) {
        g_datagram_pool[index - 1].next = g_datagram_free;
        g_datagram_free = &g_datagram_pool[index - 1];
    }
}

static virtual_datagram *acquire_datagram_locked(void) {
    virtual_datagram *item = g_datagram_free;
    if (item != NULL) {
        g_datagram_free = item->next;
    } else {
        /* Preserve the old queue capacity during an exceptional burst. The
           pool handles the normal path; overflow allocations are reclaimed
           immediately when the datagram is consumed. */
        item = (virtual_datagram *)HeapAlloc(GetProcessHeap(), 0, sizeof(*item));
    }
    return item;
}

static int is_pool_datagram(const virtual_datagram *item) {
    ULONG_PTR address = (ULONG_PTR)item;
    ULONG_PTR begin = (ULONG_PTR)&g_datagram_pool[0];
    ULONG_PTR end = (ULONG_PTR)&g_datagram_pool[ARRAYSIZE(g_datagram_pool)];
    return address >= begin && address < end && ((address - begin) % sizeof(g_datagram_pool[0])) == 0;
}

static void release_datagram_locked(virtual_datagram *item) {
    if (item == NULL) return;
    if (!is_pool_datagram(item)) {
        HeapFree(GetProcessHeap(), 0, item);
        return;
    }
    item->next = g_datagram_free;
    g_datagram_free = item;
}

static void update_max_queue(unsigned value) {
    LONG current;
    do {
        current = InterlockedCompareExchange(&g_stat_max_queue, 0, 0);
        if ((unsigned)current >= value) return;
    } while (InterlockedCompareExchange(&g_stat_max_queue, (LONG)value, current) != current);
}

static void free_queue(virtual_socket *state) {
    virtual_datagram *item = state->head;
    while (item != NULL) {
        virtual_datagram *next = item->next;
        release_datagram_locked(item);
        item = next;
    }
    state->head = NULL;
    state->tail = NULL;
    state->queued = 0;
}

static int remove_socket(SOCKET handle, unsigned short *logical_port) {
    virtual_socket *state;
    int found = 0;
    if (logical_port != NULL) *logical_port = 0;
    /* Take the queue lock first. Receivers use the same order, so a socket
       cannot be reused while its queued datagrams are being released. */
    EnterCriticalSection(&g_queue_lock);
    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state != NULL) {
        if (logical_port != NULL) *logical_port = state->logical_port;
        if (g_light_mode && state->logical_port != 0) notify_light_socket_close(state->logical_port);
        if (state->logical_port != 0 &&
            g_port_index[state->logical_port] == (unsigned char)((state - g_sockets) + 1)) {
            g_port_index[state->logical_port] = 0;
        }
        free_queue(state);
        if (state->light_transport != INVALID_SOCKET) {
            g_real_closesocket(state->light_transport);
            state->light_transport = INVALID_SOCKET;
        }
        ZeroMemory(state, sizeof(*state));
        found = 1;
    }
    LeaveCriticalSection(&g_state_lock);
    LeaveCriticalSection(&g_queue_lock);
    return found;
}

static int enqueue_datagram(const welnpt_packet_header *header, const char *payload, int length) {
    virtual_datagram *item;
    virtual_socket *state;
    unsigned short target_port = ntohs(header->target_port);

    /* Reserve a pool entry independently, then do the potentially larger
       payload copy without holding either shared lock. */
    EnterCriticalSection(&g_queue_lock);
    item = acquire_datagram_locked();
    LeaveCriticalSection(&g_queue_lock);
    if (item == NULL) {
        if (g_diagnostic_log_enabled) InterlockedIncrement(&g_stat_pool_drops);
        return 0;
    }
    ZeroMemory(item, sizeof(*item));
    item->source.sin_family = AF_INET;
    item->source.sin_addr.S_un.S_addr = header->source_ip;
    item->source.sin_port = header->source_port;
    item->length = length;
    if (length > 0) CopyMemory(item->payload, payload, (size_t)length);

    EnterCriticalSection(&g_queue_lock);
    EnterCriticalSection(&g_state_lock);
    state = find_socket_by_port_locked(target_port);
    if (state == NULL || state->queued >= WELNPT_MAX_QUEUED_DATAGRAMS) {
        if (g_diagnostic_log_enabled) InterlockedIncrement(&g_stat_queue_drops);
        LeaveCriticalSection(&g_state_lock);
        release_datagram_locked(item);
        LeaveCriticalSection(&g_queue_lock);
        return 0;
    }
    /* The socket table is now stable because close also takes queue_lock
       first. Keep queue ownership while releasing the short table lock. */
    LeaveCriticalSection(&g_state_lock);
    if (state->tail == NULL) state->head = item;
    else state->tail->next = item;
    state->tail = item;
    ++state->queued;
    if (g_diagnostic_log_enabled) update_max_queue(state->queued);
    LeaveCriticalSection(&g_queue_lock);
    return 1;
}

static int handle_transport_packet(char *packet, int received, const char *path) {
	welnpt_packet_header *header;
	int payload_length;
	int session_signal;
	welnpt_payload_kind kind;
	int new_session = 0;
	char payload_head[49];
	char peer_ip[INET_ADDRSTRLEN];
	unsigned long payload_hash;
	LONG selected_path = WELNPT_GAME_PATH_RELAY;
	if (received < (int)sizeof(welnpt_packet_header)) return 0;
	header = (welnpt_packet_header *)packet;
	payload_length = (int)ntohs(header->payload_length);
	if (!welnpt_valid_header(header) || header->type != WELNPT_PACKET_DATA ||
		memcmp(header->room, g_room, WELNPT_ROOM_LENGTH) != 0 ||
		(((header->flags & WELNPT_FLAG_BROADCAST) == 0) && header->target_ip != g_logical_ip) ||
		payload_length > WELNPT_MAX_PAYLOAD || received != (int)sizeof(*header) + payload_length) return 0;
	kind = classify_control_payload(packet + sizeof(*header), payload_length);
	if ((header->flags & WELNPT_FLAG_BROADCAST) != 0 && kind == WELNPT_PAYLOAD_SEARCH &&
		header->source_ip == g_direct_transaction_peer_ip) {
		reset_game_session("peer-search-broadcast");
	}
	session_signal = (header->flags & WELNPT_FLAG_BROADCAST) == 0 &&
		(kind == WELNPT_PAYLOAD_JOIN || kind == WELNPT_PAYLOAD_ACCEPT);
	if (session_signal) {
		unsigned short source_port = ntohs(header->source_port);
		unsigned short target_port = ntohs(header->target_port);
		unsigned short join_port = kind == WELNPT_PAYLOAD_JOIN ? source_port : target_port;
		new_session = report_game_peer(header->source_ip, join_port, source_port, target_port);
		if (g_diagnostic_log_enabled) {
			payload_fingerprint(packet + sizeof(*header), payload_length, payload_head, sizeof(payload_head), &payload_hash);
			if (InetNtopA(AF_INET, &header->source_ip, peer_ip, sizeof(peer_ip)) == NULL) strcpy_s(peer_ip, sizeof(peer_ip), "unknown");
			log_line("\"api\":\"session-signal\",\"direction\":\"receive\",\"kind\":\"%s\",\"sequence\":%lu,\"length\":%d,\"newSession\":%s,\"peerIp\":\"%s\",\"sourcePort\":%u,\"targetPort\":%u,\"payloadHead\":\"%s\",\"payloadHash\":\"%08lx\"",
				payload_kind_name(kind), (unsigned long)ntohl(header->sequence), payload_length, new_session ? "true" : "false",
				peer_ip, (unsigned)source_port, (unsigned)target_port, payload_head, payload_hash);
		}
	}
	if ((header->flags & WELNPT_FLAG_BROADCAST) == 0 && header->source_ip == g_direct_transaction_peer_ip) {
		selected_path = InterlockedCompareExchange(&g_game_path, 0, 0);
		if ((strcmp(path, "relay") == 0 && selected_path == WELNPT_GAME_PATH_DIRECT) ||
			(strcmp(path, "direct") == 0 && selected_path != WELNPT_GAME_PATH_DIRECT)) {
			log_line("\"api\":\"transport-ignore\",\"path\":\"%s\",\"reason\":\"match-path-locked\"", path);
			return 1;
		}
	}
    if (!enqueue_datagram(header, packet + sizeof(*header), payload_length)) {
		log_line("\"api\":\"transport-drop\",\"path\":\"%s\",\"targetPort\":%u,\"length\":%d",
			path, (unsigned)ntohs(header->target_port), payload_length);
        return 0;
    }
	if (g_diagnostic_log_enabled) {
		if (strcmp(path, "direct") == 0) InterlockedIncrement(&g_stat_recv_direct);
		else InterlockedIncrement(&g_stat_recv_relay);
	}
	if (g_diagnostic_log_enabled) {
		payload_fingerprint(packet + sizeof(*header), payload_length, payload_head, sizeof(payload_head), &payload_hash);
		log_line("\"api\":\"transport-recv\",\"path\":\"%s\",\"broadcast\":%s,\"sequence\":%lu,\"sourcePort\":%u,\"length\":%d,\"packetKind\":\"%s\",\"payloadHead\":\"%s\",\"payloadHash\":\"%08lx\"",
			path, (header->flags & WELNPT_FLAG_BROADCAST) != 0 ? "true" : "false",
			(unsigned long)ntohl(header->sequence), (unsigned)ntohs(header->source_port), payload_length,
			payload_kind_name(kind), payload_head, payload_hash);
	}
	return 1;
}

/*
 * Light mode keeps the game process on a small loopback protocol. The Host
 * process owns WNP3 framing, relay traffic and ICE traffic;
 * this receive worker only restores a datagram to the game's virtual socket.
 */
static DWORD WINAPI light_receive_thread(LPVOID unused) {
    char packet[sizeof(welnpt_host_frame) + WELNPT_MAX_PAYLOAD];
    (void)unused;
    while (InterlockedCompareExchange(&g_stopping, 0, 0) == 0) {
        struct sockaddr_in source;
        int source_length = sizeof(source);
        int received = g_real_recvfrom(g_host_transport, packet, sizeof(packet), 0,
            (struct sockaddr *)&source, &source_length);
        welnpt_host_frame *frame;
        int payload_length;
        if (received > (int)strlen(WELNPT_TRANSPORT_STATE_PREFIX) &&
            memcmp(packet, WELNPT_TRANSPORT_STATE_PREFIX, strlen(WELNPT_TRANSPORT_STATE_PREFIX)) == 0) {
            log_line("\"api\":\"transport-state\",\"state\":\"%.*s\"",
                received - (int)strlen(WELNPT_TRANSPORT_STATE_PREFIX),
                packet + strlen(WELNPT_TRANSPORT_STATE_PREFIX));
            continue;
        }
        if (received > (int)strlen(WELNPT_ICE_STATE_PREFIX) &&
            memcmp(packet, WELNPT_ICE_STATE_PREFIX, strlen(WELNPT_ICE_STATE_PREFIX)) == 0) {
            log_line("\"api\":\"direct-state\",\"state\":\"%.*s\"",
                received - (int)strlen(WELNPT_ICE_STATE_PREFIX),
                packet + strlen(WELNPT_ICE_STATE_PREFIX));
            continue;
        }
        if (received < (int)sizeof(welnpt_host_frame)) continue;
        frame = (welnpt_host_frame *)packet;
        if (!welnpt_valid_host_frame(frame) || frame->type != WELNPT_HOST_FRAME_DATA) continue;
        payload_length = (int)ntohs(frame->payload_length);
        if (payload_length < 0 || payload_length > WELNPT_MAX_PAYLOAD ||
            received != (int)sizeof(*frame) + payload_length) continue;
        {
            virtual_socket *target_state;
            SOCKET target_socket = INVALID_SOCKET;
            unsigned short target_local_port = 0;
            unsigned short target_port = ntohs(frame->target_port);
            EnterCriticalSection(&g_state_lock);
            target_state = find_socket_by_port_locked(target_port);
            if (target_state != NULL) {
                target_socket = target_state->light_transport;
                target_local_port = target_state->light_local_port;
            }
            LeaveCriticalSection(&g_state_lock);
            if (target_socket != INVALID_SOCKET && target_local_port != 0) {
                struct sockaddr_in target_address;
                ZeroMemory(&target_address, sizeof(target_address));
                target_address.sin_family = AF_INET;
                target_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
                target_address.sin_port = htons(target_local_port);
                g_real_sendto(target_socket, packet, received, 0,
                    (const struct sockaddr *)&target_address, sizeof(target_address));
            }
        }
    }
    return 0;
}

static int send_light_datagram(SOCKET handle, const char *payload, int length,
    const struct sockaddr *destination, int destination_length) {
    char packet[sizeof(welnpt_host_frame) + WELNPT_MAX_PAYLOAD];
    welnpt_host_frame *frame = (welnpt_host_frame *)packet;
    const struct sockaddr_in *target;
    unsigned short source_port;
    int sent;

    if (g_host_transport == INVALID_SOCKET || destination == NULL ||
        destination_length < (int)sizeof(struct sockaddr_in) ||
        destination->sa_family != AF_INET || length < 0 || length > WELNPT_MAX_PAYLOAD) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    if (!virtual_socket_port(handle, &source_port)) {
        WSASetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    target = (const struct sockaddr_in *)destination;
    welnpt_initialize_host_frame(frame, WELNPT_HOST_FRAME_DATA);
    frame->source_ip = g_logical_ip;
    frame->source_port = htons(source_port);
    frame->target_ip = target->sin_addr.S_un.S_addr;
    frame->target_port = target->sin_port;
    frame->payload_length = htons((u_short)length);
    frame->sequence = htonl((u_long)InterlockedIncrement(&g_sequence));
    if (target->sin_addr.S_un.S_addr == INADDR_BROADCAST) frame->flags |= WELNPT_FLAG_BROADCAST;
    if (length > 0) CopyMemory(packet + sizeof(*frame), payload, (size_t)length);
    sent = g_real_sendto(g_host_transport, packet, (int)sizeof(*frame) + length, 0,
        (const struct sockaddr *)&g_host_address, sizeof(g_host_address));
    if (sent == SOCKET_ERROR) return SOCKET_ERROR;
    WSASetLastError(0);
    return length;
}

static void notify_light_socket_close(unsigned short logical_port) {
    welnpt_host_frame frame;
    if (!g_light_mode || g_host_transport == INVALID_SOCKET || logical_port == 0) return;
    welnpt_initialize_host_frame(&frame, WELNPT_HOST_FRAME_CLOSE);
    frame.source_port = htons(logical_port);
    g_real_sendto(g_host_transport, (const char *)&frame, sizeof(frame), 0,
        (const struct sockaddr *)&g_host_address, sizeof(g_host_address));
}

static int send_register_packet(void) {
    welnpt_packet_header header;
    welnpt_initialize_header(&header, WELNPT_PACKET_REGISTER);
    CopyMemory(header.room, g_room, WELNPT_ROOM_LENGTH);
    header.source_ip = g_logical_ip;
    return g_real_sendto(g_transport, (const char *)&header, sizeof(header), 0,
        (const struct sockaddr *)&g_relay_address, sizeof(g_relay_address));
}

static int report_game_peer(uint32_t target_ip, unsigned short join_port,
    unsigned short observed_source_port, unsigned short observed_target_port) {
    char target[INET_ADDRSTRLEN];
    char message[112];
    int length;
    int is_new_transaction;
    LONG generation;
    if (g_direct_transport == INVALID_SOCKET || g_direct_agent_address.sin_port == 0 || target_ip == 0) return 0;
    if (InetNtopA(AF_INET, &target_ip, target, sizeof(target)) == NULL) return 0;
    EnterCriticalSection(&g_state_lock);
	is_new_transaction = g_direct_transaction_peer_ip != target_ip ||
		g_direct_transaction_join_port != join_port;
	if (is_new_transaction) {
		g_direct_transaction_peer_ip = target_ip;
		g_direct_transaction_join_port = join_port;
		InterlockedIncrement(&g_direct_transaction_generation);
	}
    generation = InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0);
    LeaveCriticalSection(&g_state_lock);
    if (!is_new_transaction) return 0;
    /* A new peer or join Socket starts a new connection session. Same-key
       64/84-byte packets are data in the current session, not a new match. */
    g_direct_peer_ip = 0;
    InterlockedExchange(&g_direct_connected, 0);
    InterlockedExchange(&g_game_path, WELNPT_GAME_PATH_PENDING);
    g_ice_decision_started = GetTickCount64();
    g_ice_decision_deadline = 0;
    g_ice_session_deadline = g_ice_decision_started + WELNPT_ICE_SESSION_TIMEOUT_MS;
	notify_agent_transport_state("pending");
	length = _snprintf_s(message, sizeof(message), _TRUNCATE, "%s%s|%u|%u|%lu", WELNPT_GAME_PEER_PREFIX,
		target, (unsigned)join_port, (unsigned)join_port, (unsigned long)generation);
	if (length <= 0) return 0;
	g_real_sendto(g_direct_transport, message, length, 0,
		(const struct sockaddr *)&g_direct_agent_address, sizeof(g_direct_agent_address));
	log_line("\"api\":\"direct-target\",\"target\":\"%s\",\"joinPort\":%u,\"generation\":%lu,\"observedSourcePort\":%u,\"observedTargetPort\":%u",
		target, (unsigned)join_port, (unsigned long)generation,
		(unsigned)observed_source_port, (unsigned)observed_target_port);
	log_line("\"api\":\"session-state\",\"state\":\"SESSION_NEGOTIATING\",\"generation\":%lu,\"reason\":\"peer-or-join-port-changed\"",
		(unsigned long)generation);
		log_line("\"api\":\"ice-decision\",\"result\":\"pending\",\"reason\":\"session-start\",\"windowMs\":0,\"sessionTimeoutMs\":%d,\"generation\":%lu",
			WELNPT_ICE_SESSION_TIMEOUT_MS, (unsigned long)generation);
    return 1;
}

static int send_virtual_datagram(SOCKET handle, const char *payload, int length,
    const struct sockaddr *destination, int destination_length) {
    char packet[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
    welnpt_packet_header *header = (welnpt_packet_header *)packet;
    const struct sockaddr_in *target;
    unsigned short source_port;
	int sent;
	int session_signal;
	welnpt_payload_kind kind;
	int new_session = 0;
	char payload_head[49];
	char peer_ip[INET_ADDRSTRLEN];
	unsigned long payload_hash;
	LONG selected_path = WELNPT_GAME_PATH_RELAY;
	LONG direct_ready = 0;

    if (g_light_mode) return send_light_datagram(handle, payload, length, destination, destination_length);

    if (destination == NULL || destination_length < (int)sizeof(struct sockaddr_in) ||
        destination->sa_family != AF_INET || length < 0 || length > WELNPT_MAX_PAYLOAD) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    if (!virtual_socket_port(handle, &source_port)) {
        WSASetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    target = (const struct sockaddr_in *)destination;
    welnpt_initialize_header(header, WELNPT_PACKET_DATA);
    CopyMemory(header->room, g_room, WELNPT_ROOM_LENGTH);
    header->source_ip = g_logical_ip;
    header->source_port = htons(source_port);
    header->target_ip = target->sin_addr.S_un.S_addr;
    header->target_port = target->sin_port;
    header->payload_length = htons((u_short)length);
    header->sequence = htonl((u_long)InterlockedIncrement(&g_sequence));
    if (target->sin_addr.S_un.S_addr == INADDR_BROADCAST) header->flags |= WELNPT_FLAG_BROADCAST;
    if (length > 0) CopyMemory(packet + sizeof(*header), payload, (size_t)length);
	kind = classify_control_payload(payload, length);
	if ((header->flags & WELNPT_FLAG_BROADCAST) != 0 && kind == WELNPT_PAYLOAD_SEARCH) {
		reset_game_session("search-broadcast");
	}
	session_signal = (header->flags & WELNPT_FLAG_BROADCAST) == 0 &&
		(kind == WELNPT_PAYLOAD_JOIN || kind == WELNPT_PAYLOAD_ACCEPT);
	if (session_signal) {
		unsigned short target_port = ntohs(target->sin_port);
		unsigned short join_port = kind == WELNPT_PAYLOAD_JOIN ? source_port : target_port;
		new_session = report_game_peer(header->target_ip, join_port, source_port, target_port);
		if (g_diagnostic_log_enabled) {
			payload_fingerprint(payload, length, payload_head, sizeof(payload_head), &payload_hash);
			if (InetNtopA(AF_INET, &header->target_ip, peer_ip, sizeof(peer_ip)) == NULL) strcpy_s(peer_ip, sizeof(peer_ip), "unknown");
			log_line("\"api\":\"session-signal\",\"direction\":\"send\",\"kind\":\"%s\",\"sequence\":%lu,\"length\":%d,\"newSession\":%s,\"peerIp\":\"%s\",\"sourcePort\":%u,\"targetPort\":%u,\"payloadHead\":\"%s\",\"payloadHash\":\"%08lx\"",
				payload_kind_name(kind), (unsigned long)ntohl(header->sequence), length, new_session ? "true" : "false",
				peer_ip, (unsigned)source_port, (unsigned)target_port, payload_head, payload_hash);
		}
	}
	if ((header->flags & WELNPT_FLAG_BROADCAST) == 0) {
		selected_path = InterlockedCompareExchange(&g_game_path, 0, 0);
		direct_ready = InterlockedCompareExchange(&g_direct_connected, 0, 0);
	}
	if ((header->flags & WELNPT_FLAG_BROADCAST) == 0 &&
		g_direct_transport != INVALID_SOCKET && header->target_ip == g_direct_peer_ip &&
		selected_path == WELNPT_GAME_PATH_DIRECT && direct_ready != 0) {
        sent = g_real_sendto(g_direct_transport, packet, (int)sizeof(*header) + length, 0,
            (const struct sockaddr *)&g_direct_agent_address, sizeof(g_direct_agent_address));
        if (sent != SOCKET_ERROR) {
			if (g_diagnostic_log_enabled) {
				InterlockedIncrement(&g_stat_send_direct);
				maybe_log_stats();
			}
			if (g_diagnostic_log_enabled) {
				payload_fingerprint(payload, length, payload_head, sizeof(payload_head), &payload_hash);
				log_line("\"api\":\"sendto\",\"path\":\"direct\",\"socket\":%llu,\"sequence\":%lu,\"sourcePort\":%u,\"targetPort\":%u,\"length\":%d,\"packetKind\":\"%s\",\"payloadHead\":\"%s\",\"payloadHash\":\"%08lx\"",
					(unsigned __int64)handle, (unsigned long)ntohl(header->sequence), (unsigned)source_port, (unsigned)ntohs(target->sin_port), length,
					payload_kind_name(kind), payload_head, payload_hash);
			}
            WSASetLastError(0);
            return length;
        }
        InterlockedExchange(&g_direct_connected, 0);
        InterlockedExchange(&g_game_path, WELNPT_GAME_PATH_RELAY);
        notify_agent_transport_state("relay");
        log_line("\"api\":\"direct-fallback\",\"reason\":\"local-send-failed\"");
        log_line("\"api\":\"transport-lock\",\"path\":\"relay\",\"reason\":\"direct-send-failed\"");
    }
    sent = g_real_sendto(g_transport, packet, (int)sizeof(*header) + length, 0,
        (const struct sockaddr *)&g_relay_address, sizeof(g_relay_address));
    if (sent == SOCKET_ERROR) return SOCKET_ERROR;
	if (g_diagnostic_log_enabled) {
		InterlockedIncrement(&g_stat_send_relay);
		maybe_log_stats();
	}
	if (g_diagnostic_log_enabled) {
		payload_fingerprint(payload, length, payload_head, sizeof(payload_head), &payload_hash);
		log_line("\"api\":\"sendto\",\"path\":\"relay\",\"broadcast\":%s,\"socket\":%llu,\"sequence\":%lu,\"sourcePort\":%u,\"targetPort\":%u,\"length\":%d,\"packetKind\":\"%s\",\"payloadHead\":\"%s\",\"payloadHash\":\"%08lx\"",
			(header->flags & WELNPT_FLAG_BROADCAST) != 0 ? "true" : "false",
			(unsigned __int64)handle, (unsigned long)ntohl(header->sequence), (unsigned)source_port,
			(unsigned)ntohs(target->sin_port), length,
			payload_kind_name(kind), payload_head, payload_hash);
	}
    WSASetLastError(0);
    return length;
}

static int receive_virtual_datagram(SOCKET handle, char *buffer, int length, int flags,
    struct sockaddr *source, int *source_length) {
    virtual_socket *state;
    virtual_datagram *item;
    int result;
    int item_length;
    int peek = (flags & MSG_PEEK) != 0;

    if (g_light_mode) {
        char packet[sizeof(welnpt_host_frame) + WELNPT_MAX_PAYLOAD];
        struct sockaddr_in local_source;
        int local_source_length = sizeof(local_source);
        SOCKET light_socket = INVALID_SOCKET;
        welnpt_host_frame *frame;
        int payload_length;
        int result;
        EnterCriticalSection(&g_state_lock);
        state = find_socket_locked(handle);
        if (state != NULL) light_socket = state->light_transport;
        LeaveCriticalSection(&g_state_lock);
        if (light_socket == INVALID_SOCKET) return -2;
        result = g_real_recvfrom(light_socket, packet, sizeof(packet), peek ? MSG_PEEK : 0,
            (struct sockaddr *)&local_source, &local_source_length);
        if (result == SOCKET_ERROR) return SOCKET_ERROR;
        if (result < (int)sizeof(welnpt_host_frame)) {
            WSASetLastError(WSAEINVAL);
            return SOCKET_ERROR;
        }
        frame = (welnpt_host_frame *)packet;
        payload_length = (int)ntohs(frame->payload_length);
        if (!welnpt_valid_host_frame(frame) || frame->type != WELNPT_HOST_FRAME_DATA ||
            payload_length < 0 || payload_length > WELNPT_MAX_PAYLOAD ||
            result != (int)sizeof(*frame) + payload_length) {
            if (!peek) {
                /* The malformed frame has already been consumed from the local
                   socket. Do not expose it to the game. */
            }
            WSASetLastError(WSAEINVAL);
            return SOCKET_ERROR;
        }
        if (source != NULL && source_length != NULL) {
            struct sockaddr_in remote;
            if (*source_length < (int)sizeof(remote)) {
                WSASetLastError(WSAEFAULT);
                return SOCKET_ERROR;
            }
            ZeroMemory(&remote, sizeof(remote));
            remote.sin_family = AF_INET;
            remote.sin_addr.S_un.S_addr = frame->source_ip;
            remote.sin_port = frame->source_port;
            CopyMemory(source, &remote, sizeof(remote));
            *source_length = sizeof(remote);
        }
        if (length < payload_length) {
            WSASetLastError(WSAEMSGSIZE);
            return SOCKET_ERROR;
        }
        if (payload_length > 0 && buffer != NULL) CopyMemory(buffer, packet + sizeof(*frame), (size_t)payload_length);
        WSASetLastError(0);
        return payload_length;
    }

    EnterCriticalSection(&g_queue_lock);
    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state == NULL) {
        LeaveCriticalSection(&g_state_lock);
        LeaveCriticalSection(&g_queue_lock);
        return -2;
    }
    item = state->head;
    if (item == NULL) {
        LeaveCriticalSection(&g_state_lock);
        LeaveCriticalSection(&g_queue_lock);
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }
    if (!peek) {
        state->head = item->next;
        if (state->head == NULL) state->tail = NULL;
        --state->queued;
    }
    item_length = item->length;
    result = item_length;
    if (length < result) result = length;
    if (!peek) {
        /* Once detached, the datagram no longer needs the queue lock. Keep
           its pool slot until the copy completes, then return it to the pool. */
        LeaveCriticalSection(&g_state_lock);
        LeaveCriticalSection(&g_queue_lock);
    }
    if (result > 0 && buffer != NULL) CopyMemory(buffer, item->payload, (size_t)result);
    if (source != NULL && source_length != NULL) {
        if (*source_length < (int)sizeof(struct sockaddr_in)) {
            if (peek) {
                LeaveCriticalSection(&g_state_lock);
                LeaveCriticalSection(&g_queue_lock);
            } else {
                EnterCriticalSection(&g_queue_lock);
                release_datagram_locked(item);
                LeaveCriticalSection(&g_queue_lock);
            }
            WSASetLastError(WSAEFAULT);
            return SOCKET_ERROR;
        }
        CopyMemory(source, &item->source, sizeof(item->source));
        *source_length = sizeof(item->source);
    }
    {
        unsigned short source_port = ntohs(item->source.sin_port);
        if (peek) {
            LeaveCriticalSection(&g_state_lock);
            LeaveCriticalSection(&g_queue_lock);
        } else {
            EnterCriticalSection(&g_queue_lock);
            release_datagram_locked(item);
            LeaveCriticalSection(&g_queue_lock);
        }
        if (g_diagnostic_log_enabled) {
            log_line("\"api\":\"recvfrom\",\"socket\":%llu,\"sourcePort\":%u,\"length\":%d",
                (unsigned __int64)handle, (unsigned)source_port, item_length);
        }
    }
    if (length < item_length) {
        WSASetLastError(WSAEMSGSIZE);
        return SOCKET_ERROR;
    }
    if (g_diagnostic_log_enabled) maybe_log_stats();
    WSASetLastError(0);
    return result;
}

static SOCKET WSAAPI wel_socket(int family, int type, int protocol) {
    SOCKET result = g_real_socket(family, type, protocol);
    register_socket(result, family, type);
    return result;
}

static SOCKET WSAAPI wel_wsa_socket_a(int family, int type, int protocol,
    LPWSAPROTOCOL_INFOA info, GROUP group, DWORD flags) {
    SOCKET result = g_real_wsa_socket_a(family, type, protocol, info, group, flags);
    register_socket(result, family, type);
    return result;
}

static SOCKET WSAAPI wel_wsa_socket_w(int family, int type, int protocol,
    LPWSAPROTOCOL_INFOW info, GROUP group, DWORD flags) {
    SOCKET result = g_real_wsa_socket_w(family, type, protocol, info, group, flags);
    register_socket(result, family, type);
    return result;
}

static int WSAAPI wel_bind(SOCKET handle, const struct sockaddr *address, int address_length) {
    const struct sockaddr_in *ipv4;
    virtual_socket *state;
    unsigned short requested_port;

    if (address == NULL || address_length < (int)sizeof(struct sockaddr_in) || address->sa_family != AF_INET) {
        return g_real_bind(handle, address, address_length);
    }
    ipv4 = (const struct sockaddr_in *)address;
    requested_port = ntohs(ipv4->sin_port);
    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state == NULL) {
        LeaveCriticalSection(&g_state_lock);
        return g_real_bind(handle, address, address_length);
    }
    if (requested_port != 0 && port_in_use_locked(requested_port, handle)) {
        LeaveCriticalSection(&g_state_lock);
        WSASetLastError(WSAEADDRINUSE);
        return SOCKET_ERROR;
    }
    if (state->logical_port != 0 && state->logical_port != requested_port &&
        g_port_index[state->logical_port] == (unsigned char)((state - g_sockets) + 1)) {
        g_port_index[state->logical_port] = 0;
    }
    state->logical_port = requested_port != 0 ? requested_port : allocate_port_locked(handle);
    state->light_map_port = 0;
    if (state->logical_port != 0) {
        g_port_index[state->logical_port] = (unsigned char)((state - g_sockets) + 1);
    }
    requested_port = state->logical_port;
    LeaveCriticalSection(&g_state_lock);
    if (requested_port == 0) {
        WSASetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    log_line("\"api\":\"bind\",\"socket\":%llu,\"logicalPort\":%u",
        (unsigned __int64)handle, (unsigned)requested_port);
    if (g_light_mode) {
        unsigned short announced;
        virtual_socket_port(handle, &announced);
    }
    WSASetLastError(0);
    return 0;
}

static int WSAAPI wel_getsockname(SOCKET handle, struct sockaddr *name, int *name_length) {
    virtual_socket *state;
    struct sockaddr_in logical_address;
    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state == NULL) {
        LeaveCriticalSection(&g_state_lock);
        return g_real_getsockname(handle, name, name_length);
    }
    if (name == NULL || name_length == NULL || *name_length < (int)sizeof(logical_address)) {
        LeaveCriticalSection(&g_state_lock);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    ZeroMemory(&logical_address, sizeof(logical_address));
    logical_address.sin_family = AF_INET;
    logical_address.sin_addr.S_un.S_addr = g_logical_ip;
    logical_address.sin_port = htons(state->logical_port);
    CopyMemory(name, &logical_address, sizeof(logical_address));
    *name_length = sizeof(logical_address);
    LeaveCriticalSection(&g_state_lock);
    WSASetLastError(0);
    return 0;
}

static int WSAAPI wel_sendto(SOCKET handle, const char *buffer, int length, int flags,
    const struct sockaddr *destination, int destination_length) {
    unsigned short ignored;
    (void)flags;
    if (!virtual_socket_port(handle, &ignored)) {
        return g_real_sendto(handle, buffer, length, flags, destination, destination_length);
    }
    return send_virtual_datagram(handle, buffer, length, destination, destination_length);
}

static int WSAAPI wel_recvfrom(SOCKET handle, char *buffer, int length, int flags,
    struct sockaddr *source, int *source_length) {
    int result = receive_virtual_datagram(handle, buffer, length, flags, source, source_length);
    if (result == -2) return g_real_recvfrom(handle, buffer, length, flags, source, source_length);
    return result;
}

static int WSAAPI wel_wsasendto(SOCKET handle, LPWSABUF buffers, DWORD buffer_count,
    LPDWORD bytes_sent, DWORD flags, const struct sockaddr *destination, int destination_length,
    LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    char payload[WELNPT_MAX_PAYLOAD];
    DWORD index;
    DWORD total = 0;
    int result;
    unsigned short ignored;
    (void)flags;
    if (!virtual_socket_port(handle, &ignored)) {
        return g_real_wsasendto(handle, buffers, buffer_count, bytes_sent, flags, destination,
            destination_length, overlapped, completion);
    }
    if (overlapped != NULL || completion != NULL) {
        WSASetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }
    for (index = 0; index < buffer_count; ++index) {
        if (buffers[index].len > WELNPT_MAX_PAYLOAD - total) {
            WSASetLastError(WSAEMSGSIZE);
            return SOCKET_ERROR;
        }
        CopyMemory(payload + total, buffers[index].buf, buffers[index].len);
        total += buffers[index].len;
    }
    result = send_virtual_datagram(handle, payload, (int)total, destination, destination_length);
    if (result == SOCKET_ERROR) return SOCKET_ERROR;
    if (bytes_sent != NULL) *bytes_sent = (DWORD)result;
    return 0;
}

static int WSAAPI wel_wsarecvfrom(SOCKET handle, LPWSABUF buffers, DWORD buffer_count,
    LPDWORD bytes_received, LPDWORD flags, struct sockaddr *source, LPINT source_length,
    LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    int result;
    int receive_flags = flags == NULL ? 0 : (int)*flags;
    unsigned short ignored;
    if (!virtual_socket_port(handle, &ignored)) {
        return g_real_wsarecvfrom(handle, buffers, buffer_count, bytes_received, flags, source,
            source_length, overlapped, completion);
    }
    if (overlapped != NULL || completion != NULL || buffer_count == 0) {
        WSASetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }
    result = receive_virtual_datagram(handle, buffers[0].buf, (int)buffers[0].len,
        receive_flags, source, source_length);
    if (result == SOCKET_ERROR) return SOCKET_ERROR;
    if (bytes_received != NULL) *bytes_received = (DWORD)result;
    return 0;
}

static int WSAAPI wel_closesocket(SOCKET handle) {
    unsigned short logical_port = 0;
    if (remove_socket(handle, &logical_port) && logical_port != 0 &&
        logical_port == g_direct_transaction_join_port) {
        reset_game_session("match-socket-closed");
    }
    return g_real_closesocket(handle);
}

static void patch_import_slot(PULONG_PTR slot, ULONG_PTR replacement) {
    DWORD old_protection;
    if (*slot == replacement || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) return;
    *slot = replacement;
    VirtualProtect(slot, sizeof(*slot), old_protection, &old_protection);
}

static void patch_named_import(PULONG_PTR slot, const char *name) {
    if (strcmp(name, "socket") == 0) patch_import_slot(slot, (ULONG_PTR)wel_socket);
    else if (strcmp(name, "WSASocketA") == 0) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_a);
    else if (strcmp(name, "WSASocketW") == 0) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_w);
    else if (strcmp(name, "bind") == 0) patch_import_slot(slot, (ULONG_PTR)wel_bind);
    else if (strcmp(name, "getsockname") == 0) patch_import_slot(slot, (ULONG_PTR)wel_getsockname);
    else if (strcmp(name, "sendto") == 0) patch_import_slot(slot, (ULONG_PTR)wel_sendto);
    else if (strcmp(name, "recvfrom") == 0) patch_import_slot(slot, (ULONG_PTR)wel_recvfrom);
    else if (strcmp(name, "WSASendTo") == 0) patch_import_slot(slot, (ULONG_PTR)wel_wsasendto);
    else if (strcmp(name, "WSARecvFrom") == 0) patch_import_slot(slot, (ULONG_PTR)wel_wsarecvfrom);
    else if (strcmp(name, "closesocket") == 0) patch_import_slot(slot, (ULONG_PTR)wel_closesocket);
}

static void patch_address_import(PULONG_PTR slot) {
    if (*slot == (ULONG_PTR)g_real_socket) patch_import_slot(slot, (ULONG_PTR)wel_socket);
    else if (*slot == (ULONG_PTR)g_real_wsa_socket_a) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_a);
    else if (*slot == (ULONG_PTR)g_real_wsa_socket_w) patch_import_slot(slot, (ULONG_PTR)wel_wsa_socket_w);
    else if (*slot == (ULONG_PTR)g_real_bind) patch_import_slot(slot, (ULONG_PTR)wel_bind);
    else if (*slot == (ULONG_PTR)g_real_getsockname) patch_import_slot(slot, (ULONG_PTR)wel_getsockname);
    else if (*slot == (ULONG_PTR)g_real_sendto) patch_import_slot(slot, (ULONG_PTR)wel_sendto);
    else if (*slot == (ULONG_PTR)g_real_recvfrom) patch_import_slot(slot, (ULONG_PTR)wel_recvfrom);
    else if (*slot == (ULONG_PTR)g_real_wsasendto) patch_import_slot(slot, (ULONG_PTR)wel_wsasendto);
    else if (*slot == (ULONG_PTR)g_real_wsarecvfrom) patch_import_slot(slot, (ULONG_PTR)wel_wsarecvfrom);
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
                    PIMAGE_IMPORT_BY_NAME imported =
                        (PIMAGE_IMPORT_BY_NAME)((BYTE *)module + names->u1.AddressOfData);
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
        Sleep(WELNPT_MODULE_SCAN_MS);
    }
    return 0;
}

static DWORD WINAPI relay_receive_thread(LPVOID unused) {
    char packet[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
    ULONGLONG last_register = 0;
    (void)unused;
    while (InterlockedCompareExchange(&g_stopping, 0, 0) == 0) {
        struct sockaddr_in source;
        int source_length = sizeof(source);
        int received;
        ULONGLONG now = GetTickCount64();
        if (now - last_register >= WELNPT_HEARTBEAT_MS) {
            send_register_packet();
            last_register = now;
        }
        received = g_real_recvfrom(g_transport, packet, sizeof(packet), 0,
            (struct sockaddr *)&source, &source_length);
        if (received > 0) handle_transport_packet(packet, received, "relay");
        if (g_diagnostic_log_enabled) maybe_log_stats();
    }
    return 0;
}

static DWORD WINAPI direct_receive_thread(LPVOID unused) {
    char packet[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
    (void)unused;
    while (InterlockedCompareExchange(&g_stopping, 0, 0) == 0) {
        struct sockaddr_in source;
        int source_length = sizeof(source);
        int received = g_real_recvfrom(g_direct_transport, packet, sizeof(packet), 0,
            (struct sockaddr *)&source, &source_length);
        if (received <= 0) {
            check_ice_decision_deadline();
            continue;
        }
        check_ice_decision_deadline();
        if (received > (int)strlen(WELNPT_ICE_AGENT_PREFIX) &&
            memcmp(packet, WELNPT_ICE_AGENT_PREFIX, strlen(WELNPT_ICE_AGENT_PREFIX)) == 0) {
            unsigned long port = strtoul(packet + strlen(WELNPT_ICE_AGENT_PREFIX), NULL, 10);
            if (port > 0 && port <= 65535) {
                unsigned short previous_port = ntohs(g_direct_agent_address.sin_port);
                g_direct_agent_address.sin_port = htons((u_short)port);
                InterlockedExchange(&g_direct_connected, 0);
                if (previous_port != 0 && previous_port != (unsigned short)port &&
                    InterlockedCompareExchange(&g_game_path, 0, 0) == WELNPT_GAME_PATH_RELAY) {
                    /* A new agent is a fresh ICE attempt for the same peer.
                       Relay remains usable until this attempt connects. */
                    InterlockedExchange(&g_game_path, WELNPT_GAME_PATH_PENDING);
                    g_ice_decision_started = GetTickCount64();
                    g_ice_decision_deadline = 0;
                    g_ice_session_deadline = g_ice_decision_started + WELNPT_ICE_SESSION_TIMEOUT_MS;
                    notify_agent_transport_state("pending");
                }
                log_line("\"api\":\"direct-agent\",\"port\":%lu", port);
            }
            continue;
        }
        if (received == (int)strlen(WELNPT_ICE_REMOTE_SET_PREFIX) &&
            memcmp(packet, WELNPT_ICE_REMOTE_SET_PREFIX, strlen(WELNPT_ICE_REMOTE_SET_PREFIX)) == 0) {
            if (InterlockedCompareExchange(&g_game_path, 0, 0) == WELNPT_GAME_PATH_PENDING) {
                g_ice_decision_started = GetTickCount64();
                /* No fixed five-second lock. Relay remains usable while ICE
                   is pending and a later connected state may upgrade it. */
                g_ice_decision_deadline = 0;
                log_line("\"api\":\"ice-decision\",\"result\":\"pending\",\"reason\":\"remote-set\",\"windowMs\":0,\"generation\":%lu",
                    (unsigned long)InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0));
            }
            continue;
        }
        if (received > (int)strlen(WELNPT_ICE_STATE_PREFIX) &&
            memcmp(packet, WELNPT_ICE_STATE_PREFIX, strlen(WELNPT_ICE_STATE_PREFIX)) == 0) {
            const char *state = packet + strlen(WELNPT_ICE_STATE_PREFIX);
            int connected = strncmp(state, "connected", 9) == 0 || strncmp(state, "completed", 9) == 0;
            int failed = strncmp(state, "failed", 6) == 0;
            int disconnected = strncmp(state, "disconnected", 12) == 0;
            LONG selected = InterlockedCompareExchange(&g_game_path, 0, 0);
            if (connected && (selected == WELNPT_GAME_PATH_PENDING || selected == WELNPT_GAME_PATH_RELAY)) {
                LONG changed = selected == WELNPT_GAME_PATH_PENDING
                    ? InterlockedCompareExchange(&g_game_path, WELNPT_GAME_PATH_DIRECT,
                        WELNPT_GAME_PATH_PENDING)
                    : InterlockedExchange(&g_game_path, WELNPT_GAME_PATH_DIRECT);
                if (changed == WELNPT_GAME_PATH_PENDING || changed == WELNPT_GAME_PATH_RELAY) {
                    ULONGLONG now = GetTickCount64();
                    ULONGLONG elapsed = g_ice_decision_started != 0 && now >= g_ice_decision_started
                        ? now - g_ice_decision_started : 0;
                    g_ice_decision_deadline = 0;
                    g_ice_session_deadline = 0;
                    log_line("\"api\":\"ice-decision\",\"result\":\"direct\",\"reason\":\"ice-connected\",\"elapsedMs\":%llu,\"windowMs\":0,\"generation\":%lu,\"directState\":\"%.*s\"",
                        (unsigned __int64)elapsed,
                        (unsigned long)InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0),
                        received - (int)strlen(WELNPT_ICE_STATE_PREFIX), state);
                    log_line("\"api\":\"transport-lock\",\"path\":\"direct\",\"reason\":\"ice-connected\"");
                    notify_agent_transport_state("direct");
                    log_line("\"api\":\"session-state\",\"state\":\"SESSION_ACTIVE\",\"generation\":%lu,\"reason\":\"ice-connected\"",
                        (unsigned long)InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0));
                }
                selected = InterlockedCompareExchange(&g_game_path, 0, 0);
            } else if (failed && selected == WELNPT_GAME_PATH_PENDING) {
                lock_relay_for_decision("ice-failed");
                selected = InterlockedCompareExchange(&g_game_path, 0, 0);
            } else if (failed && selected == WELNPT_GAME_PATH_DIRECT) {
                log_line("\"api\":\"direct-fallback\",\"reason\":\"peer-ice-failed\"");
                InterlockedExchange(&g_game_path, WELNPT_GAME_PATH_RELAY);
                selected = WELNPT_GAME_PATH_RELAY;
                notify_agent_transport_state("relay");
                log_line("\"api\":\"transport-lock\",\"path\":\"relay\",\"reason\":\"peer-ice-failed\"");
                log_line("\"api\":\"session-state\",\"state\":\"SESSION_ACTIVE\",\"generation\":%lu,\"path\":\"relay\",\"reason\":\"peer-ice-failed\"",
                    (unsigned long)InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0));
            } else if (disconnected && selected == WELNPT_GAME_PATH_DIRECT) {
                InterlockedExchange(&g_game_path, WELNPT_GAME_PATH_RELAY);
                selected = WELNPT_GAME_PATH_RELAY;
                notify_agent_transport_state("relay");
                log_line("\"api\":\"transport-lock\",\"path\":\"relay\",\"reason\":\"direct-disconnected\"");
                log_line("\"api\":\"session-state\",\"state\":\"SESSION_ACTIVE\",\"generation\":%lu,\"path\":\"relay\",\"reason\":\"direct-disconnected\"",
                    (unsigned long)InterlockedCompareExchange(&g_direct_transaction_generation, 0, 0));
            }
            InterlockedExchange(&g_direct_connected, connected && selected == WELNPT_GAME_PATH_DIRECT);
            log_line("\"api\":\"direct-state\",\"state\":\"%.*s\"", received - (int)strlen(WELNPT_ICE_STATE_PREFIX), state);
            continue;
        }
		if (received > (int)strlen(WELNPT_ICE_PEER_PREFIX) &&
			memcmp(packet, WELNPT_ICE_PEER_PREFIX, strlen(WELNPT_ICE_PEER_PREFIX)) == 0) {
			const char *peer = packet + strlen(WELNPT_ICE_PEER_PREFIX);
			char peer_text[INET_ADDRSTRLEN];
			int peer_length = received - (int)strlen(WELNPT_ICE_PEER_PREFIX);
			if (peer_length > 0 && peer_length < (int)sizeof(peer_text)) {
				CopyMemory(peer_text, peer, (size_t)peer_length);
				peer_text[peer_length] = '\0';
			}
			if (peer_length > 0 && peer_length < (int)sizeof(peer_text) &&
				InetPtonA(AF_INET, peer_text, &g_direct_peer_ip) == 1) {
				log_line("\"api\":\"direct-peer\",\"target\":\"%s\"", peer_text);
			}
			continue;
		}
        handle_transport_packet(packet, received, "direct");
		if (g_diagnostic_log_enabled) maybe_log_stats();
    }
    return 0;
}

static void signal_ready(void) {
    char name[128];
    DWORD length = GetEnvironmentVariableA("WEL_NOTAP_READY_EVENT", name, sizeof(name));
    HANDLE event;
    if (length == 0 || length >= sizeof(name)) return;
    event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (event != NULL) {
        SetEvent(event);
        CloseHandle(event);
    }
}

static int load_configuration(void) {
    char relay[256];
    char logical_ip[64];
    char direct_peer_ip[64];
    char direct_agent_port[16];
    char direct_hook_port[16];
    char *separator;
    char port[16];
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    DWORD room_length;
    unsigned short host_port = 0;

    if (GetEnvironmentVariableA("WEL_NOTAP_RELAY", relay, sizeof(relay)) == 0 ||
        GetEnvironmentVariableA("WEL_NOTAP_LOGICAL_IP", logical_ip, sizeof(logical_ip)) == 0) return 0;
    room_length = GetEnvironmentVariableA("WEL_NOTAP_ROOM", g_room, sizeof(g_room));
    if (room_length == 0 || room_length >= sizeof(g_room)) return 0;
    separator = strrchr(relay, ':');
    if (separator == NULL || separator == relay || separator[1] == '\0') return 0;
    strcpy_s(port, sizeof(port), separator + 1);
    *separator = '\0';
    if (InetPtonA(AF_INET, logical_ip, &g_logical_ip) != 1) return 0;
    parse_environment_port("WEL_NOTAP_HOST_PORT", &host_port);
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(relay, port, &hints, &addresses) != 0 || addresses == NULL) return 0;
    CopyMemory(&g_relay_address, addresses->ai_addr, sizeof(g_relay_address));
    freeaddrinfo(addresses);

    if (GetEnvironmentVariableA("WEL_NOTAP_DIRECT_AGENT_PORT", direct_agent_port, sizeof(direct_agent_port)) > 0 &&
        GetEnvironmentVariableA("WEL_NOTAP_DIRECT_HOOK_PORT", direct_hook_port, sizeof(direct_hook_port)) > 0) {
        unsigned long agent_port = strtoul(direct_agent_port, NULL, 10);
        unsigned long hook_port = strtoul(direct_hook_port, NULL, 10);
        if (agent_port > 0 && agent_port <= 65535 && hook_port > 0 && hook_port <= 65535) {
            ZeroMemory(&g_direct_agent_address, sizeof(g_direct_agent_address));
            g_direct_agent_address.sin_family = AF_INET;
            g_direct_agent_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
            g_direct_agent_address.sin_port = htons((u_short)agent_port);
            g_direct_hook_port = (unsigned short)hook_port;
			if (GetEnvironmentVariableA("WEL_NOTAP_DIRECT_PEER_IP", direct_peer_ip, sizeof(direct_peer_ip)) > 0) {
				InetPtonA(AF_INET, direct_peer_ip, &g_direct_peer_ip);
			}
        } else {
            g_direct_peer_ip = 0;
        }
    }
    return 1;
}

static int initialize_hook(void) {
    HMODULE winsock;
    WSADATA winsock_data;
    struct sockaddr_in local_address;
    struct sockaddr_in direct_address;
    DWORD timeout = 500;
    HANDLE worker;

    InitializeCriticalSection(&g_state_lock);
    InitializeCriticalSection(&g_queue_lock);
    InitializeCriticalSection(&g_log_lock);
    g_locks_initialized = 1;
    ZeroMemory(g_port_index, sizeof(g_port_index));
    initialize_datagram_pool();
    {
        char logging[16];
        DWORD length = GetEnvironmentVariableA("WEL_NOTAP_DIAGNOSTIC_LOG", logging, sizeof(logging));
        if (length > 0 && length < sizeof(logging) &&
            (_stricmp(logging, "true") == 0 || strcmp(logging, "1") == 0 || _stricmp(logging, "yes") == 0)) {
            g_diagnostic_log_enabled = 1;
            GetEnvironmentVariableW(L"WEL_NOTAP_LOG_PATH", g_log_path, ARRAYSIZE(g_log_path));
        }
    }
    g_next_port = (LONG)(GetTickCount() & 0x3fff);
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) return 0;
    if (!load_configuration()) return 0;
    winsock = GetModuleHandleW(L"ws2_32.dll");
    if (winsock == NULL) winsock = LoadLibraryW(L"ws2_32.dll");
    if (winsock == NULL) return 0;

    g_real_socket = (wel_socket_fn)GetProcAddress(winsock, "socket");
    g_real_wsa_socket_a = (wel_wsasocketa_fn)GetProcAddress(winsock, "WSASocketA");
    g_real_wsa_socket_w = (wel_wsasocketw_fn)GetProcAddress(winsock, "WSASocketW");
    g_real_bind = (wel_bind_fn)GetProcAddress(winsock, "bind");
    g_real_getsockname = (wel_getsockname_fn)GetProcAddress(winsock, "getsockname");
    g_real_sendto = (wel_sendto_fn)GetProcAddress(winsock, "sendto");
    g_real_recvfrom = (wel_recvfrom_fn)GetProcAddress(winsock, "recvfrom");
    g_real_wsasendto = (wel_wsasendto_fn)GetProcAddress(winsock, "WSASendTo");
    g_real_wsarecvfrom = (wel_wsarecvfrom_fn)GetProcAddress(winsock, "WSARecvFrom");
    g_real_closesocket = (wel_closesocket_fn)GetProcAddress(winsock, "closesocket");
    if (g_real_socket == NULL || g_real_bind == NULL || g_real_getsockname == NULL ||
        g_real_sendto == NULL || g_real_recvfrom == NULL || g_real_closesocket == NULL) return 0;

    /* The launcher starts welnpthost.exe before injecting us. If its loopback
       port is present, keep the game process on the lightweight local frame
       path and leave WNP3/ICE work to that external process. Without the
       variable, retain the original in-process transport for compatibility. */
    if (parse_environment_port("WEL_NOTAP_HOST_PORT", &g_host_port)) {
        ZeroMemory(&g_host_address, sizeof(g_host_address));
        g_host_address.sin_family = AF_INET;
        g_host_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        g_host_address.sin_port = htons(g_host_port);
        g_host_transport = g_real_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_host_transport == INVALID_SOCKET) return 0;
        ZeroMemory(&local_address, sizeof(local_address));
        local_address.sin_family = AF_INET;
        local_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        local_address.sin_port = 0;
        if (g_real_bind(g_host_transport, (const struct sockaddr *)&local_address, sizeof(local_address)) == SOCKET_ERROR) return 0;
        tune_transport_socket(g_host_transport);
        setsockopt(g_host_transport, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        {
            welnpt_host_frame hello;
            welnpt_initialize_host_frame(&hello, WELNPT_HOST_FRAME_HELLO);
            if (g_real_sendto(g_host_transport, (const char *)&hello, sizeof(hello), 0,
                (const struct sockaddr *)&g_host_address, sizeof(g_host_address)) == SOCKET_ERROR) return 0;
        }
        g_light_mode = 1;
        patch_module_imports(GetModuleHandleW(NULL));
        worker = CreateThread(NULL, 0, light_receive_thread, NULL, 0, NULL);
        if (worker == NULL) return 0;
        CloseHandle(worker);
        worker = CreateThread(NULL, 0, module_watch_thread, NULL, 0, NULL);
        if (worker != NULL) CloseHandle(worker);
        log_line("\"api\":\"hook-ready\",\"mode\":\"loopback-host\",\"protocol\":3,\"hostPort\":%u",
            (unsigned)g_host_port);
        log_line("\"api\":\"session-state\",\"state\":\"WAIT_JOIN\",\"generation\":0,\"reason\":\"hook-ready\"");
        return 1;
    }

    g_transport = g_real_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_transport == INVALID_SOCKET) return 0;
    ZeroMemory(&local_address, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    local_address.sin_port = 0;
    if (g_real_bind(g_transport, (const struct sockaddr *)&local_address, sizeof(local_address)) == SOCKET_ERROR) return 0;
    tune_transport_socket(g_transport);
    setsockopt(g_transport, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	if (g_direct_agent_address.sin_port != 0 && g_direct_hook_port != 0) {
		g_direct_transport = g_real_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (g_direct_transport != INVALID_SOCKET) {
			ZeroMemory(&direct_address, sizeof(direct_address));
			direct_address.sin_family = AF_INET;
			direct_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
			direct_address.sin_port = htons(g_direct_hook_port);
			if (g_real_bind(g_direct_transport, (const struct sockaddr *)&direct_address, sizeof(direct_address)) == SOCKET_ERROR) {
				g_real_closesocket(g_direct_transport);
				g_direct_transport = INVALID_SOCKET;
			} else {
				tune_transport_socket(g_direct_transport);
				setsockopt(g_direct_transport, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
				g_real_sendto(g_direct_transport, "WELICESTATE?", 12, 0,
					(const struct sockaddr *)&g_direct_agent_address, sizeof(g_direct_agent_address));
			}
		}
	}
    send_register_packet();
    patch_module_imports(GetModuleHandleW(NULL));
    worker = CreateThread(NULL, 0, relay_receive_thread, NULL, 0, NULL);
    if (worker == NULL) return 0;
    CloseHandle(worker);
	if (g_direct_transport != INVALID_SOCKET) {
		worker = CreateThread(NULL, 0, direct_receive_thread, NULL, 0, NULL);
		if (worker != NULL) CloseHandle(worker);
		log_line("\"api\":\"direct-ready\",\"hookPort\":%u,\"agentPort\":%u",
			(unsigned)g_direct_hook_port, (unsigned)ntohs(g_direct_agent_address.sin_port));
	}
    worker = CreateThread(NULL, 0, module_watch_thread, NULL, 0, NULL);
    if (worker != NULL) CloseHandle(worker);
    log_line("\"api\":\"hook-ready\",\"mode\":\"virtual-socket\",\"protocol\":3");
    log_line("\"api\":\"session-state\",\"state\":\"WAIT_JOIN\",\"generation\":0,\"reason\":\"hook-ready\"");
    return 1;
}

static DWORD WINAPI bootstrap_thread(LPVOID unused) {
    (void)unused;
    if (initialize_hook()) signal_ready();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE worker;
        g_hook_module = instance;
        DisableThreadLibraryCalls(instance);
        worker = CreateThread(NULL, 0, bootstrap_thread, NULL, 0, NULL);
        if (worker != NULL) CloseHandle(worker);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stopping, 1);
    }
    return TRUE;
}
