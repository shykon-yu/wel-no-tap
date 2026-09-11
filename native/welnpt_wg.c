#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "welnpt_protocol.h"

#define WG_BUFFER_SIZE (sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD)
#define WG_DATA_PORT 51830
#define WG_HELLO_INTERVAL_MS 250
#define WG_TIMEOUT_MS 3000
#define WG_CONNECT_TIMEOUT_MS 12000
#define WG_GAME_PEER_PREFIX "WELGAMEPEER:"

static int parse_port(const char *value, unsigned short *result) {
    char *end = NULL;
    unsigned long port = strtoul(value == NULL ? "" : value, &end, 10);
    if (end == value || *end != '\0' || port == 0 || port > 65535) return 0;
    *result = (unsigned short)port;
    return 1;
}

static int valid_packet(const char *packet, int length) {
    const welnpt_packet_header *header;
    if (length < (int)sizeof(welnpt_packet_header)) return 0;
    header = (const welnpt_packet_header *)packet;
    return welnpt_valid_header(header) && header->type == WELNPT_PACKET_DATA &&
        ntohs(header->payload_length) <= WELNPT_MAX_PAYLOAD &&
        length == (int)sizeof(*header) + (int)ntohs(header->payload_length);
}

static void emit_state(SOCKET socket_value, const struct sockaddr_in *host, const char *state) {
    char message[64];
    int length = _snprintf_s(message, sizeof(message), _TRUNCATE, "WELICESTATE:%s", state);
    if (length > 0) sendto(socket_value, message, length, 0, (const struct sockaddr *)host, sizeof(*host));
    printf("STATE %s\n", state);
    fflush(stdout);
}

static void emit_game_peer(const char *message, int length) {
    const char *payload;
    int payload_length;
    char peer_ip[INET_ADDRSTRLEN];
    unsigned short source_port = 0;
    unsigned short target_port = 0;
    unsigned long generation = 0;
    char *separator;
    char value[128];
    struct in_addr address;

    if (message == NULL || length <= (int)strlen(WG_GAME_PEER_PREFIX) ||
        length >= (int)sizeof(value) ||
        memcmp(message, WG_GAME_PEER_PREFIX, strlen(WG_GAME_PEER_PREFIX)) != 0) return;
    payload = message + strlen(WG_GAME_PEER_PREFIX);
    payload_length = length - (int)strlen(WG_GAME_PEER_PREFIX);
    CopyMemory(value, payload, (size_t)payload_length);
    value[payload_length] = '\0';

    separator = strchr(value, '|');
    if (separator == NULL) return;
    *separator = '\0';
    if (InetPtonA(AF_INET, value, &address) != 1 ||
        InetNtopA(AF_INET, &address, peer_ip, sizeof(peer_ip)) == NULL) return;
    payload = separator + 1;
    separator = strchr((char *)payload, '|');
    if (separator == NULL) return;
    *separator = '\0';
    if (!parse_port(payload, &source_port)) return;
    payload = separator + 1;
    separator = strchr((char *)payload, '|');
    if (separator == NULL) return;
    *separator = '\0';
    if (!parse_port(payload, &target_port)) return;
    payload = separator + 1;
    if (*payload == '\0') return;
    {
        char *end = NULL;
        generation = strtoul(payload, &end, 10);
        if (end == payload || *end != '\0' || generation == 0) return;
    }
    printf("GAME_PEER %s|%u|%u|%lu\n", peer_ip, (unsigned)source_port,
        (unsigned)target_port, generation);
    fflush(stdout);
}

