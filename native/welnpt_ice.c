#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define JUICE_STATIC

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <juice/juice.h>

#include "welnpt_protocol.h"
#include "welnpt_auth_windows.h"

#define WEL_ICE_BUFFER_SIZE 8192
#define WEL_ICE_CONTROL_PREFIX "WELICESTATE:"
#define WEL_ICE_PEER_PREFIX "WELICEPEER:"
#define WEL_GAME_PEER_PREFIX "WELGAMEPEER:"
#define WEL_ICE_PING_PREFIX "WELICEPING:"
#define WEL_ICE_PONG_PREFIX "WELICEPONG:"

static juice_agent_t *g_agent;
static SOCKET g_local_socket = INVALID_SOCKET;
static SOCKET g_relay_socket = INVALID_SOCKET;
static struct sockaddr_in g_relay_address;
static uint32_t g_logical_ip;
static char g_room[WELNPT_ROOM_LENGTH];
static welnpt_auth_context g_auth;
static struct sockaddr_in g_hook_address;
static volatile LONG g_stopping;
static volatile LONG g_connected;
static volatile LONG g_hook_active;
static CRITICAL_SECTION g_output_lock;
static CRITICAL_SECTION g_ping_lock;
static char g_ping_nonce[64];
static LARGE_INTEGER g_counter_frequency;
static LARGE_INTEGER g_ping_started;
static char g_relay_ping_nonce[64];
static LARGE_INTEGER g_relay_ping_started;
static char g_relay_peer_ping_nonce[64];
static LARGE_INTEGER g_relay_peer_ping_started;

static void send_relay_presence(void);

static double elapsed_milliseconds(LARGE_INTEGER started) {
	LARGE_INTEGER current;
	if (g_counter_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&current)) return 0.0;
	return ((double)(current.QuadPart - started.QuadPart) * 1000.0) /
		(double)g_counter_frequency.QuadPart;
}

static void output_line(const char *format, ...) {
	va_list arguments;
	EnterCriticalSection(&g_output_lock);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	fputc('\n', stdout);
	fflush(stdout);
	LeaveCriticalSection(&g_output_lock);
}

static void notify_hook(const char *state) {
	char message[96];
	int length = _snprintf_s(message, sizeof(message), _TRUNCATE, "%s%s", WEL_ICE_CONTROL_PREFIX, state);
	if (length > 0 && g_local_socket != INVALID_SOCKET && g_hook_address.sin_port != 0) {
		sendto(g_local_socket, message, length, 0, (const struct sockaddr *)&g_hook_address, sizeof(g_hook_address));
	}
}

static void notify_hook_peer(const char *logical_ip) {
	char message[96];
	int length = _snprintf_s(message, sizeof(message), _TRUNCATE, "%s%s", WEL_ICE_PEER_PREFIX, logical_ip);
	if (length > 0 && g_local_socket != INVALID_SOCKET && g_hook_address.sin_port != 0) {
		sendto(g_local_socket, message, length, 0, (const struct sockaddr *)&g_hook_address, sizeof(g_hook_address));
	}
}

static void notify_hook_agent(unsigned short port) {
	char message[96];
	int length = _snprintf_s(message, sizeof(message), _TRUNCATE, "WELICEAGENT:%u", (unsigned)port);
	if (length > 0 && g_local_socket != INVALID_SOCKET && g_hook_address.sin_port != 0) {
		sendto(g_local_socket, message, length, 0, (const struct sockaddr *)&g_hook_address, sizeof(g_hook_address));
	}
}

static void on_state_changed(juice_agent_t *agent, juice_state_t state, void *user_ptr) {
	const char *name = juice_state_to_string(state);
	(void)agent;
	(void)user_ptr;
	InterlockedExchange(&g_connected, state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED);
	output_line("STATE %s", name == NULL ? "unknown" : name);
	if (InterlockedCompareExchange(&g_hook_active, 0, 0) != 0) {
		notify_hook(name == NULL ? "unknown" : name);
	}
}

static void on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr) {
	const char *type = "unknown";
	(void)agent;
	(void)user_ptr;
	if (sdp != NULL) {
		if (strstr(sdp, " typ host") != NULL) type = "host";
		else if (strstr(sdp, " typ srflx") != NULL) type = "srflx";
		else if (strstr(sdp, " typ relay") != NULL) type = "relay";
	}
	output_line("CANDIDATE %s", type);
}

