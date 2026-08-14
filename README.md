# WEL No-TAP

This repository is the isolated WEL implementation that runs WE8 without
TAP-Windows, n2n, WinDivert, a packet driver, virtual IP installation, or
system routes. It does not modify the current production client in
`welopenvpn-clean`.

## Current Status

`P2 - authenticated cloud relay` is implemented. P1 completed a real
two-computer WE8 match without a virtual adapter: all overlapping packets
and bytes matched in both directions, and the Hook reported zero queue drops.
The current connection GUI authenticates through the shared Go API and receives
its room address, logical IP, community, and relay credential from the separate
No-TAP controller.

The build contains two independent tools:

- `WEL无网卡联机.exe`: injects the virtual Socket Hook and connects WE8 through
  a UDP room relay.
- `WEL无网卡观测工具.exe`: retains the P0 observation workflow and does not
  alter network traffic.

The P2 data path is:

```text
WE8.exe
  -> welnpt.dll virtual UDP sockets
  -> one physical UDP transport socket
  -> authenticated Linux/Windows room relay on UDP 22333
  -> peer welnpt.dll
  -> peer WE8.exe recvfrom queue
```

The Hook virtualizes `socket`, `bind`, `getsockname`, `sendto`, `recvfrom`,
`WSASendTo`, `WSARecvFrom`, and `closesocket`. Each game Socket keeps its own
logical source port and receive queue. Empty nonblocking reads return
`WSAEWOULDBLOCK (10035)`, matching the successful real-LAN trace.

Protocol v2 authenticates every registration and game packet with a truncated
HMAC-SHA256 tag. The test token is never written to the JSONL game log.

## Build on Windows

Open an **x86 Native Tools Command Prompt for Visual Studio** and run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-windows.ps1
```

Outputs are written to `build\x86`:

```text
WEL无网卡联机.exe
welnpt.dll
welnptrelay.exe
WEL无网卡观测工具.exe
welnpttrace.dll
welnptgame.exe
```

All injected DLLs are x86 because the tested `WE8.exe` is x86.

## Two-Computer Cloud Test

Keep the complete artifact extracted on both computers.

On both computers:

1. Open `WEL无网卡联机.exe`.
2. Enter the Go API address, Laravel account, and password.
3. Select the same No-TAP room, for example `01 - 10.122.1.0/24`.
4. Select `WE8.exe` and click `登录并启动 WE8`.

The Go API assigns a different logical address to each active account. The
host then creates a room inside WE8; the client searches and joins normally.
The two players must select the same No-TAP room, but use their own Laravel
accounts. No TAP/n2n driver, system route, or manually copied token is needed.

Neither player needs an inbound WE8 port. Both players only send and receive
through one outbound UDP mapping to the cloud relay.

Both machines write a JSONL diagnostic log to the Desktop. Send both files
together after each test, whether it succeeds or fails.

## Linux Relay

Build with:

```bash
./scripts/build-linux-relay.sh
```

The production host uses the independent systemd unit in `deploy/systemd` and
an `/etc/welnpt-notap.env` file containing `WEL_NOTAP_PORT` and the private
relay secret. It does not share a process, port, configuration, or restart cycle
with the current TAP/n2n platform.

## Scope and Security

P2 authenticates packets with a deployment relay secret. The GUI does not
embed that secret: the Go API authenticates the Laravel account and returns
the room credential for the current lease. The relay secret is still shared
by the No-TAP deployment and should be rotated as an operational secret.

The current protocol does not encrypt WE8 payloads. Production hardening should
add per-player credentials, replay protection, and rate limits. P2P can be
added later as an optimization; a reliable authenticated relay remains the
baseline.

Detailed design and the confirmed WE8 Socket timeline are in
[`docs/NO_TAP_ARCHITECTURE_ZH.md`](docs/NO_TAP_ARCHITECTURE_ZH.md).
