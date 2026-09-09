#ifndef WELNPT_PROTOCOL_H
#define WELNPT_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define WELNPT_UNUSED __attribute__((unused))
#else
#define WELNPT_UNUSED
#endif

#define WELNPT_PROTOCOL_VERSION 3
#define WELNPT_ROOM_LENGTH 32
#define WELNPT_MAX_PAYLOAD 4096
#define WELNPT_PACKET_REGISTER 1
#define WELNPT_PACKET_DATA 2
#define WELNPT_PACKET_PING 3
#define WELNPT_PACKET_PONG 4
#define WELNPT_FLAG_BROADCAST 0x01
#define WELNPT_HOST_MAGIC "WHF1"
#define WELNPT_HOST_VERSION 1
#define WELNPT_HOST_FRAME_HELLO 1
#define WELNPT_HOST_FRAME_DATA 2
#define WELNPT_HOST_FRAME_MAP 3
#define WELNPT_HOST_FRAME_CLOSE 4

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
} welnpt_packet_header;

/*
 * The game-process Hook uses this small loopback frame when the external Host
 * transport is enabled. It deliberately contains no room secret or HMAC: the
 * loopback endpoint is owned by the launcher-created Host process, which is
 * where the transport packet is built and verified.
 */
typedef struct welnpt_host_frame {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t reserved;
    uint32_t source_ip;
    uint16_t source_port;
    uint32_t target_ip;
    uint16_t target_port;
    uint16_t payload_length;
    uint32_t sequence;
} welnpt_host_frame;
#pragma pack(pop)

static void welnpt_initialize_header(welnpt_packet_header *header, uint8_t type) {
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, "WNP3", 4);
    header->version = WELNPT_PROTOCOL_VERSION;
    header->type = type;
}

static int welnpt_valid_header(const welnpt_packet_header *header) {
    return memcmp(header->magic, "WNP3", 4) == 0 &&
        header->version == WELNPT_PROTOCOL_VERSION &&
        (header->type == WELNPT_PACKET_REGISTER || header->type == WELNPT_PACKET_DATA ||
         header->type == WELNPT_PACKET_PING || header->type == WELNPT_PACKET_PONG);
}

static WELNPT_UNUSED void welnpt_initialize_host_frame(welnpt_host_frame *frame, uint8_t type) {
    memset(frame, 0, sizeof(*frame));
    memcpy(frame->magic, WELNPT_HOST_MAGIC, 4);
    frame->version = WELNPT_HOST_VERSION;
    frame->type = type;
}

static WELNPT_UNUSED int welnpt_valid_host_frame(const welnpt_host_frame *frame) {
    return frame != NULL && memcmp(frame->magic, WELNPT_HOST_MAGIC, 4) == 0 &&
        frame->version == WELNPT_HOST_VERSION &&
        (frame->type == WELNPT_HOST_FRAME_HELLO || frame->type == WELNPT_HOST_FRAME_DATA ||
         frame->type == WELNPT_HOST_FRAME_MAP || frame->type == WELNPT_HOST_FRAME_CLOSE);
}

#endif
