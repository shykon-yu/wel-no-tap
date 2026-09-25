#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <glib.h>
#include <nice/agent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "welnpt_protocol.h"

#define CONTROL_PREFIX "WELICESTATE:"
#define PEER_PREFIX "WELICEPEER:"
#define AGENT_PREFIX "WELICEAGENT:"
#define REMOTE_SET "WELICEREMOTESET"
#define SESSION_RESET "WELICESESSIONRESET"
#define TRANSPORT_PREFIX "WELTRANSPORT:"
#define GAME_PEER_PREFIX "WELGAMEPEER:"
#define BUFFER_SIZE 8192

static NiceAgent *g_agent;
static guint g_stream;
static SOCKET g_loopback = INVALID_SOCKET;
static struct sockaddr_in g_hook;
static unsigned short g_local_port;
static volatile LONG g_stopping;
static volatile LONG g_connected;
static volatile LONG g_hook_active;
static volatile LONG g_standby;
static CRITICAL_SECTION g_output_lock;

static void output(const char *format, ...) {
    va_list args;
    EnterCriticalSection(&g_output_lock);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fputc('\n', stdout);
    fflush(stdout);
    LeaveCriticalSection(&g_output_lock);
}

static void hook_message(const char *prefix, const char *value) {
    char packet[256];
    int length = _snprintf_s(packet, sizeof(packet), _TRUNCATE, "%s%s", prefix, value ? value : "");
    if (length > 0 && g_loopback != INVALID_SOCKET && g_hook.sin_port != 0)
        sendto(g_loopback, packet, length, 0, (const struct sockaddr *)&g_hook, sizeof(g_hook));
}

static void notify_hook_agent(unsigned short port) {
    char message[96];
    int length = _snprintf_s(message, sizeof(message), _TRUNCATE, "%s%u", AGENT_PREFIX, (unsigned)port);
    if (length > 0 && g_loopback != INVALID_SOCKET && g_hook.sin_port != 0)
        sendto(g_loopback, message, length, 0, (const struct sockaddr *)&g_hook, sizeof(g_hook));
}

static void on_component_state_changed(NiceAgent *agent, guint stream_id, guint component_id,
                                       guint state, gpointer user_data) {
    const char *name = nice_component_state_to_string((NiceComponentState)state);
    (void)agent; (void)stream_id; (void)component_id; (void)user_data;
    output("STATE %s", name ? name : "unknown");
    if (state == NICE_COMPONENT_STATE_CONNECTED || state == NICE_COMPONENT_STATE_READY) {
        InterlockedExchange(&g_connected, 1);
        if (InterlockedCompareExchange(&g_hook_active, 0, 0)) hook_message(CONTROL_PREFIX, "connected");
    } else if (state == NICE_COMPONENT_STATE_DISCONNECTED) {
        InterlockedExchange(&g_connected, 0);
        if (InterlockedCompareExchange(&g_hook_active, 0, 0)) hook_message(CONTROL_PREFIX, "disconnected");
    } else if (state == NICE_COMPONENT_STATE_FAILED) {
        InterlockedExchange(&g_connected, 0);
        if (InterlockedCompareExchange(&g_hook_active, 0, 0)) hook_message(CONTROL_PREFIX, "failed");
    }
}

static void on_candidate_gathering_done(NiceAgent *agent, guint stream_id, gpointer user_data) {
    gchar *sdp;
    (void)user_data;
    sdp = nice_agent_generate_local_sdp(agent);
    if (sdp == NULL) { output("ERROR local-description"); return; }
    output("LOCAL_SDP_BEGIN");
    fputs(sdp, stdout);
    if (sdp[0] && sdp[strlen(sdp) - 1] != '\n') fputc('\n', stdout);
    fputs("LOCAL_SDP_END\n", stdout);
    fflush(stdout);
    g_free(sdp);
    output("LOCAL_PORT %u", (unsigned)g_local_port);
    (void)stream_id;
}

static void on_recv(NiceAgent *agent, guint stream_id, guint component_id, guint length,
                    gchar *buffer, gpointer user_data) {
    (void)agent; (void)stream_id; (void)component_id; (void)user_data;
    if (length > 0 && length <= BUFFER_SIZE && g_loopback != INVALID_SOCKET && g_hook.sin_port)
        sendto(g_loopback, buffer, (int)length, 0, (const struct sockaddr *)&g_hook, sizeof(g_hook));
}

