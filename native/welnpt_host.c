#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "welnpt_protocol.h"

#define WEL_HOST_BUFFER_SIZE (sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD)
#define WEL_HOST_DECISION_WINDOW_MS 5000
#define WEL_HOST_SESSION_TIMEOUT_MS 12000
#define WEL_HOST_HEARTBEAT_MS 2000
#define WEL_HOST_PATH_PENDING 0
#define WEL_HOST_PATH_DIRECT 1
#define WEL_HOST_PATH_RELAY 2

static SOCKET g_socket = INVALID_SOCKET;
static struct sockaddr_in g_relay_address;
static struct sockaddr_in g_agent_address;
static struct sockaddr_in g_hook_address;
static int g_hook_known;
static unsigned short g_agent_port;
static uint32_t g_logical_ip;
static char g_room[WELNPT_ROOM_LENGTH];
static volatile LONG g_stopping;
static volatile LONG g_sequence;
static LONG g_path = WEL_HOST_PATH_PENDING;
static LONG g_direct_connected;
static uint32_t g_peer_ip;
static unsigned short g_join_port;
static LONG g_generation;
static ULONGLONG g_decision_started;
static ULONGLONG g_decision_deadline;
static ULONGLONG g_session_deadline;

typedef struct wel_host_socket_map {
    int active;
    unsigned short logical_port;
    unsigned short local_port;
} wel_host_socket_map;

static wel_host_socket_map g_socket_maps[64];

typedef enum wel_host_payload_kind {
    WEL_HOST_PAYLOAD_DATA = 0,
    WEL_HOST_PAYLOAD_JOIN,
    WEL_HOST_PAYLOAD_ACCEPT,
    WEL_HOST_PAYLOAD_SEARCH,
} wel_host_payload_kind;

static int parse_port(const char *value, unsigned short *port) {
    char *end = NULL;
    unsigned long parsed;
    if (value == NULL || port == NULL) return 0;
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > 65535) return 0;
    *port = (unsigned short)parsed;
    return 1;
}

static int env_port(const char *name, unsigned short *port) {
    char value[16];
    DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length > 0 && length < sizeof(value) && parse_port(value, port);
}

static int env_text(const char *name, char *value, DWORD capacity) {
    DWORD length = GetEnvironmentVariableA(name, value, capacity);
    return length > 0 && length < capacity;
}

static int same_room(const char left[WELNPT_ROOM_LENGTH], const char right[WELNPT_ROOM_LENGTH]) {
    return memcmp(left, right, WELNPT_ROOM_LENGTH) == 0;
}

static int is_loopback(const struct sockaddr_in *address) {
    return address != NULL && address->sin_addr.S_un.S_addr == htonl(INADDR_LOOPBACK);
}

static void signal_ready(void) {
    char name[128];
    HANDLE event;
    DWORD length = GetEnvironmentVariableA("WEL_NOTAP_HOST_READY_EVENT", name, sizeof(name));
    if (length == 0 || length >= sizeof(name)) return;
    event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (event != NULL) {
        SetEvent(event);
        CloseHandle(event);
    }
}

