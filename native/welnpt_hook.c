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
#include "welnpt_auth_windows.h"

#define WELNPT_MAX_SOCKETS 64
#define WELNPT_MAX_QUEUED_DATAGRAMS 4096
#define WELNPT_HEARTBEAT_MS 2000
#define WELNPT_GAME_JOIN_PAYLOAD_LENGTH 64
#define WELNPT_GAME_ACCEPT_PAYLOAD_LENGTH 84
#define WELNPT_ICE_STATE_PREFIX "WELICESTATE:"
#define WELNPT_ICE_AGENT_PREFIX "WELICEAGENT:"
#define WELNPT_ICE_PEER_PREFIX "WELICEPEER:"
#define WELNPT_GAME_PEER_PREFIX "WELGAMEPEER:"

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
    char payload[1];
} virtual_datagram;

typedef struct virtual_socket {
    int active;
    SOCKET handle;
    unsigned short logical_port;
    virtual_datagram *head;
    virtual_datagram *tail;
    unsigned queued;
} virtual_socket;

static HMODULE g_hook_module;
static volatile LONG g_stopping;
static volatile LONG g_next_port;
static volatile LONG g_sequence;
static CRITICAL_SECTION g_state_lock;
static CRITICAL_SECTION g_log_lock;
static int g_locks_initialized;
static virtual_socket g_sockets[WELNPT_MAX_SOCKETS];
static SOCKET g_transport = INVALID_SOCKET;
static struct sockaddr_in g_relay_address;
static SOCKET g_direct_transport = INVALID_SOCKET;
static struct sockaddr_in g_direct_agent_address;
static uint32_t g_direct_peer_ip;
static uint32_t g_direct_transaction_peer_ip;
static unsigned short g_direct_transaction_join_port;
static unsigned short g_direct_hook_port;
static volatile LONG g_direct_connected;
static uint32_t g_logical_ip;
static char g_room[WELNPT_ROOM_LENGTH];
static welnpt_auth_context g_auth;
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

static void report_game_peer(uint32_t target_ip, unsigned short join_port,
    unsigned short observed_source_port, unsigned short observed_target_port);

static void log_line(const char *format, ...) {
    char message[768];
    char line[896];
    va_list arguments;
    HANDLE file;
    DWORD written;
    int length;

    if (g_log_path[0] == L'\0' || !g_locks_initialized) return;
    va_start(arguments, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
    va_end(arguments);
    length = _snprintf_s(line, sizeof(line), _TRUNCATE, "{\"tick\":%lu,%s}\r\n",
        (unsigned long)GetTickCount(), message);
    if (length <= 0) return;
    EnterCriticalSection(&g_log_lock);
    file = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
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

static int port_in_use_locked(unsigned short port, SOCKET except_handle) {
    size_t index;
    if (port == 0) return 0;
    for (index = 0; index < ARRAYSIZE(g_sockets); ++index) {
        if (g_sockets[index].active && g_sockets[index].handle != except_handle &&
            g_sockets[index].logical_port == port) return 1;
    }
    return 0;
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
    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state != NULL) {
        if (state->logical_port == 0) state->logical_port = allocate_port_locked(handle);
        *port = state->logical_port;
        found = state->logical_port != 0;
    }
    LeaveCriticalSection(&g_state_lock);
    return found;
}

static void free_queue(virtual_socket *state) {
    virtual_datagram *item = state->head;
    while (item != NULL) {
        virtual_datagram *next = item->next;
        HeapFree(GetProcessHeap(), 0, item);
        item = next;
    }
    state->head = NULL;
    state->tail = NULL;
    state->queued = 0;
}

static int remove_socket(SOCKET handle) {
    virtual_socket *state;
    int found = 0;
    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state != NULL) {
        free_queue(state);
        ZeroMemory(state, sizeof(*state));
        found = 1;
    }
    LeaveCriticalSection(&g_state_lock);
    return found;
}