static DWORD WINAPI loopback_thread(LPVOID unused) {
    char buffer[BUFFER_SIZE];
    (void)unused;
    while (!InterlockedCompareExchange(&g_stopping, 0, 0)) {
        int length;
        struct sockaddr_in source;
        int source_length = sizeof(source);
        length = recvfrom(g_loopback, buffer, sizeof(buffer), 0, (struct sockaddr *)&source, &source_length);
        if (length <= 0) continue;
        if (length > (int)strlen("WELICESTATE?") && memcmp(buffer, "WELICESTATE?", strlen("WELICESTATE?")) == 0) {
            hook_message(CONTROL_PREFIX, InterlockedCompareExchange(&g_connected, 0, 0) ? "connected" : "connecting");
        } else if (length > (int)strlen(GAME_PEER_PREFIX) && memcmp(buffer, GAME_PEER_PREFIX, strlen(GAME_PEER_PREFIX)) == 0) {
            output("GAME_PEER %.*s", length - (int)strlen(GAME_PEER_PREFIX), buffer + strlen(GAME_PEER_PREFIX));
        } else if (length > (int)strlen(TRANSPORT_PREFIX) && memcmp(buffer, TRANSPORT_PREFIX, strlen(TRANSPORT_PREFIX)) == 0) {
            output("TRANSPORT_STATE %.*s", length - (int)strlen(TRANSPORT_PREFIX), buffer + strlen(TRANSPORT_PREFIX));
        } else if (InterlockedCompareExchange(&g_connected, 0, 0)) {
            nice_agent_send(g_agent, g_stream, NICE_COMPONENT_TYPE_RTP, length, buffer);
        }
    }
    return 0;
}

static int parse_port(const char *value, unsigned short *port) {
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end || parsed == 0 || parsed > 65535) return 0;
    *port = (unsigned short)parsed;
    return 1;
}

static char *read_remote_sdp(void) {
    char line[1024];
    size_t used = 0, capacity = 16384;
    char *sdp = g_malloc0(capacity);
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strcmp(line, "REMOTE_SDP_END\n") == 0 || strcmp(line, "REMOTE_SDP_END\r\n") == 0) break;
        if (used + strlen(line) + 1 >= capacity) { g_free(sdp); return NULL; }
        memcpy(sdp + used, line, strlen(line));
        used += strlen(line);
        sdp[used] = '\0';
    }
    return used ? sdp : NULL;
}

static DWORD WINAPI command_thread(LPVOID unused) {
    char command[256];
    (void)unused;
    while (!InterlockedCompareExchange(&g_stopping, 0, 0) && fgets(command, sizeof(command), stdin)) {
        char *newline = strpbrk(command, "\r\n");
        if (newline) *newline = '\0';
        if (!strcmp(command, "EXIT")) { InterlockedExchange(&g_stopping, 1); break; }
        if (!strcmp(command, "REMOTE_SDP_BEGIN")) {
            char *sdp = read_remote_sdp();
            if (!sdp || nice_agent_parse_remote_sdp(g_agent, sdp) < 0) output("ERROR remote-description");
            else { nice_agent_peer_candidate_gathering_done(g_agent, g_stream); output("REMOTE_SET"); hook_message(REMOTE_SET, ""); }
            g_free(sdp);
        } else if (!strncmp(command, "TARGET ", 7)) {
            hook_message(PEER_PREFIX, command + 7);
            output("TARGET_SET %s", command + 7);
        } else if (!strcmp(command, "RESET_SESSION")) {
            if (InterlockedCompareExchange(&g_hook_active, 0, 0)) {
                hook_message(SESSION_RESET, "");
            }
        } else if (!strcmp(command, "ACTIVATE")) {
            InterlockedExchange(&g_standby, 0);
            InterlockedExchange(&g_hook_active, 1);
            notify_hook_agent(g_local_port);
            hook_message(CONTROL_PREFIX, "connecting");
        }
    }
    InterlockedExchange(&g_stopping, 1);
    return 0;
}