static void on_gathering_done(juice_agent_t *agent, void *user_ptr) {
	char description[JUICE_MAX_SDP_STRING_LEN];
	(void)user_ptr;
	if (juice_get_local_description(agent, description, sizeof(description)) != JUICE_ERR_SUCCESS) {
		output_line("ERROR local-description");
		return;
	}
	EnterCriticalSection(&g_output_lock);
	fputs("LOCAL_SDP_BEGIN\n", stdout);
	fputs(description, stdout);
	if (description[0] != '\0' && description[strlen(description) - 1] != '\n') fputc('\n', stdout);
	fputs("LOCAL_SDP_END\n", stdout);
	fflush(stdout);
	LeaveCriticalSection(&g_output_lock);
}

static void on_receive(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
	(void)user_ptr;
	if (size > strlen(WEL_ICE_PING_PREFIX) && memcmp(data, WEL_ICE_PING_PREFIX, strlen(WEL_ICE_PING_PREFIX)) == 0) {
		char response[128];
		int length = _snprintf_s(response, sizeof(response), _TRUNCATE, "%s%.*s", WEL_ICE_PONG_PREFIX,
			(int)(size - strlen(WEL_ICE_PING_PREFIX)), data + strlen(WEL_ICE_PING_PREFIX));
		if (length > 0) juice_send(agent, response, (size_t)length);
		return;
	}
	if (size > strlen(WEL_ICE_PONG_PREFIX) && memcmp(data, WEL_ICE_PONG_PREFIX, strlen(WEL_ICE_PONG_PREFIX)) == 0) {
		const char *nonce = data + strlen(WEL_ICE_PONG_PREFIX);
		size_t nonce_length = size - strlen(WEL_ICE_PONG_PREFIX);
		EnterCriticalSection(&g_ping_lock);
		if (g_ping_nonce[0] != '\0' && strlen(g_ping_nonce) == nonce_length && memcmp(g_ping_nonce, nonce, nonce_length) == 0) {
			output_line("PING_RESULT %s %.1f", g_ping_nonce, elapsed_milliseconds(g_ping_started));
			g_ping_nonce[0] = '\0';
		}
		LeaveCriticalSection(&g_ping_lock);
		return;
	}
	if (size <= WEL_ICE_BUFFER_SIZE && g_local_socket != INVALID_SOCKET && g_hook_address.sin_port != 0) {
		sendto(g_local_socket, data, (int)size, 0, (const struct sockaddr *)&g_hook_address, sizeof(g_hook_address));
	}
}

static DWORD WINAPI local_transport_thread(LPVOID unused) {
	char buffer[WEL_ICE_BUFFER_SIZE];
	(void)unused;
	while (InterlockedCompareExchange(&g_stopping, 0, 0) == 0) {
		struct sockaddr_in source;
		int source_length = sizeof(source);
		int received = recvfrom(g_local_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&source, &source_length);
		if (received == 12 && memcmp(buffer, "WELICESTATE?", 12) == 0) {
			if (InterlockedCompareExchange(&g_hook_active, 0, 0) != 0) {
				notify_hook(InterlockedCompareExchange(&g_connected, 0, 0) != 0 ? "connected" : "connecting");
			}
		} else if (received > (int)strlen(WEL_GAME_PEER_PREFIX) &&
			memcmp(buffer, WEL_GAME_PEER_PREFIX, strlen(WEL_GAME_PEER_PREFIX)) == 0) {
			char peer[INET_ADDRSTRLEN];
			int peer_length = received - (int)strlen(WEL_GAME_PEER_PREFIX);
			uint32_t peer_ip;
			const char *payload = buffer + strlen(WEL_GAME_PEER_PREFIX);
			const char *separator = memchr(payload, '|', (size_t)peer_length);
			int ip_length = separator == NULL ? peer_length : (int)(separator - payload);
			if (ip_length > 0 && ip_length < (int)sizeof(peer)) {
				memcpy(peer, payload, (size_t)ip_length);
				peer[ip_length] = '\0';
				if (InetPtonA(AF_INET, peer, &peer_ip) == 1) output_line("GAME_PEER %.*s", peer_length, payload);
			}
		} else if (received > 0 && InterlockedCompareExchange(&g_connected, 0, 0) != 0) {
			juice_send(g_agent, buffer, (size_t)received);
		}
	}
	return 0;
}