static int enqueue_datagram(const welnpt_packet_header *header, const char *payload, int length) {
    virtual_datagram *item;
    virtual_socket *state = NULL;
    size_t allocation_size;
    size_t index;
    unsigned short target_port = ntohs(header->target_port);

    allocation_size = sizeof(virtual_datagram) + (size_t)(length > 0 ? length - 1 : 0);
    item = (virtual_datagram *)HeapAlloc(GetProcessHeap(), 0, allocation_size);
    if (item == NULL) return 0;
    ZeroMemory(item, sizeof(*item));
    item->source.sin_family = AF_INET;
    item->source.sin_addr.S_un.S_addr = header->source_ip;
    item->source.sin_port = header->source_port;
    item->length = length;
    if (length > 0) CopyMemory(item->payload, payload, (size_t)length);

    EnterCriticalSection(&g_state_lock);
    for (index = 0; index < ARRAYSIZE(g_sockets); ++index) {
        if (g_sockets[index].active && g_sockets[index].logical_port == target_port) {
            state = &g_sockets[index];
            break;
        }
    }
    if (state == NULL || state->queued >= WELNPT_MAX_QUEUED_DATAGRAMS) {
        LeaveCriticalSection(&g_state_lock);
        HeapFree(GetProcessHeap(), 0, item);
        return 0;
    }
    if (state->tail == NULL) state->head = item;
    else state->tail->next = item;
    state->tail = item;
    ++state->queued;
    LeaveCriticalSection(&g_state_lock);
    return 1;
}

static int handle_transport_packet(char *packet, int received, const char *path) {
	welnpt_packet_header *header;
	int payload_length;
	if (received < (int)sizeof(welnpt_packet_header)) return 0;
	header = (welnpt_packet_header *)packet;
	payload_length = (int)ntohs(header->payload_length);
	if (!welnpt_valid_header(header) || header->type != WELNPT_PACKET_DATA ||
		memcmp(header->room, g_room, WELNPT_ROOM_LENGTH) != 0 ||
		(((header->flags & WELNPT_FLAG_BROADCAST) == 0) && header->target_ip != g_logical_ip) ||
		payload_length > WELNPT_MAX_PAYLOAD || received != (int)sizeof(*header) + payload_length ||
		!welnpt_auth_verify(&g_auth, packet, received)) return 0;
	/* The 64-byte join and the 84-byte acceptance belong to the same game
	   session. Normalize both to the initiator's join Socket port so an
	   acceptance cannot trigger a second ICE reset, while a later game using a
	   new join Socket is treated as a new session. */
	if ((header->flags & WELNPT_FLAG_BROADCAST) == 0 &&
		(payload_length == WELNPT_GAME_JOIN_PAYLOAD_LENGTH ||
		 payload_length == WELNPT_GAME_ACCEPT_PAYLOAD_LENGTH)) {
		unsigned short source_port = ntohs(header->source_port);
		unsigned short target_port = ntohs(header->target_port);
		unsigned short join_port = payload_length == WELNPT_GAME_JOIN_PAYLOAD_LENGTH ? source_port : target_port;
		report_game_peer(header->source_ip, join_port, source_port, target_port);
	}
	if (!enqueue_datagram(header, packet + sizeof(*header), payload_length)) {
		log_line("\"api\":\"transport-drop\",\"path\":\"%s\",\"targetPort\":%u,\"length\":%d",
			path, (unsigned)ntohs(header->target_port), payload_length);
		return 0;
	}
	log_line("\"api\":\"transport-recv\",\"path\":\"%s\",\"broadcast\":%s,\"sourcePort\":%u,\"length\":%d",
		path, (header->flags & WELNPT_FLAG_BROADCAST) != 0 ? "true" : "false",
		(unsigned)ntohs(header->source_port), payload_length);
	return 1;
}

static int send_register_packet(void) {
    welnpt_packet_header header;
    welnpt_initialize_header(&header, WELNPT_PACKET_REGISTER);
    CopyMemory(header.room, g_room, WELNPT_ROOM_LENGTH);
    header.source_ip = g_logical_ip;
    if (!welnpt_auth_sign(&g_auth, (char *)&header, sizeof(header))) return SOCKET_ERROR;
    return g_real_sendto(g_transport, (const char *)&header, sizeof(header), 0,
        (const struct sockaddr *)&g_relay_address, sizeof(g_relay_address));
}