static wel_host_payload_kind classify_payload(const char *payload, int length) {
    static const unsigned char join_prefix[] = { 0xe7, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char accept_prefix[] = { 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00 };
    if (payload == NULL || length < 0) return WEL_HOST_PAYLOAD_DATA;
    if (length == 64 && length >= (int)sizeof(join_prefix) && memcmp(payload, join_prefix, sizeof(join_prefix)) == 0) {
        return WEL_HOST_PAYLOAD_JOIN;
    }
    if (length == 84 && length >= (int)sizeof(accept_prefix) && memcmp(payload, accept_prefix, sizeof(accept_prefix)) == 0) {
        return WEL_HOST_PAYLOAD_ACCEPT;
    }
    if (length == 24 && length >= (int)sizeof(join_prefix) && memcmp(payload, join_prefix, sizeof(join_prefix)) == 0) {
        return WEL_HOST_PAYLOAD_SEARCH;
    }
    return WEL_HOST_PAYLOAD_DATA;
}

/* Only join/accept/search packets affect the session state. Ordinary game
 * datagrams stay on the fast path and do not need a payload prefix scan. */
static wel_host_payload_kind classify_control_payload(const char *payload, int length) {
    if (length != 24 && length != 64 && length != 84) return WEL_HOST_PAYLOAD_DATA;
    return classify_payload(payload, length);
}

static void send_to_hook(const void *data, int length) {
    if (!g_hook_known || data == NULL || length <= 0) return;
    sendto(g_socket, (const char *)data, length, 0,
        (const struct sockaddr *)&g_hook_address, sizeof(g_hook_address));
}

static void send_hook_text(const char *text) {
    if (text != NULL) send_to_hook(text, (int)strlen(text));
}

static void send_to_agent(const char *data, int length) {
    if (g_agent_port == 0 || data == NULL || length <= 0) return;
    g_agent_address.sin_port = htons(g_agent_port);
    sendto(g_socket, data, length, 0, (const struct sockaddr *)&g_agent_address, sizeof(g_agent_address));
}

static void notify_transport(const char *path) {
    char message[64];
    int length;
    if (path == NULL) return;
    length = _snprintf_s(message, sizeof(message), _TRUNCATE, "WELTRANSPORT:%s", path);
    if (length > 0) send_to_agent(message, length);
    length = _snprintf_s(message, sizeof(message), _TRUNCATE, "WELTRANSPORT:%s", path);
    if (length > 0) send_hook_text(message);
}

static void lock_relay(const char *reason) {
    (void)reason;
    if (InterlockedCompareExchange(&g_path, WEL_HOST_PATH_RELAY, WEL_HOST_PATH_PENDING) == WEL_HOST_PATH_PENDING) {
        g_decision_deadline = 0;
        g_session_deadline = 0;
        InterlockedExchange(&g_direct_connected, 0);
        notify_transport("relay");
    }
}

static void reset_session(void) {
    g_peer_ip = 0;
    g_join_port = 0;
    g_decision_started = 0;
    g_decision_deadline = 0;
    g_session_deadline = 0;
    InterlockedExchange(&g_direct_connected, 0);
    InterlockedExchange(&g_path, WEL_HOST_PATH_PENDING);
    notify_transport("pending");
}

static void check_deadline(void) {
    ULONGLONG now = GetTickCount64();
    if (InterlockedCompareExchange(&g_path, 0, 0) != WEL_HOST_PATH_PENDING) return;
    if (g_decision_deadline != 0 && now >= g_decision_deadline) lock_relay("decision-timeout");
    else if (g_session_deadline != 0 && now >= g_session_deadline) lock_relay("remote-sdp-timeout");
}

static void report_game_peer(uint32_t peer_ip, unsigned short join_port,
    unsigned short source_port, unsigned short target_port) {
    char text[128];
    char ip[INET_ADDRSTRLEN];
    int length;
    int changed = g_peer_ip != peer_ip || g_join_port != join_port;
    if (!changed || peer_ip == 0 || InetNtopA(AF_INET, &peer_ip, ip, sizeof(ip)) == NULL) return;
    g_peer_ip = peer_ip;
    g_join_port = join_port;
    InterlockedIncrement(&g_generation);
    g_decision_started = GetTickCount64();
    g_decision_deadline = 0;
    g_session_deadline = g_decision_started + WEL_HOST_SESSION_TIMEOUT_MS;
    InterlockedExchange(&g_direct_connected, 0);
    InterlockedExchange(&g_path, WEL_HOST_PATH_PENDING);
    length = _snprintf_s(text, sizeof(text), _TRUNCATE, "WELGAMEPEER:%s|%u|%u|%lu",
        ip, (unsigned)source_port, (unsigned)target_port, (unsigned long)g_generation);
    if (length > 0) send_to_agent(text, length);
    notify_transport("pending");
}

static int send_wire_packet(const char *payload, int length, uint32_t target_ip,
    unsigned short source_port, unsigned short target_port, uint8_t flags) {
    char packet[WEL_HOST_BUFFER_SIZE];
    welnpt_packet_header *header = (welnpt_packet_header *)packet;
    int sent;
    LONG path;
    if (length < 0 || length > WELNPT_MAX_PAYLOAD) return SOCKET_ERROR;
    welnpt_initialize_header(header, WELNPT_PACKET_DATA);
    header->flags = flags;
    CopyMemory(header->room, g_room, WELNPT_ROOM_LENGTH);
    header->source_ip = g_logical_ip;
    header->source_port = htons(source_port);
    header->target_ip = target_ip;
    header->target_port = htons(target_port);
    header->payload_length = htons((u_short)length);
    header->sequence = htonl((u_long)InterlockedIncrement(&g_sequence));
    if (length > 0) CopyMemory(packet + sizeof(*header), payload, (size_t)length);
    path = InterlockedCompareExchange(&g_path, 0, 0);
    if ((flags & WELNPT_FLAG_BROADCAST) == 0 && path == WEL_HOST_PATH_DIRECT &&
        InterlockedCompareExchange(&g_direct_connected, 0, 0) != 0 && target_ip == g_peer_ip && g_agent_port != 0) {
        g_agent_address.sin_port = htons(g_agent_port);
        sent = sendto(g_socket, packet, (int)sizeof(*header) + length, 0,
            (const struct sockaddr *)&g_agent_address, sizeof(g_agent_address));
        if (sent != SOCKET_ERROR) return length;
        lock_relay("direct-send-failed");
    }
    sent = sendto(g_socket, packet, (int)sizeof(*header) + length, 0,
        (const struct sockaddr *)&g_relay_address, sizeof(g_relay_address));
    return sent == SOCKET_ERROR ? SOCKET_ERROR : length;
}

static void send_local_frame(const welnpt_packet_header *header, const char *payload, int length) {
    char packet[sizeof(welnpt_host_frame) + WELNPT_MAX_PAYLOAD];
    welnpt_host_frame *frame = (welnpt_host_frame *)packet;
    struct sockaddr_in local;
    size_t index;
    unsigned short target_port;
    if (!g_hook_known || header == NULL || length < 0 || length > WELNPT_MAX_PAYLOAD) return;
    welnpt_initialize_host_frame(frame, WELNPT_HOST_FRAME_DATA);
    frame->flags = header->flags;
    frame->source_ip = header->source_ip;
    frame->source_port = header->source_port;
    frame->target_ip = header->target_ip;
    frame->target_port = header->target_port;
    frame->payload_length = htons((u_short)length);
    frame->sequence = header->sequence;
    if (length > 0) CopyMemory(packet + sizeof(*frame), payload, (size_t)length);
    target_port = ntohs(header->target_port);
    for (index = 0; index < ARRAYSIZE(g_socket_maps); ++index) {
        if (g_socket_maps[index].active && g_socket_maps[index].logical_port == target_port) {
            ZeroMemory(&local, sizeof(local));
            local.sin_family = AF_INET;
            local.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
            local.sin_port = htons(g_socket_maps[index].local_port);
            sendto(g_socket, packet, (int)sizeof(*frame) + length, 0,
                (const struct sockaddr *)&local, sizeof(local));
            return;
        }
    }
    /* The Hook also listens on its control endpoint and can route this frame
       after a MAP datagram races with the first inbound game packet. */
    send_to_hook(packet, (int)sizeof(*frame) + length);
}

static void process_game_frame(const welnpt_host_frame *frame, const char *payload) {
    wel_host_payload_kind kind;
    int session_signal;
    unsigned short source_port;
    unsigned short target_port;
    unsigned short join_port;
    if (frame == NULL) return;
    kind = classify_control_payload(payload, (int)ntohs(frame->payload_length));
    if ((frame->flags & WELNPT_FLAG_BROADCAST) != 0 && kind == WEL_HOST_PAYLOAD_SEARCH) reset_session();
    session_signal = (frame->flags & WELNPT_FLAG_BROADCAST) == 0 &&
        (kind == WEL_HOST_PAYLOAD_JOIN || kind == WEL_HOST_PAYLOAD_ACCEPT);
    source_port = ntohs(frame->source_port);
    target_port = ntohs(frame->target_port);
    join_port = kind == WEL_HOST_PAYLOAD_JOIN ? source_port : target_port;
    if (session_signal) report_game_peer(frame->target_ip, join_port, source_port, target_port);
    if (send_wire_packet(payload, (int)ntohs(frame->payload_length), frame->target_ip,
        source_port, target_port, frame->flags) == SOCKET_ERROR) return;
}

static int process_wire_packet(char *packet, int received, int direct) {
    welnpt_packet_header *header;
    int payload_length;
    wel_host_payload_kind kind;
    unsigned short source_port;
    unsigned short target_port;
    unsigned short join_port;
    LONG path;
    if (received < (int)sizeof(welnpt_packet_header)) return 0;
    header = (welnpt_packet_header *)packet;
    payload_length = (int)ntohs(header->payload_length);
    if (!welnpt_valid_header(header) || header->type != WELNPT_PACKET_DATA ||
        !same_room(header->room, g_room) ||
        (((header->flags & WELNPT_FLAG_BROADCAST) == 0) && header->target_ip != g_logical_ip) ||
        payload_length < 0 || payload_length > WELNPT_MAX_PAYLOAD ||
        received != (int)sizeof(*header) + payload_length) return 0;
    kind = classify_control_payload(packet + sizeof(*header), payload_length);
    if ((header->flags & WELNPT_FLAG_BROADCAST) != 0 &&
        kind == WEL_HOST_PAYLOAD_SEARCH && header->source_ip == g_peer_ip) {
        reset_session();
    }
    source_port = ntohs(header->source_port);
    target_port = ntohs(header->target_port);
    join_port = kind == WEL_HOST_PAYLOAD_JOIN ? source_port : target_port;
    if ((header->flags & WELNPT_FLAG_BROADCAST) == 0 &&
        (kind == WEL_HOST_PAYLOAD_JOIN || kind == WEL_HOST_PAYLOAD_ACCEPT)) {
        report_game_peer(header->source_ip, join_port, source_port, target_port);
    }
    path = InterlockedCompareExchange(&g_path, 0, 0);
    if ((header->flags & WELNPT_FLAG_BROADCAST) == 0 && header->source_ip == g_peer_ip) {
        if (direct && path != WEL_HOST_PATH_DIRECT) return 1;
        if (!direct && path == WEL_HOST_PATH_DIRECT) return 1;
    }
    send_local_frame(header, packet + sizeof(*header), payload_length);
    return 1;
}

static void process_ice_message(char *packet, int received) {
    const char *state;
    if (received > (int)strlen("WELICEAGENT:") && memcmp(packet, "WELICEAGENT:", strlen("WELICEAGENT:")) == 0) {
        unsigned short port;
        if (parse_port(packet + strlen("WELICEAGENT:"), &port)) g_agent_port = port;
        send_to_hook(packet, received);
        return;
    }
    if (received == (int)strlen("WELICEREMOTESET") && memcmp(packet, "WELICEREMOTESET", received) == 0) {
        if (InterlockedCompareExchange(&g_path, 0, 0) == WEL_HOST_PATH_PENDING) {
            g_decision_started = GetTickCount64();
            g_decision_deadline = g_decision_started + WEL_HOST_DECISION_WINDOW_MS;
            g_session_deadline = 0;
        }
        send_to_hook(packet, received);
        return;
    }
    if (received > (int)strlen("WELICESTATE:") && memcmp(packet, "WELICESTATE:", strlen("WELICESTATE:")) == 0) {
        state = packet + strlen("WELICESTATE:");
        if ((strncmp(state, "connected", 9) == 0 || strncmp(state, "completed", 9) == 0) &&
            InterlockedCompareExchange(&g_path, 0, 0) == WEL_HOST_PATH_PENDING) {
            InterlockedExchange(&g_path, WEL_HOST_PATH_DIRECT);
            InterlockedExchange(&g_direct_connected, 1);
            g_decision_deadline = 0;
            g_session_deadline = 0;
            notify_transport("direct");
        } else if (strncmp(state, "failed", 6) == 0 &&
            InterlockedCompareExchange(&g_path, 0, 0) == WEL_HOST_PATH_PENDING) {
            lock_relay("ice-failed");
        } else if ((strncmp(state, "failed", 6) == 0 || strncmp(state, "disconnected", 12) == 0) &&
            InterlockedCompareExchange(&g_path, 0, 0) == WEL_HOST_PATH_DIRECT) {
            InterlockedExchange(&g_path, WEL_HOST_PATH_RELAY);
            InterlockedExchange(&g_direct_connected, 0);
            notify_transport("relay");
        }
        send_to_hook(packet, received);
        return;
    }
    if (received > (int)strlen("WELICEPEER:") && memcmp(packet, "WELICEPEER:", strlen("WELICEPEER:")) == 0) {
        send_to_hook(packet, received);
    }
}

static int initialize_configuration(unsigned short host_port) {
    char relay[256];
    char logical_ip[64];
    char service[16];
    char *separator;
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct sockaddr_in local;
    unsigned short direct_agent_port = 0;
    WSADATA winsock;
    if (!env_text("WEL_NOTAP_RELAY", relay, sizeof(relay)) ||
        !env_text("WEL_NOTAP_LOGICAL_IP", logical_ip, sizeof(logical_ip)) ||
        !env_text("WEL_NOTAP_ROOM", g_room, sizeof(g_room))) return 0;
    separator = strrchr(relay, ':');
    if (separator == NULL || separator == relay || separator[1] == '\0') return 0;
    strcpy_s(service, sizeof(service), separator + 1);
    *separator = '\0';
    if (InetPtonA(AF_INET, logical_ip, &g_logical_ip) != 1) return 0;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 0;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(relay, service, &hints, &addresses) != 0 || addresses == NULL) return 0;
    CopyMemory(&g_relay_address, addresses->ai_addr, sizeof(g_relay_address));
    freeaddrinfo(addresses);
    env_port("WEL_NOTAP_DIRECT_AGENT_PORT", &direct_agent_port);
    g_agent_port = direct_agent_port;
    g_agent_address.sin_family = AF_INET;
    g_agent_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    g_agent_address.sin_port = htons(g_agent_port);
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) return 0;
    ZeroMemory(&local, sizeof(local));
    local.sin_family = AF_INET;
    /* One transport socket serves both the local Hook/ICE control path and
       the room relay. Bind all interfaces so relay datagrams do not
       leave with a 127.0.0.1 source address. Local callers still target the
       explicit loopback address in g_hook_address/g_agent_address. */
    local.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    local.sin_port = htons(host_port);
    if (bind(g_socket, (const struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) return 0;
    {
        u_long nonblocking = 1;
        if (ioctlsocket(g_socket, FIONBIO, &nonblocking) == SOCKET_ERROR) return 0;
    }
    return 1;
}

static void send_presence(void) {
    welnpt_packet_header header;
    welnpt_initialize_header(&header, WELNPT_PACKET_REGISTER);
    CopyMemory(header.room, g_room, WELNPT_ROOM_LENGTH);
    header.source_ip = g_logical_ip;
    sendto(g_socket, (const char *)&header, sizeof(header), 0,
        (const struct sockaddr *)&g_relay_address, sizeof(g_relay_address));
}

int main(int argc, char **argv) {
    unsigned short host_port = 0;
    DWORD game_pid = 0;
    HANDLE game = NULL;
    char packet[WEL_HOST_BUFFER_SIZE];
    ULONGLONG next_presence = 0;
    int index;
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
        char packet[sizeof(welnpt_packet_header) + 4];
        welnpt_packet_header *header = (welnpt_packet_header *)packet;
        welnpt_host_frame frame;
        welnpt_initialize_header(header, WELNPT_PACKET_DATA);
        header->payload_length = htons(4);
        CopyMemory(packet + sizeof(*header), "test", 4);
        welnpt_initialize_host_frame(&frame, WELNPT_HOST_FRAME_DATA);
        if (sizeof(*header) != 58 || !welnpt_valid_header(header) || !welnpt_valid_host_frame(&frame)) return 1;
        welnpt_initialize_host_frame(&frame, WELNPT_HOST_FRAME_CLOSE);
        frame.source_port = htons(49152);
        if (!welnpt_valid_host_frame(&frame)) return 1;
        puts("SELF-TEST OK");
        return 0;
    }
    if (!env_port("WEL_NOTAP_HOST_PORT", &host_port)) return 2;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--game-pid") == 0 && index + 1 < argc) game_pid = (DWORD)strtoul(argv[++index], NULL, 10);
        else return 2;
    }
    if (!initialize_configuration(host_port)) return 3;
    if (game_pid != 0) game = OpenProcess(SYNCHRONIZE, FALSE, game_pid);
    signal_ready();
    send_presence();
    next_presence = GetTickCount64() + WEL_HOST_HEARTBEAT_MS;
    while (InterlockedCompareExchange(&g_stopping, 0, 0) == 0) {
        struct sockaddr_in source;
        int source_length = sizeof(source);
        int received;
        ULONGLONG now = GetTickCount64();
        if (game != NULL && WaitForSingleObject(game, 0) == WAIT_OBJECT_0) break;
        if (now >= next_presence) {
            send_presence();
            next_presence = now + WEL_HOST_HEARTBEAT_MS;
        }
        received = recvfrom(g_socket, packet, sizeof(packet), 0,
            (struct sockaddr *)&source, &source_length);
        if (received > 0 && received >= (int)sizeof(welnpt_host_frame) &&
            memcmp(packet, WELNPT_HOST_MAGIC, 4) == 0) {
            welnpt_host_frame *frame = (welnpt_host_frame *)packet;
            if (frame->type == WELNPT_HOST_FRAME_HELLO) {
                g_hook_address = source;
                g_hook_known = 1;
            } else if (g_hook_known && source.sin_port == g_hook_address.sin_port) {
                int payload_length = (int)ntohs(frame->payload_length);
                size_t map_index;
                if (welnpt_valid_host_frame(frame) && frame->type == WELNPT_HOST_FRAME_MAP) {
                    for (map_index = 0; map_index < ARRAYSIZE(g_socket_maps); ++map_index) {
                        if (!g_socket_maps[map_index].active ||
                            g_socket_maps[map_index].logical_port == ntohs(frame->source_port)) break;
                    }
                    if (map_index < ARRAYSIZE(g_socket_maps) && ntohs(frame->target_port) != 0) {
                        g_socket_maps[map_index].active = 1;
                        g_socket_maps[map_index].logical_port = ntohs(frame->source_port);
                        g_socket_maps[map_index].local_port = ntohs(frame->target_port);
                    }
                } else if (welnpt_valid_host_frame(frame) && frame->type == WELNPT_HOST_FRAME_CLOSE) {
                    unsigned short logical_port = ntohs(frame->source_port);
                    for (map_index = 0; map_index < ARRAYSIZE(g_socket_maps); ++map_index) {
                        if (g_socket_maps[map_index].active &&
                            g_socket_maps[map_index].logical_port == logical_port) {
                            ZeroMemory(&g_socket_maps[map_index], sizeof(g_socket_maps[map_index]));
                            break;
                        }
                    }
                    if (logical_port != 0 && logical_port == g_join_port) reset_session();
                } else if (welnpt_valid_host_frame(frame) && frame->type == WELNPT_HOST_FRAME_DATA &&
                    payload_length >= 0 && payload_length <= WELNPT_MAX_PAYLOAD &&
                    received == (int)sizeof(*frame) + payload_length) {
                    process_game_frame(frame, packet + sizeof(*frame));
                }
            }
        } else if (received > 0 && received >= (int)sizeof(welnpt_packet_header) &&
            memcmp(packet, "WNP3", 4) == 0) {
            process_wire_packet(packet, received,
                is_loopback(&source) && g_agent_port != 0 && ntohs(source.sin_port) == g_agent_port);
        } else if (received > 0 && is_loopback(&source)) {
            process_ice_message(packet, received);
        }
        check_deadline();
        if (received <= 0 && WSAGetLastError() == WSAEWOULDBLOCK) Sleep(20);
    }
    if (game != NULL) CloseHandle(game);
    if (g_socket != INVALID_SOCKET) closesocket(g_socket);
    WSACleanup();
    return 0;
}