static DWORD WINAPI relay_receive_thread(LPVOID unused) {
	char packet[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
	ULONGLONG next_presence = GetTickCount64() + 10000;
	(void)unused;
	while (InterlockedCompareExchange(&g_stopping, 0, 0) == 0) {
		struct sockaddr_in source;
		int source_length = sizeof(source);
		int received = recvfrom(g_relay_socket, packet, sizeof(packet), 0, (struct sockaddr *)&source, &source_length);
		welnpt_packet_header *header;
		if (GetTickCount64() >= next_presence) {
			send_relay_presence();
			next_presence = GetTickCount64() + 10000;
		}
		if (received < (int)sizeof(welnpt_packet_header)) continue;
		header = (welnpt_packet_header *)packet;
		if (!welnpt_valid_header(header) || memcmp(header->room, g_room, WELNPT_ROOM_LENGTH) != 0 ||
			!welnpt_auth_verify(&g_auth, packet, received)) continue;
		if (header->type == WELNPT_PACKET_PING) {
			/* 中继转发的 PING：目标是自己则回 PONG，发回中继由其转发给发起方 */
			if (header->target_ip == g_logical_ip && g_relay_socket != INVALID_SOCKET) {
				char pong[sizeof(welnpt_packet_header) + WELNPT_MAX_PAYLOAD];
				welnpt_packet_header *pong_header = (welnpt_packet_header *)pong;
				memcpy(pong, packet, (size_t)received);
				pong_header->type = WELNPT_PACKET_PONG;
				pong_header->source_ip = g_logical_ip;
				pong_header->target_ip = header->source_ip;
				if (welnpt_auth_sign(&g_auth, pong, (size_t)received) &&
					sendto(g_relay_socket, pong, (size_t)received, 0,
						(const struct sockaddr *)&g_relay_address, sizeof(g_relay_address)) == received) {
					/* 转发式 PING 已应答 */
				}
			}
			continue;
		}
		if (header->type != WELNPT_PACKET_PONG) continue;
		EnterCriticalSection(&g_ping_lock);
		if (g_relay_ping_nonce[0] != '\0' && ntohl(header->sequence) == (uint32_t)strtoul(g_relay_ping_nonce, NULL, 10)) {
			output_line("RELAY_PING_RESULT %s %.1f", g_relay_ping_nonce,
				elapsed_milliseconds(g_relay_ping_started));
			g_relay_ping_nonce[0] = '\0';
		} else if (g_relay_peer_ping_nonce[0] != '\0' &&
			ntohl(header->sequence) == (uint32_t)strtoul(g_relay_peer_ping_nonce, NULL, 10)) {
			output_line("RELAY_PEER_PING_RESULT %s %.1f", g_relay_peer_ping_nonce,
				elapsed_milliseconds(g_relay_peer_ping_started));
			g_relay_peer_ping_nonce[0] = '\0';
		}
		LeaveCriticalSection(&g_ping_lock);
	}
	return 0;
}

static int read_remote_description(char *buffer, size_t capacity) {
	char line[1024];
	size_t used = 0;
	buffer[0] = '\0';
	while (fgets(line, sizeof(line), stdin) != NULL) {
		if (strcmp(line, "REMOTE_SDP_END\n") == 0 || strcmp(line, "REMOTE_SDP_END\r\n") == 0) return used > 0;
		if (used + strlen(line) + 1 > capacity) return 0;
		memcpy(buffer + used, line, strlen(line));
		used += strlen(line);
		buffer[used] = '\0';
	}
	return 0;
}

static void send_ping(const char *nonce) {
	char packet[128];
	int length;
	if (InterlockedCompareExchange(&g_connected, 0, 0) == 0) {
		output_line("PING_UNAVAILABLE %s", nonce);
		return;
	}
	EnterCriticalSection(&g_ping_lock);
	strncpy_s(g_ping_nonce, sizeof(g_ping_nonce), nonce, _TRUNCATE);
	QueryPerformanceCounter(&g_ping_started);
	length = _snprintf_s(packet, sizeof(packet), _TRUNCATE, "%s%s", WEL_ICE_PING_PREFIX, g_ping_nonce);
	if (length <= 0 || juice_send(g_agent, packet, (size_t)length) != JUICE_ERR_SUCCESS) {
		g_ping_nonce[0] = '\0';
		output_line("PING_UNAVAILABLE %s", nonce);
	}
	LeaveCriticalSection(&g_ping_lock);
}

static void send_relay_ping(const char *nonce) {
	char packet[sizeof(welnpt_packet_header)];
	welnpt_packet_header *header = (welnpt_packet_header *)packet;
	if (g_relay_socket == INVALID_SOCKET) {
		output_line("RELAY_PING_UNAVAILABLE %s", nonce);
		return;
	}
	EnterCriticalSection(&g_ping_lock);
	strncpy_s(g_relay_ping_nonce, sizeof(g_relay_ping_nonce), nonce, _TRUNCATE);
	QueryPerformanceCounter(&g_relay_ping_started);
	welnpt_initialize_header(header, WELNPT_PACKET_PING);
	CopyMemory(header->room, g_room, WELNPT_ROOM_LENGTH);
	header->source_ip = g_logical_ip;
	header->sequence = htonl((u_long)strtoul(nonce, NULL, 10));
	if (!welnpt_auth_sign(&g_auth, packet, sizeof(packet)) || sendto(g_relay_socket, packet, sizeof(packet), 0,
		(const struct sockaddr *)&g_relay_address, sizeof(g_relay_address)) == SOCKET_ERROR) {
		g_relay_ping_nonce[0] = '\0';
		output_line("RELAY_PING_UNAVAILABLE %s", nonce);
	}
	LeaveCriticalSection(&g_ping_lock);
}

static void send_relay_peer_ping(const char *nonce, uint32_t target_ip) {
	char packet[sizeof(welnpt_packet_header)];
	welnpt_packet_header *header = (welnpt_packet_header *)packet;
	if (g_relay_socket == INVALID_SOCKET || target_ip == 0) {
		output_line("RELAY_PEER_PING_UNAVAILABLE %s", nonce);
		return;
	}
	EnterCriticalSection(&g_ping_lock);
	strncpy_s(g_relay_peer_ping_nonce, sizeof(g_relay_peer_ping_nonce), nonce, _TRUNCATE);
	QueryPerformanceCounter(&g_relay_peer_ping_started);
	welnpt_initialize_header(header, WELNPT_PACKET_PING);
	CopyMemory(header->room, g_room, WELNPT_ROOM_LENGTH);
	header->source_ip = g_logical_ip;
	header->target_ip = target_ip;
	header->sequence = htonl((u_long)strtoul(nonce, NULL, 10));
	if (!welnpt_auth_sign(&g_auth, packet, sizeof(packet)) || sendto(g_relay_socket, packet, sizeof(packet), 0,
		(const struct sockaddr *)&g_relay_address, sizeof(g_relay_address)) == SOCKET_ERROR) {
		g_relay_peer_ping_nonce[0] = '\0';
		output_line("RELAY_PEER_PING_UNAVAILABLE %s", nonce);
	}
	LeaveCriticalSection(&g_ping_lock);
}

static void send_relay_presence(void) {
	char packet[sizeof(welnpt_packet_header)];
	welnpt_packet_header *header = (welnpt_packet_header *)packet;
	if (g_relay_socket == INVALID_SOCKET) return;
	welnpt_initialize_header(header, WELNPT_PACKET_PING);
	CopyMemory(header->room, g_room, WELNPT_ROOM_LENGTH);
	header->source_ip = g_logical_ip;
	header->sequence = 0;
	if (welnpt_auth_sign(&g_auth, packet, sizeof(packet))) {
		sendto(g_relay_socket, packet, sizeof(packet), 0,
			(const struct sockaddr *)&g_relay_address, sizeof(g_relay_address));
	}
}

static int parse_port(const char *value, unsigned short *port) {
	char *end = NULL;
	unsigned long parsed = strtoul(value, &end, 10);
	if (end == value || *end != '\0' || parsed == 0 || parsed > 65535) return 0;
	*port = (unsigned short)parsed;
	return 1;
}

static int initialize_relay(void) {
	char relay[256];
	char logical_ip[64];
	char token[WELNPT_AUTH_SECRET_MAX];
	char service[16];
	char *separator;
	struct addrinfo hints;
	struct addrinfo *addresses = NULL;
	DWORD timeout = 250;
	if (GetEnvironmentVariableA("WEL_NOTAP_RELAY", relay, sizeof(relay)) == 0 ||
		GetEnvironmentVariableA("WEL_NOTAP_LOGICAL_IP", logical_ip, sizeof(logical_ip)) == 0 ||
		GetEnvironmentVariableA("WEL_NOTAP_ROOM", g_room, sizeof(g_room)) == 0 ||
		GetEnvironmentVariableA("WEL_NOTAP_TOKEN", token, sizeof(token)) == 0) return 0;
	separator = strrchr(relay, ':');
	if (separator == NULL || separator == relay || separator[1] == '\0') return 0;
	strcpy_s(service, sizeof(service), separator + 1);
	*separator = '\0';
	if (InetPtonA(AF_INET, logical_ip, &g_logical_ip) != 1 || !welnpt_auth_initialize(&g_auth, token)) return 0;
	SecureZeroMemory(token, sizeof(token));
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	if (getaddrinfo(relay, service, &hints, &addresses) != 0 || addresses == NULL) return 0;
	CopyMemory(&g_relay_address, addresses->ai_addr, sizeof(g_relay_address));
	freeaddrinfo(addresses);
	g_relay_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_relay_socket == INVALID_SOCKET) return 0;
	setsockopt(g_relay_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	send_relay_presence();
	return 1;
}

int main(int argc, char **argv) {
	const char *stun_host = NULL;
	unsigned short stun_port = 0;
	unsigned short hook_port = 0;
	struct sockaddr_in local_address;
	juice_config_t config;
	WSADATA winsock;
	HANDLE transport_thread = NULL;
	HANDLE relay_thread = NULL;
	char command[256];
	int index;
	int validate_args = 0;
	int no_hook = 0;
	int standby = 0;
	DWORD timeout = 250;

	for (index = 1; index < argc; ++index) {
		if (strcmp(argv[index], "--stun-host") == 0) {
			if (index + 1 >= argc) return 2;
			stun_host = argv[++index];
		} else if (strcmp(argv[index], "--stun-port") == 0) {
			if (index + 1 >= argc || !parse_port(argv[++index], &stun_port)) return 2;
		} else if (strcmp(argv[index], "--hook-port") == 0) {
			if (index + 1 >= argc || !parse_port(argv[++index], &hook_port)) return 2;
		} else if (strcmp(argv[index], "--no-hook") == 0) {
			no_hook = 1;
		} else if (strcmp(argv[index], "--standby") == 0) {
			standby = 1;
		} else if (strcmp(argv[index], "--validate-args") == 0) validate_args = 1;
		else if (strcmp(argv[index], "--self-test") == 0) return 0;
		else return 2;
	}
	if (stun_host == NULL || stun_host[0] == '\0' || stun_port == 0 || (!no_hook && hook_port == 0)) return 2;
	if (validate_args) return 0;
	setvbuf(stdout, NULL, _IONBF, 0);
	InitializeCriticalSection(&g_output_lock);
	InitializeCriticalSection(&g_ping_lock);
	if (!QueryPerformanceFrequency(&g_counter_frequency)) return 3;
	if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 3;

	g_local_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_local_socket == INVALID_SOCKET) return 4;
	ZeroMemory(&local_address, sizeof(local_address));
	local_address.sin_family = AF_INET;
	local_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
	if (bind(g_local_socket, (const struct sockaddr *)&local_address, sizeof(local_address)) == SOCKET_ERROR) return 5;
	setsockopt(g_local_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	index = sizeof(local_address);
	if (getsockname(g_local_socket, (struct sockaddr *)&local_address, &index) == SOCKET_ERROR) return 6;
	ZeroMemory(&g_hook_address, sizeof(g_hook_address));
	if (!no_hook) {
		g_hook_address.sin_family = AF_INET;
		g_hook_address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
		g_hook_address.sin_port = htons(hook_port);
		InterlockedExchange(&g_hook_active, standby ? 0 : 1);
		if (!standby) {
			notify_hook_agent(ntohs(local_address.sin_port));
			notify_hook("connecting");
		}
	}

	ZeroMemory(&config, sizeof(config));
	config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
	/* WE8 is IPv4 UDP. Binding the ICE socket to an IPv4 wildcard prevents
	 * Win7's IPv6 tunnel adapters from becoming part of candidate gathering. */
	config.bind_address = "0.0.0.0";
	config.stun_server_host = stun_host;
	config.stun_server_port = stun_port;
	config.cb_state_changed = on_state_changed;
	config.cb_candidate = on_candidate;
	config.cb_gathering_done = on_gathering_done;
	config.cb_recv = on_receive;
	g_agent = juice_create(&config);
	if (g_agent == NULL) return 7;
	output_line("LOCAL_PORT %u", (unsigned)ntohs(local_address.sin_port));
	output_line("GATHERING_STARTED %s %u", stun_host, (unsigned)stun_port);
	transport_thread = CreateThread(NULL, 0, local_transport_thread, NULL, 0, NULL);
	if (transport_thread == NULL || juice_gather_candidates(g_agent) != JUICE_ERR_SUCCESS) return 8;
	if (initialize_relay()) relay_thread = CreateThread(NULL, 0, relay_receive_thread, NULL, 0, NULL);

	while (fgets(command, sizeof(command), stdin) != NULL) {
		char *newline = strpbrk(command, "\r\n");
		if (newline != NULL) *newline = '\0';
		if (strcmp(command, "REMOTE_SDP_BEGIN") == 0) {
			char remote[JUICE_MAX_SDP_STRING_LEN];
			if (!read_remote_description(remote, sizeof(remote)) || juice_set_remote_description(g_agent, remote) != JUICE_ERR_SUCCESS) {
				output_line("ERROR remote-description");
			} else {
				juice_set_remote_gathering_done(g_agent);
				output_line("REMOTE_SET");
			}
		} else if (strncmp(command, "PING ", 5) == 0 && command[5] != '\0') {
			send_ping(command + 5);
		} else if (strncmp(command, "TARGET ", 7) == 0 && command[7] != '\0') {
			uint32_t peer_ip;
			if (InetPtonA(AF_INET, command + 7, &peer_ip) == 1) {
				notify_hook_peer(command + 7);
				output_line("TARGET_SET %s", command + 7);
			} else {
				output_line("ERROR target");
			}
		} else if (strncmp(command, "PING_RELAY_PEER ", 16) == 0 && command[16] != '\0') {
			char *space = strchr(command + 16, ' ');
			if (space != NULL && space[1] != '\0') {
				char nonce[64];
				size_t nonce_length = (size_t)(space - (command + 16));
				uint32_t target_ip;
				if (nonce_length > 0 && nonce_length < sizeof(nonce)) {
					memcpy(nonce, command + 16, nonce_length);
					nonce[nonce_length] = '\0';
					if (InetPtonA(AF_INET, space + 1, &target_ip) == 1) {
						send_relay_peer_ping(nonce, target_ip);
					} else {
						output_line("RELAY_PEER_PING_UNAVAILABLE %s", nonce);
					}
				} else {
					output_line("RELAY_PEER_PING_UNAVAILABLE %s", command + 16);
				}
			} else {
				output_line("RELAY_PEER_PING_UNAVAILABLE %s", command + 16);
			}
		} else if (strncmp(command, "PING_RELAY ", 11) == 0 && command[11] != '\0') {
			send_relay_ping(command + 11);
		} else if (strcmp(command, "EXIT") == 0) {
			break;
		} else if (strcmp(command, "ACTIVATE") == 0 && !no_hook) {
			InterlockedExchange(&g_hook_active, 1);
			notify_hook_agent(ntohs(local_address.sin_port));
			notify_hook("connecting");
		}
	}

	InterlockedExchange(&g_stopping, 1);
	if (transport_thread != NULL) {
		WaitForSingleObject(transport_thread, 2000);
		CloseHandle(transport_thread);
	}
	if (relay_thread != NULL) {
		WaitForSingleObject(relay_thread, 2000);
		CloseHandle(relay_thread);
	}
	juice_destroy(g_agent);
	closesocket(g_local_socket);
	if (g_relay_socket != INVALID_SOCKET) closesocket(g_relay_socket);
	WSACleanup();
	DeleteCriticalSection(&g_ping_lock);
	DeleteCriticalSection(&g_output_lock);
	return 0;
}