static void report_game_peer(uint32_t target_ip, unsigned short join_port,
    unsigned short observed_source_port, unsigned short observed_target_port) {
    char target[INET_ADDRSTRLEN];
    char message[96];
    int length;
    int is_new_transaction;
    if (g_direct_transport == INVALID_SOCKET || g_direct_agent_address.sin_port == 0 || target_ip == 0) return;
    if (InetNtopA(AF_INET, &target_ip, target, sizeof(target)) == NULL) return;
    EnterCriticalSection(&g_state_lock);
	is_new_transaction = g_direct_transaction_peer_ip != target_ip ||
		g_direct_transaction_join_port != join_port;
	if (is_new_transaction) {
		g_direct_transaction_peer_ip = target_ip;
		g_direct_transaction_join_port = join_port;
    }
    LeaveCriticalSection(&g_state_lock);
    if (is_new_transaction) {
        /* A new join starts a new game transaction. Keep its handshake on the
           relay until this transaction's ICE agent confirms a fresh connection. */
        InterlockedExchange(&g_direct_connected, 0);
    }
	length = _snprintf_s(message, sizeof(message), _TRUNCATE, "%s%s|%u|%u", WELNPT_GAME_PEER_PREFIX,
		target, (unsigned)join_port, (unsigned)join_port);
    if (length <= 0) return;
    g_real_sendto(g_direct_transport, message, length, 0,
        (const struct sockaddr *)&g_direct_agent_address, sizeof(g_direct_agent_address));
    if (is_new_transaction) {
		log_line("\"api\":\"direct-target\",\"target\":\"%s\",\"joinPort\":%u,\"observedSourcePort\":%u,\"observedTargetPort\":%u",
			target, (unsigned)join_port, (unsigned)observed_source_port, (unsigned)observed_target_port);
    }
}

