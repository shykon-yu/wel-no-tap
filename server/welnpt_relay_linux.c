#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../native/welnpt_protocol.h"

#define WELNPT_DEFAULT_PORT 22333
#define WELNPT_MAX_PEERS 2048
#define WELNPT_PEER_TIMEOUT_MS 30000
#define WELNPT_SECRET_MAX 128
#define WELNPT_SHA256_LENGTH 32

typedef struct relay_peer {
    int active;
    char room[WELNPT_ROOM_LENGTH];
    uint32_t logical_ip;
    struct sockaddr_in endpoint;
    uint64_t last_seen;
} relay_peer;

typedef struct relay_stats {
    uint64_t received_packets;
    uint64_t received_bytes;
    uint64_t forwarded_packets;
    uint64_t forwarded_bytes;
    uint64_t authentication_drops;
    uint64_t malformed_drops;
    uint64_t route_drops;
} relay_stats;

static relay_peer g_peers[WELNPT_MAX_PEERS];
static relay_stats g_stats;
static char g_secret[WELNPT_SECRET_MAX];
static volatile sig_atomic_t g_stopping;

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static void stop_signal(int signal_number) {
    (void)signal_number;
    g_stopping = 1;
}

static int same_room(const char left[WELNPT_ROOM_LENGTH], const char right[WELNPT_ROOM_LENGTH]) {
    return memcmp(left, right, WELNPT_ROOM_LENGTH) == 0;
}

