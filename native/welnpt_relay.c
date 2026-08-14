#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "welnpt_protocol.h"

#define WELNPT_DEFAULT_RELAY_PORT 22333
#define WELNPT_MAX_PEERS 256
#define WELNPT_PEER_TIMEOUT_MS 30000

typedef struct relay_peer {
    int active;
    char room[WELNPT_ROOM_LENGTH];
    uint32_t logical_ip;
    struct sockaddr_in endpoint;
    ULONGLONG last_seen;
} relay_peer;

static relay_peer g_peers[WELNPT_MAX_PEERS];

static int same_room(const char left[WELNPT_ROOM_LENGTH], const char right[WELNPT_ROOM_LENGTH]) {
    return memcmp(left, right, WELNPT_ROOM_LENGTH) == 0;
}

static void endpoint_text(const struct sockaddr_in *endpoint, char *result, size_t count) {
    char ip[INET_ADDRSTRLEN] = "?";
    InetNtopA(AF_INET, (PVOID)&endpoint->sin_addr, ip, ARRAYSIZE(ip));
    _snprintf_s(result, count, _TRUNCATE, "%s:%u", ip, (unsigned)ntohs(endpoint->sin_port));
}

static relay_peer *upsert_peer(const welnpt_packet_header *header, const struct sockaddr_in *endpoint) {
    relay_peer *free_peer = NULL;
    relay_peer *oldest_peer = NULL;
    size_t index;
    ULONGLONG now = GetTickCount64();

    for (index = 0; index < ARRAYSIZE(g_peers); ++index) {
        relay_peer *peer = &g_peers[index];
        if (peer->active && now - peer->last_seen > WELNPT_PEER_TIMEOUT_MS) peer->active = 0;
        if (peer->active && peer->logical_ip == header->source_ip && same_room(peer->room, header->room)) {
            peer->endpoint = *endpoint;
            peer->last_seen = now;
            return peer;
        }
        if (!peer->active && free_peer == NULL) free_peer = peer;
        if (peer->active && (oldest_peer == NULL || peer->last_seen < oldest_peer->last_seen)) {
            oldest_peer = peer;
        }
    }

    if (free_peer == NULL) free_peer = oldest_peer;
    if (free_peer == NULL) return NULL;
    ZeroMemory(free_peer, sizeof(*free_peer));
    free_peer->active = 1;
    CopyMemory(free_peer->room, header->room, WELNPT_ROOM_LENGTH);
    free_peer->logical_ip = header->source_ip;
    free_peer->endpoint = *endpoint;
    free_peer->last_seen = now;
    return free_peer;
}

static int forward_data(SOCKET socket_handle, const char *packet, int packet_length,
    const welnpt_packet_header *header) {
    size_t index;
    int delivered = 0;
    int is_broadcast = (header->flags & WELNPT_FLAG_BROADCAST) != 0 ||
        header->target_ip == INADDR_BROADCAST;
    ULONGLONG now = GetTickCount64();

    for (index = 0; index < ARRAYSIZE(g_peers); ++index) {
        relay_peer *peer = &g_peers[index];
        if (!peer->active) continue;
        if (now - peer->last_seen > WELNPT_PEER_TIMEOUT_MS) {
            peer->active = 0;
            continue;
        }
        if (!same_room(peer->room, header->room)) continue;
        if (is_broadcast) {
            if (peer->logical_ip == header->source_ip) continue;
        } else if (peer->logical_ip != header->target_ip) {
            continue;
        }
        if (sendto(socket_handle, packet, packet_length, 0,
            (const struct sockaddr *)&peer->endpoint, sizeof(peer->endpoint)) == packet_length) {
            ++delivered;
        }
    }
    return delivered;
}

static int self_test(void) {
    welnpt_packet_header header;
    welnpt_initialize_header(&header, WELNPT_PACKET_DATA);
    if (sizeof(header) != 58 || !welnpt_valid_header(&header)) return 1;
    puts("SELF-TEST OK");
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    WSADATA winsock_data;
    SOCKET socket_handle;
    struct sockaddr_in local_address;
    wchar_t *end = NULL;
    unsigned long parsed_port = WELNPT_DEFAULT_RELAY_PORT;
    char packet[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];

    if (argc > 1 && wcscmp(argv[1], L"--self-test") == 0) return self_test();
    if (argc > 1) {
        parsed_port = wcstoul(argv[1], &end, 10);
        if (end == argv[1] || *end != L'\0' || parsed_port == 0 || parsed_port > 65535) {
            fputs("Usage: welnptrelay [port]\n", stderr);
            return 2;
        }
    }
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) return 3;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return 4;
    }
    ZeroMemory(&local_address, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons((u_short)parsed_port);
    if (bind(socket_handle, (const struct sockaddr *)&local_address, sizeof(local_address)) == SOCKET_ERROR) {
        fprintf(stderr, "Relay bind failed on UDP %lu: Winsock error %d\n", parsed_port, WSAGetLastError());
        closesocket(socket_handle);
        WSACleanup();
        return 5;
    }
    printf("WEL no-TAP relay listening on UDP %lu\n", parsed_port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in source;
        int source_length = sizeof(source);
        int received = recvfrom(socket_handle, packet, sizeof(packet), 0,
            (struct sockaddr *)&source, &source_length);
        welnpt_packet_header *header;
        unsigned payload_length;
        relay_peer *peer;

        if (received == SOCKET_ERROR) continue;
        if (received < (int)sizeof(welnpt_packet_header)) continue;
        header = (welnpt_packet_header *)packet;
        if (!welnpt_valid_header(header)) continue;
        payload_length = (unsigned)ntohs(header->payload_length);
        if (payload_length > WELNPT_MAX_PAYLOAD ||
            received != (int)(sizeof(welnpt_packet_header) + payload_length)) continue;
        peer = upsert_peer(header, &source);
        if (peer == NULL) continue;
        if (header->type == WELNPT_PACKET_REGISTER) {
            char endpoint[80];
            char logical_ip[INET_ADDRSTRLEN] = "?";
            endpoint_text(&source, endpoint, sizeof(endpoint));
            InetNtopA(AF_INET, &header->source_ip, logical_ip, ARRAYSIZE(logical_ip));
            printf("REGISTER room=%.*s player=%s endpoint=%s\n",
                WELNPT_ROOM_LENGTH, header->room, logical_ip, endpoint);
            fflush(stdout);
        } else if (header->type == WELNPT_PACKET_DATA) {
            forward_data(socket_handle, packet, received, header);
        }
    }
}
