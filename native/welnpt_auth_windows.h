#ifndef WELNPT_AUTH_WINDOWS_H
#define WELNPT_AUTH_WINDOWS_H

#include <bcrypt.h>

#include "welnpt_protocol.h"

#define WELNPT_AUTH_SECRET_MAX 128
#define WELNPT_SHA256_LENGTH 32
#define WELNPT_AUTH_OBJECT_MAX 512

/* BCrypt hash objects are thread-owned. Keeping the common SHA-256 object in
   TLS removes a heap allocation from every packet without sharing handles
   between the game's networking threads. */
static __declspec(thread) unsigned char g_welnpt_auth_object[WELNPT_AUTH_OBJECT_MAX];

typedef struct welnpt_auth_context {
    BCRYPT_ALG_HANDLE provider;
    DWORD object_length;
    char secret[WELNPT_AUTH_SECRET_MAX];
} welnpt_auth_context;

static int welnpt_auth_initialize(welnpt_auth_context *context, const char *secret) {
    DWORD copied = 0;
    size_t secret_length;
    NTSTATUS status;
    if (context == NULL || secret == NULL) return 0;
    secret_length = strlen(secret);
    if (secret_length < 8 || secret_length >= sizeof(context->secret)) return 0;
    ZeroMemory(context, sizeof(*context));
    strcpy_s(context->secret, sizeof(context->secret), secret);
    status = BCryptOpenAlgorithmProvider(&context->provider, BCRYPT_SHA256_ALGORITHM,
        NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) return 0;
    status = BCryptGetProperty(context->provider, BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&context->object_length, sizeof(context->object_length), &copied, 0);
    if (status < 0 || copied != sizeof(context->object_length) || context->object_length == 0) {
        BCryptCloseAlgorithmProvider(context->provider, 0);
        context->provider = NULL;
        return 0;
    }
    return 1;
}

static int welnpt_auth_digest(welnpt_auth_context *context, const char *packet,
    int packet_length, unsigned char digest[WELNPT_SHA256_LENGTH]) {
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object;
    int heap_object = 0;
    NTSTATUS status;
    if (context == NULL || context->provider == NULL || packet == NULL || packet_length < 0) return 0;
    if (context->object_length <= sizeof(g_welnpt_auth_object)) {
        object = g_welnpt_auth_object;
    } else {
        object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, context->object_length);
        if (object == NULL) return 0;
        heap_object = 1;
    }
    status = BCryptCreateHash(context->provider, &hash, object, context->object_length,
        (PUCHAR)context->secret, (ULONG)strlen(context->secret), 0);
    if (status >= 0) status = BCryptHashData(hash, (PUCHAR)packet, (ULONG)packet_length, 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest, WELNPT_SHA256_LENGTH, 0);
    if (hash != NULL) BCryptDestroyHash(hash);
    SecureZeroMemory(object, context->object_length);
    if (heap_object) HeapFree(GetProcessHeap(), 0, object);
    return status >= 0;
}

/* Verification must hash the packet with auth_tag treated as zero. Hash the
   three logical segments directly instead of copying and clearing a full
   maximum-sized datagram for every received packet. */
static int welnpt_auth_digest_without_tag(welnpt_auth_context *context, const char *packet,
    int packet_length, unsigned char digest[WELNPT_SHA256_LENGTH]) {
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object;
    int heap_object = 0;
    NTSTATUS status;
    static const unsigned char zero_tag[WELNPT_AUTH_TAG_LENGTH] = { 0 };
    const int tag_offset = (int)sizeof(welnpt_packet_header) - WELNPT_AUTH_TAG_LENGTH;
    const int suffix_offset = tag_offset + WELNPT_AUTH_TAG_LENGTH;

    if (context == NULL || context->provider == NULL || packet == NULL ||
        packet_length < (int)sizeof(welnpt_packet_header)) return 0;
    if (context->object_length <= sizeof(g_welnpt_auth_object)) {
        object = g_welnpt_auth_object;
    } else {
        object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, context->object_length);
        if (object == NULL) return 0;
        heap_object = 1;
    }
    status = BCryptCreateHash(context->provider, &hash, object, context->object_length,
        (PUCHAR)context->secret, (ULONG)strlen(context->secret), 0);
    if (status >= 0) status = BCryptHashData(hash, (PUCHAR)packet, (ULONG)tag_offset, 0);
    if (status >= 0) status = BCryptHashData(hash, (PUCHAR)zero_tag, sizeof(zero_tag), 0);
    if (status >= 0 && packet_length > suffix_offset) {
        status = BCryptHashData(hash, (PUCHAR)(packet + suffix_offset),
            (ULONG)(packet_length - suffix_offset), 0);
    }
    if (status >= 0) status = BCryptFinishHash(hash, digest, WELNPT_SHA256_LENGTH, 0);
    if (hash != NULL) BCryptDestroyHash(hash);
    SecureZeroMemory(object, context->object_length);
    if (heap_object) HeapFree(GetProcessHeap(), 0, object);
    return status >= 0;
}

static int welnpt_auth_sign(welnpt_auth_context *context, char *packet, int packet_length) {
    welnpt_packet_header *header;
    unsigned char digest[WELNPT_SHA256_LENGTH];
    if (packet == NULL || packet_length < (int)sizeof(welnpt_packet_header)) return 0;
    header = (welnpt_packet_header *)packet;
    ZeroMemory(header->auth_tag, sizeof(header->auth_tag));
    if (!welnpt_auth_digest(context, packet, packet_length, digest)) return 0;
    CopyMemory(header->auth_tag, digest, WELNPT_AUTH_TAG_LENGTH);
    SecureZeroMemory(digest, sizeof(digest));
    return 1;
}

static int welnpt_auth_verify(welnpt_auth_context *context, const char *packet, int packet_length) {
    const welnpt_packet_header *header;
    unsigned char expected[WELNPT_AUTH_TAG_LENGTH];
    unsigned char digest[WELNPT_SHA256_LENGTH];
    unsigned char difference = 0;
    int index;
    if (packet == NULL || packet_length < (int)sizeof(welnpt_packet_header) ||
        packet_length > (int)(sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD)) return 0;
    header = (const welnpt_packet_header *)packet;
    CopyMemory(expected, header->auth_tag, sizeof(expected));
    if (!welnpt_auth_digest_without_tag(context, packet, packet_length, digest)) return 0;
    for (index = 0; index < WELNPT_AUTH_TAG_LENGTH; ++index) difference |= expected[index] ^ digest[index];
    SecureZeroMemory(expected, sizeof(expected));
    SecureZeroMemory(digest, sizeof(digest));
    return difference == 0;
}

#endif