static int send_virtual_datagram(SOCKET handle, const char *payload, int length,
    const struct sockaddr *destination, int destination_length) {
    char packet[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
    welnpt_packet_header *header = (welnpt_packet_header *)packet;
    const struct sockaddr_in *target;
    unsigned short source_port;
    int sent;

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
    if (!welnpt_auth_sign(&g_auth, packet, (int)sizeof(*header) + length)) {
        WSASetLastError(WSAEACCES);
        return SOCKET_ERROR;
    }
	/* The 64-byte join and 84-byte acceptance identify the same game session.
	   The initiator's join port is the source for 64 bytes and the target for
	   84 bytes. Do not require destination port 5739: the host can be dynamic. */
	if ((header->flags & WELNPT_FLAG_BROADCAST) == 0 &&
		(length == WELNPT_GAME_JOIN_PAYLOAD_LENGTH || length == WELNPT_GAME_ACCEPT_PAYLOAD_LENGTH)) {
		unsigned short target_port = ntohs(target->sin_port);
		unsigned short join_port = length == WELNPT_GAME_JOIN_PAYLOAD_LENGTH ? source_port : target_port;
		report_game_peer(header->target_ip, join_port, source_port, target_port);
	}
    if ((header->flags & WELNPT_FLAG_BROADCAST) == 0 && g_direct_transport != INVALID_SOCKET &&
        header->target_ip == g_direct_peer_ip && InterlockedCompareExchange(&g_direct_connected, 0, 0) != 0) {
        sent = g_real_sendto(g_direct_transport, packet, (int)sizeof(*header) + length, 0,
            (const struct sockaddr *)&g_direct_agent_address, sizeof(g_direct_agent_address));
        if (sent != SOCKET_ERROR) {
            log_line("\"api\":\"sendto\",\"path\":\"direct\",\"socket\":%llu,\"sourcePort\":%u,\"targetPort\":%u,\"length\":%d",
                (unsigned __int64)handle, (unsigned)source_port, (unsigned)ntohs(target->sin_port), length);
            WSASetLastError(0);
            return length;
        }
        InterlockedExchange(&g_direct_connected, 0);
        log_line("\"api\":\"direct-fallback\",\"reason\":\"local-send-failed\"");
    }
    sent = g_real_sendto(g_transport, packet, (int)sizeof(*header) + length, 0,
        (const struct sockaddr *)&g_relay_address, sizeof(g_relay_address));
    if (sent == SOCKET_ERROR) return SOCKET_ERROR;
    log_line("\"api\":\"sendto\",\"path\":\"relay\",\"broadcast\":%s,\"socket\":%llu,\"sourcePort\":%u,\"targetPort\":%u,\"length\":%d",
        (header->flags & WELNPT_FLAG_BROADCAST) != 0 ? "true" : "false",
        (unsigned __int64)handle, (unsigned)source_port, (unsigned)ntohs(target->sin_port), length);
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

    EnterCriticalSection(&g_state_lock);
    state = find_socket_locked(handle);
    if (state == NULL) {
        LeaveCriticalSection(&g_state_lock);
        return -2;
    }
    item = state->head;
    if (item == NULL) {
        LeaveCriticalSection(&g_state_lock);
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
    if (result > 0 && buffer != NULL) CopyMemory(buffer, item->payload, (size_t)result);
    if (source != NULL && source_length != NULL) {
        if (*source_length < (int)sizeof(struct sockaddr_in)) {
            if (!peek) HeapFree(GetProcessHeap(), 0, item);
            LeaveCriticalSection(&g_state_lock);
            WSASetLastError(WSAEFAULT);
            return SOCKET_ERROR;
        }
        CopyMemory(source, &item->source, sizeof(item->source));
        *source_length = sizeof(item->source);
    }
    log_line("\"api\":\"recvfrom\",\"socket\":%llu,\"sourcePort\":%u,\"length\":%d",
        (unsigned __int64)handle, (unsigned)ntohs(item->source.sin_port), item_length);
    if (!peek) HeapFree(GetProcessHeap(), 0, item);
    LeaveCriticalSection(&g_state_lock);
    if (length < item_length) {
        WSASetLastError(WSAEMSGSIZE);
        return SOCKET_ERROR;
    }
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
    state->logical_port = requested_port != 0 ? requested_port : allocate_port_locked(handle);
    requested_port = state->logical_port;
    LeaveCriticalSection(&g_state_lock);
    if (requested_port == 0) {
        WSASetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    log_line("\"api\":\"bind\",\"socket\":%llu,\"logicalPort\":%u",
        (unsigned __int64)handle, (unsigned)requested_port);
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
    remove_socket(handle);
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
        Sleep(250);
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
        if (received <= 0) continue;
        if (received > (int)strlen(WELNPT_ICE_AGENT_PREFIX) &&
            memcmp(packet, WELNPT_ICE_AGENT_PREFIX, strlen(WELNPT_ICE_AGENT_PREFIX)) == 0) {
            unsigned long port = strtoul(packet + strlen(WELNPT_ICE_AGENT_PREFIX), NULL, 10);
            if (port > 0 && port <= 65535) {
                g_direct_agent_address.sin_port = htons((u_short)port);
                InterlockedExchange(&g_direct_connected, 0);
                log_line("\"api\":\"direct-agent\",\"port\":%lu", port);
            }
            continue;
        }
        if (received > (int)strlen(WELNPT_ICE_STATE_PREFIX) &&
            memcmp(packet, WELNPT_ICE_STATE_PREFIX, strlen(WELNPT_ICE_STATE_PREFIX)) == 0) {
            const char *state = packet + strlen(WELNPT_ICE_STATE_PREFIX);
            int connected = strncmp(state, "connected", 9) == 0 || strncmp(state, "completed", 9) == 0;
            InterlockedExchange(&g_direct_connected, connected);
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
    char token[WELNPT_AUTH_SECRET_MAX];
    char direct_peer_ip[64];
    char direct_agent_port[16];
    char direct_hook_port[16];
    char *separator;
    char port[16];
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    DWORD room_length;

    if (GetEnvironmentVariableA("WEL_NOTAP_RELAY", relay, sizeof(relay)) == 0 ||
        GetEnvironmentVariableA("WEL_NOTAP_LOGICAL_IP", logical_ip, sizeof(logical_ip)) == 0 ||
        GetEnvironmentVariableA("WEL_NOTAP_TOKEN", token, sizeof(token)) == 0) return 0;
    room_length = GetEnvironmentVariableA("WEL_NOTAP_ROOM", g_room, sizeof(g_room));
    if (room_length == 0 || room_length >= sizeof(g_room)) return 0;
    separator = strrchr(relay, ':');
    if (separator == NULL || separator == relay || separator[1] == '\0') return 0;
    strcpy_s(port, sizeof(port), separator + 1);
    *separator = '\0';
    if (InetPtonA(AF_INET, logical_ip, &g_logical_ip) != 1) return 0;
    if (!welnpt_auth_initialize(&g_auth, token)) return 0;
    SecureZeroMemory(token, sizeof(token));
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
    InitializeCriticalSection(&g_log_lock);
    g_locks_initialized = 1;
    GetEnvironmentVariableW(L"WEL_NOTAP_LOG_PATH", g_log_path, ARRAYSIZE(g_log_path));
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

    g_transport = g_real_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_transport == INVALID_SOCKET) return 0;
    ZeroMemory(&local_address, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    local_address.sin_port = 0;
    if (g_real_bind(g_transport, (const struct sockaddr *)&local_address, sizeof(local_address)) == SOCKET_ERROR) return 0;
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
    log_line("\"api\":\"hook-ready\",\"mode\":\"virtual-socket\",\"protocol\":2");
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
