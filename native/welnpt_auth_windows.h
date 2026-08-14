#ifndef WELNPT_AUTH_WINDOWS_H
#define WELNPT_AUTH_WINDOWS_H

#include <bcrypt.h>

#include "welnpt_protocol.h"

#define WELNPT_AUTH_SECRET_MAX 128
#define WELNPT_SHA256_LENGTH 32

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
    NTSTATUS status;
    if (context == NULL || context->provider == NULL || packet == NULL || packet_length < 0) return 0;
    object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, context->object_length);
    if (object == NULL) return 0;
    status = BCryptCreateHash(context->provider, &hash, object, context->object_length,
        (PUCHAR)context->secret, (ULONG)strlen(context->secret), 0);
    if (status >= 0) status = BCryptHashData(hash, (PUCHAR)packet, (ULONG)packet_length, 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest, WELNPT_SHA256_LENGTH, 0);
    if (hash != NULL) BCryptDestroyHash(hash);
    SecureZeroMemory(object, context->object_length);
    HeapFree(GetProcessHeap(), 0, object);
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
    char authenticated[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
    welnpt_packet_header *header;
    unsigned char expected[WELNPT_AUTH_TAG_LENGTH];
    unsigned char digest[WELNPT_SHA256_LENGTH];
    unsigned char difference = 0;
    int index;
    if (packet == NULL || packet_length < (int)sizeof(welnpt_packet_header) ||
        packet_length > (int)sizeof(authenticated)) return 0;
    CopyMemory(authenticated, packet, (size_t)packet_length);
    header = (welnpt_packet_header *)authenticated;
    CopyMemory(expected, header->auth_tag, sizeof(expected));
    ZeroMemory(header->auth_tag, sizeof(header->auth_tag));
    if (!welnpt_auth_digest(context, authenticated, packet_length, digest)) return 0;
    for (index = 0; index < WELNPT_AUTH_TAG_LENGTH; ++index) difference |= expected[index] ^ digest[index];
    SecureZeroMemory(authenticated, sizeof(authenticated));
    SecureZeroMemory(expected, sizeof(expected));
    SecureZeroMemory(digest, sizeof(digest));
    return difference == 0;
}

#endif