int main(int argc, char **argv) {
    SOCKET local_socket = INVALID_SOCKET, tunnel_socket = INVALID_SOCKET;
    struct sockaddr_in local_address, host_address, tunnel_address, peer_address;
    unsigned short agent_port = 0, hook_port = 0, data_port = WG_DATA_PORT;
    uint32_t virtual_ip = 0;
    char packet[WG_BUFFER_SIZE];
    WSADATA winsock;
    ULONGLONG next_hello = 0, last_peer_packet = 0, target_started = 0;
    int connected = 0, attempt_failed = 0, index;
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
        if (sizeof(welnpt_packet_header) != 58) return 1;
        puts("SELF-TEST OK");
        return 0;
    }
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--agent-port") == 0 && index + 1 < argc) parse_port(argv[++index], &agent_port);
        else if (strcmp(argv[index], "--hook-port") == 0 && index + 1 < argc) parse_port(argv[++index], &hook_port);
        else if (strcmp(argv[index], "--data-port") == 0 && index + 1 < argc) parse_port(argv[++index], &data_port);
        else if (strcmp(argv[index], "--virtual-ip") == 0 && index + 1 < argc && InetPtonA(AF_INET, argv[++index], &virtual_ip) == 1) {}
        else return 2;
    }
    if (agent_port == 0 || hook_port == 0 || virtual_ip == 0 || WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 2;
    local_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    tunnel_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (local_socket == INVALID_SOCKET || tunnel_socket == INVALID_SOCKET) return 3;
    ZeroMemory(&local_address, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    local_address.sin_port = htons(agent_port);
    if (bind(local_socket, (const struct sockaddr *)&local_address, sizeof(local_address)) == SOCKET_ERROR) return 3;
    ZeroMemory(&tunnel_address, sizeof(tunnel_address));
    tunnel_address.sin_family = AF_INET;
    tunnel_address.sin_addr.S_un.S_addr = virtual_ip;
    tunnel_address.sin_port = htons(data_port);
    if (bind(tunnel_socket, (const struct sockaddr *)&tunnel_address, sizeof(tunnel_address)) == SOCKET_ERROR) return 3;
    ZeroMemory(&host_address, sizeof(host_address));
    host_address.sin_family = AF_INET;
    host_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    host_address.sin_port = htons(hook_port);
    ZeroMemory(&peer_address, sizeof(peer_address));
    peer_address.sin_family = AF_INET;
    peer_address.sin_port = htons(data_port);
    printf("READY %u %u\n", (unsigned)agent_port, (unsigned)data_port);
    fflush(stdout);
    for (;;) {
        fd_set read_set;
        struct timeval timeout;
        ULONGLONG now;
        int selected;
        FD_ZERO(&read_set);
        FD_SET(local_socket, &read_set);
        FD_SET(tunnel_socket, &read_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        selected = select(0, &read_set, NULL, NULL, &timeout);
        if (selected == SOCKET_ERROR) break;
        if (FD_ISSET(local_socket, &read_set)) {
            struct sockaddr_in source;
            int source_length = sizeof(source);
            int received = recvfrom(local_socket, packet, sizeof(packet), 0, (struct sockaddr *)&source, &source_length);
            if (received > (int)strlen(WG_GAME_PEER_PREFIX) &&
                memcmp(packet, WG_GAME_PEER_PREFIX, strlen(WG_GAME_PEER_PREFIX)) == 0) {
                emit_game_peer(packet, received);
            } else if (received >= 7 && memcmp(packet, "TARGET ", 7) == 0) {
                int size = received - 7;
                char peer_ip[64];
                if (size == 0) {
                    peer_address.sin_addr.S_un.S_addr = 0;
                    target_started = 0;
                    attempt_failed = 0;
                    if (connected) { connected = 0; emit_state(local_socket, &host_address, "disconnected"); }
                } else if (size < (int)sizeof(peer_ip)) {
                    memcpy(peer_ip, packet + 7, (size_t)size); peer_ip[size] = '\0';
                    if (InetPtonA(AF_INET, peer_ip, &peer_address.sin_addr) == 1) {
                        connected = 0; last_peer_packet = 0; next_hello = 0;
                        target_started = GetTickCount64(); attempt_failed = 0;
                    }
                }
            } else if (received > 0 && peer_address.sin_addr.S_un.S_addr != 0 && valid_packet(packet, received)) {
                sendto(tunnel_socket, packet, received, 0, (const struct sockaddr *)&peer_address, sizeof(peer_address));
            }
        }
        if (FD_ISSET(tunnel_socket, &read_set)) {
            struct sockaddr_in source;
            int source_length = sizeof(source);
            int received = recvfrom(tunnel_socket, packet, sizeof(packet), 0, (struct sockaddr *)&source, &source_length);
            if (peer_address.sin_addr.S_un.S_addr != 0 && source.sin_addr.S_un.S_addr == peer_address.sin_addr.S_un.S_addr) {
                if (received == 5 && memcmp(packet, "HELLO", 5) == 0) {
                    sendto(tunnel_socket, "HELLO", 5, 0, (const struct sockaddr *)&peer_address, sizeof(peer_address));
                } else if (received > 0 && valid_packet(packet, received)) {
                    sendto(local_socket, packet, received, 0, (const struct sockaddr *)&host_address, sizeof(host_address));
                } else {
                    continue;
                }
                last_peer_packet = GetTickCount64();
                if (!connected) { connected = 1; emit_state(local_socket, &host_address, "connected"); }
            }
        }
        now = GetTickCount64();
        if (peer_address.sin_addr.S_un.S_addr != 0 && (!connected || now >= next_hello)) {
            sendto(tunnel_socket, "HELLO", 5, 0, (const struct sockaddr *)&peer_address, sizeof(peer_address));
            next_hello = now + WG_HELLO_INTERVAL_MS;
        }
        if (connected && last_peer_packet != 0 && now - last_peer_packet >= WG_TIMEOUT_MS) {
            connected = 0;
            emit_state(local_socket, &host_address, "disconnected");
        }
        if (peer_address.sin_addr.S_un.S_addr != 0 && !connected && !attempt_failed &&
            target_started != 0 && now - target_started >= WG_CONNECT_TIMEOUT_MS) {
            attempt_failed = 1;
            emit_state(local_socket, &host_address, "failed");
        }
    }
    closesocket(local_socket); closesocket(tunnel_socket); WSACleanup();
    return 0;
}