static int authenticate_packet(const char *packet, size_t packet_length) {
    unsigned char authenticated[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
    unsigned char expected[WELNPT_AUTH_TAG_LENGTH];
    unsigned char digest[WELNPT_SHA256_LENGTH];
    unsigned int digest_length = 0;
    welnpt_packet_header *header;
    unsigned char difference = 0;
    size_t index;

    if (packet == NULL || packet_length < sizeof(welnpt_packet_header) ||
        packet_length > sizeof(authenticated)) return 0;
    memcpy(authenticated, packet, packet_length);
    header = (welnpt_packet_header *)authenticated;
    memcpy(expected, header->auth_tag, sizeof(expected));
    memset(header->auth_tag, 0, sizeof(header->auth_tag));
    if (HMAC(EVP_sha256(), g_secret, (int)strlen(g_secret), authenticated, packet_length,
        digest, &digest_length) == NULL || digest_length != WELNPT_SHA256_LENGTH) return 0;
    for (index = 0; index < WELNPT_AUTH_TAG_LENGTH; ++index) difference |= expected[index] ^ digest[index];
    memset(authenticated, 0, sizeof(authenticated));
    memset(expected, 0, sizeof(expected));
    memset(digest, 0, sizeof(digest));
    return difference == 0;
}

static int sign_packet(char *packet, size_t packet_length) {
    welnpt_packet_header *header = (welnpt_packet_header *)packet;
    unsigned char digest[WELNPT_SHA256_LENGTH];
    unsigned int digest_length = 0;
    memset(header->auth_tag, 0, sizeof(header->auth_tag));
    if (HMAC(EVP_sha256(), g_secret, (int)strlen(g_secret), (unsigned char *)packet,
        packet_length, digest, &digest_length) == NULL || digest_length != WELNPT_SHA256_LENGTH) return 0;
    memcpy(header->auth_tag, digest, WELNPT_AUTH_TAG_LENGTH);
    memset(digest, 0, sizeof(digest));
    return 1;
}

static relay_peer *upsert_peer(const welnpt_packet_header *header,
    const struct sockaddr_in *endpoint, uint64_t now) {
    relay_peer *free_peer = NULL;
    relay_peer *oldest_peer = NULL;
    size_t index;

    for (index = 0; index < WELNPT_MAX_PEERS; ++index) {
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
    memset(free_peer, 0, sizeof(*free_peer));
    free_peer->active = 1;
    memcpy(free_peer->room, header->room, WELNPT_ROOM_LENGTH);
    free_peer->logical_ip = header->source_ip;
    free_peer->endpoint = *endpoint;
    free_peer->last_seen = now;
    return free_peer;
}

static int forward_packet(int socket_handle, const char *packet, size_t packet_length,
    const welnpt_packet_header *header, uint64_t now) {
    int delivered = 0;
    int is_broadcast = (header->flags & WELNPT_FLAG_BROADCAST) != 0 ||
        header->target_ip == INADDR_BROADCAST;
    size_t index;

    for (index = 0; index < WELNPT_MAX_PEERS; ++index) {
        relay_peer *peer = &g_peers[index];
        ssize_t sent;
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
        sent = sendto(socket_handle, packet, packet_length, 0,
            (const struct sockaddr *)&peer->endpoint, sizeof(peer->endpoint));
        if (sent == (ssize_t)packet_length) {
            ++delivered;
            ++g_stats.forwarded_packets;
            g_stats.forwarded_bytes += packet_length;
        }
    }
    return delivered;
}

static size_t active_peer_count(uint64_t now) {
    size_t active = 0;
    size_t index;
    for (index = 0; index < WELNPT_MAX_PEERS; ++index) {
        if (g_peers[index].active && now - g_peers[index].last_seen <= WELNPT_PEER_TIMEOUT_MS) ++active;
    }
    return active;
}

static void log_stats(uint64_t now) {
    fprintf(stdout,
        "stats peers=%zu received_packets=%llu received_bytes=%llu forwarded_packets=%llu "
        "forwarded_bytes=%llu auth_drops=%llu malformed_drops=%llu route_drops=%llu\n",
        active_peer_count(now),
        (unsigned long long)g_stats.received_packets,
        (unsigned long long)g_stats.received_bytes,
        (unsigned long long)g_stats.forwarded_packets,
        (unsigned long long)g_stats.forwarded_bytes,
        (unsigned long long)g_stats.authentication_drops,
        (unsigned long long)g_stats.malformed_drops,
        (unsigned long long)g_stats.route_drops);
    fflush(stdout);
}

static int self_test(void) {
    char packet[sizeof(welnpt_packet_header) + 4];
    welnpt_packet_header *header = (welnpt_packet_header *)packet;
    strcpy(g_secret, "local-test-token");
    welnpt_initialize_header(header, WELNPT_PACKET_DATA);
    header->payload_length = htons(4);
    memcpy(packet + sizeof(*header), "test", 4);
    if (sizeof(*header) != 74 || !welnpt_valid_header(header) ||
        !sign_packet(packet, sizeof(packet)) || !authenticate_packet(packet, sizeof(packet))) return 1;
    packet[sizeof(*header)] ^= 1;
    if (authenticate_packet(packet, sizeof(packet))) return 1;
    puts("SELF-TEST OK");
    return 0;
}

int main(int argc, char **argv) {
    const char *port_text;
    const char *secret;
    char *port_end = NULL;
    unsigned long port;
    int socket_handle;
    int receive_buffer = 4 * 1024 * 1024;
    struct timeval receive_timeout;
    struct sockaddr_in local_address;
    char packet[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
    uint64_t last_stats;

    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) return self_test();
    port_text = getenv("WEL_NOTAP_PORT");
    secret = getenv("WEL_NOTAP_TOKEN");
    if (port_text == NULL || *port_text == '\0') port_text = "22333";
    port = strtoul(port_text, &port_end, 10);
    if (port_end == port_text || *port_end != '\0' || port == 0 || port > 65535) {
        fputs("WEL_NOTAP_PORT must be a valid UDP port\n", stderr);
        return 2;
    }
    if (secret == NULL || strlen(secret) < 8 || strlen(secret) >= sizeof(g_secret)) {
        fputs("WEL_NOTAP_TOKEN must contain 8-127 characters\n", stderr);
        return 3;
    }
    strcpy(g_secret, secret);
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle < 0) {
        perror("socket");
        return 4;
    }
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    receive_timeout.tv_sec = 1;
    receive_timeout.tv_usec = 0;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    memset(&local_address, 0, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons((uint16_t)port);
    if (bind(socket_handle, (const struct sockaddr *)&local_address, sizeof(local_address)) != 0) {
        perror("bind");
        close(socket_handle);
        return 5;
    }
    fprintf(stdout, "WEL no-TAP relay protocol=2 listening=0.0.0.0:%lu/udp\n", port);
    fflush(stdout);
    last_stats = monotonic_ms();

    while (!g_stopping) {
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        ssize_t received = recvfrom(socket_handle, packet, sizeof(packet), 0,
            (struct sockaddr *)&source, &source_length);
        welnpt_packet_header *header;
        uint16_t payload_length;
        uint64_t now;
        relay_peer *peer;
        int delivered;

        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recvfrom");
            continue;
        }
        now = monotonic_ms();
        ++g_stats.received_packets;
        g_stats.received_bytes += (uint64_t)received;
        if (received < (ssize_t)sizeof(welnpt_packet_header)) {
            ++g_stats.malformed_drops;
            continue;
        }
        header = (welnpt_packet_header *)packet;
        payload_length = ntohs(header->payload_length);
        if (!welnpt_valid_header(header) || payload_length > WELNPT_MAX_PAYLOAD ||
            received != (ssize_t)(sizeof(*header) + payload_length)) {
            ++g_stats.malformed_drops;
            continue;
        }
        if (!authenticate_packet(packet, (size_t)received)) {
            ++g_stats.authentication_drops;
            continue;
        }
        peer = upsert_peer(header, &source, now);
        if (peer == NULL) {
            ++g_stats.route_drops;
            continue;
        }
        if (header->type == WELNPT_PACKET_DATA) {
            delivered = forward_packet(socket_handle, packet, (size_t)received, header, now);
            if (delivered == 0) ++g_stats.route_drops;
        } else if (header->type == WELNPT_PACKET_PING) {
            if (header->target_ip != 0 && header->target_ip != htonl(INADDR_BROADCAST)) {
                /* 转发式 PING：目标玩家在房间内则转过去，中继只当中转，不回包 */
                delivered = forward_packet(socket_handle, packet, (size_t)received, header, now);
                if (delivered == 0) ++g_stats.route_drops;
            } else {
                /* 到中继服务器的 PING：就地回 PONG，保持链路健康检查语义 */
                char pong[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
                welnpt_packet_header *pong_header = (welnpt_packet_header *)pong;
                memcpy(pong, packet, (size_t)received);
                pong_header->type = WELNPT_PACKET_PONG;
                if (sign_packet(pong, (size_t)received) && sendto(socket_handle, pong, received, 0,
                    (const struct sockaddr *)&source, sizeof(source)) == received) {
                    ++g_stats.forwarded_packets;
                    g_stats.forwarded_bytes += (uint64_t)received;
                }
            }
        } else if (header->type == WELNPT_PACKET_PONG) {
            /* 转发式 PING 的回包：按 target_ip 转发回发起方 */
            delivered = forward_packet(socket_handle, packet, (size_t)received, header, now);
            if (delivered == 0) ++g_stats.route_drops;
        }
        if (now - last_stats >= 60000) {
            log_stats(now);
            last_stats = now;
        }
    }
    log_stats(monotonic_ms());
    memset(g_secret, 0, sizeof(g_secret));
    close(socket_handle);
    return 0;
}