int main(int argc, char **argv) {
    const char *stun_host = NULL;
    const char *turn_host = NULL, *turn_user = NULL, *turn_password = NULL;
    unsigned short stun_port = 0, turn_port = 0, hook_port = 0, ice_port = 0;
    WSADATA winsock;
    struct sockaddr_in local;
    HANDLE receiver = NULL, commands = NULL;
    guint state_handler, gathering_handler;
    int index;
    int standby = 0;
    for (index = 1; index < argc; ++index) {
        if (!strcmp(argv[index], "--stun-host") && index + 1 < argc) stun_host = argv[++index];
        else if (!strcmp(argv[index], "--stun-port") && index + 1 < argc) { if (!parse_port(argv[++index], &stun_port)) return 2; }
        else if (!strcmp(argv[index], "--turn-host") && index + 1 < argc) turn_host = argv[++index];
        else if (!strcmp(argv[index], "--turn-port") && index + 1 < argc) { if (!parse_port(argv[++index], &turn_port)) return 2; }
        else if (!strcmp(argv[index], "--turn-user") && index + 1 < argc) turn_user = argv[++index];
        else if (!strcmp(argv[index], "--turn-password") && index + 1 < argc) turn_password = argv[++index];
        else if (!strcmp(argv[index], "--hook-port") && index + 1 < argc) { if (!parse_port(argv[++index], &hook_port)) return 2; }
        else if (!strcmp(argv[index], "--ice-port") && index + 1 < argc) { if (!parse_port(argv[++index], &ice_port)) return 2; }
        else if (!strcmp(argv[index], "--standby")) standby = 1;
        else if (!strcmp(argv[index], "--validate-args")) return stun_host && stun_port && hook_port ? 0 : 2;
        else if (!strcmp(argv[index], "--self-test")) return 0;
        else if (!strcmp(argv[index], "--room") || !strcmp(argv[index], "--logical-ip") || !strcmp(argv[index], "--relay") || !strcmp(argv[index], "--token") || !strcmp(argv[index], "--session-key")) { if (index + 1 < argc) ++index; }
        else return 2;
    }
    if (!stun_host || !stun_port || !hook_port) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_output_lock);
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 3;
    g_loopback = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_loopback == INVALID_SOCKET) return 4;
    ZeroMemory(&local, sizeof(local)); local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_loopback, (struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) return 5;
    int local_length = sizeof(local); getsockname(g_loopback, (struct sockaddr *)&local, &local_length);
    ZeroMemory(&g_hook, sizeof(g_hook)); g_hook.sin_family = AF_INET; g_hook.sin_addr.s_addr = htonl(INADDR_LOOPBACK); g_hook.sin_port = htons(hook_port);
    g_local_port = ntohs(local.sin_port);
    output("LOCAL_PORT %u", (unsigned)g_local_port);
    InterlockedExchange(&g_standby, standby ? 1 : 0);
    InterlockedExchange(&g_hook_active, standby ? 0 : 1);
    {
        char port_text[16];
        _snprintf_s(port_text, sizeof(port_text), _TRUNCATE, "%u", (unsigned)g_local_port);
        if (!standby) {
            hook_message(AGENT_PREFIX, port_text);
            hook_message(CONTROL_PREFIX, "connecting");
        }
    }
    output("GATHERING_STARTED %s %u", stun_host, (unsigned)stun_port);

    g_agent = nice_agent_new(NULL, NICE_COMPATIBILITY_RFC5245);
    if (!g_agent) return 6;
    g_object_set(g_agent, "stun-server", stun_host, "stun-server-port", (guint)stun_port, NULL);
    g_stream = nice_agent_add_stream(g_agent, 1);
    if (!g_stream) return 7;
    nice_agent_set_stream_name(g_agent, g_stream, "we8");
    if (turn_host && turn_port && turn_user && turn_password) {
        if (!nice_agent_set_relay_info(g_agent, g_stream, NICE_COMPONENT_TYPE_RTP, turn_host, turn_port,
                                       turn_user, turn_password, NICE_RELAY_TYPE_TURN_UDP)) {
            output("ERROR relay-config");
        }
    }
    if (ice_port) nice_agent_set_port_range(g_agent, g_stream, NICE_COMPONENT_TYPE_RTP, ice_port, ice_port);
    state_handler = g_signal_connect(g_agent, "component-state-changed", G_CALLBACK(on_component_state_changed), NULL);
    gathering_handler = g_signal_connect(g_agent, "candidate-gathering-done", G_CALLBACK(on_candidate_gathering_done), NULL);
    (void)state_handler; (void)gathering_handler;
    nice_agent_attach_recv(g_agent, g_stream, NICE_COMPONENT_TYPE_RTP, NULL, on_recv, NULL);
    if (!nice_agent_gather_candidates(g_agent, g_stream)) return 8;
    receiver = CreateThread(NULL, 0, loopback_thread, NULL, 0, NULL);
    commands = CreateThread(NULL, 0, command_thread, NULL, 0, NULL);
    while (!InterlockedCompareExchange(&g_stopping, 0, 0)) g_main_context_iteration(NULL, TRUE);
    if (receiver) { WaitForSingleObject(receiver, 1000); CloseHandle(receiver); }
    if (commands) { WaitForSingleObject(commands, 1000); CloseHandle(commands); }
    g_object_unref(g_agent); closesocket(g_loopback); WSACleanup(); DeleteCriticalSection(&g_output_lock);
    return 0;
}
