#ifndef WELNPT_PROTOCOL_H
#define WELNPT_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#define WELNPT_PROTOCOL_VERSION 2
#define WELNPT_ROOM_LENGTH 32
#define WELNPT_AUTH_TAG_LENGTH 16
#define WELNPT_MAX_PAYLOAD 4096
#define WELNPT_PACKET_REGISTER 1
#define WELNPT_PACKET_DATA 2
#define WELNPT_FLAG_BROADCAST 0x01

#pragma pack(push, 1)
typedef struct welnpt_packet_header {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t reserved;
    char room[WELNPT_ROOM_LENGTH];
    uint32_t source_ip;
    uint16_t source_port;
    uint32_t target_ip;
    uint16_t target_port;
    uint16_t payload_length;
    uint32_t sequence;
    uint8_t auth_tag[WELNPT_AUTH_TAG_LENGTH];
} welnpt_packet_header;
#pragma pack(pop)

static void welnpt_initialize_header(welnpt_packet_header *header, uint8_t type) {
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, "WNP2", 4);
    header->version = WELNPT_PROTOCOL_VERSION;
    header->type = type;
}

static int welnpt_valid_header(const welnpt_packet_header *header) {
    return memcmp(header->magic, "WNP2", 4) == 0 &&
        header->version == WELNPT_PROTOCOL_VERSION &&
        (header->type == WELNPT_PACKET_REGISTER || header->type == WELNPT_PACKET_DATA);
}

#endif
