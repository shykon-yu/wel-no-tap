# WEL No-TAP

This repository is the isolated WEL implementation that runs WE8 without
TAP-Windows, n2n, WinDivert, a packet driver, virtual IP installation, or
system routes. It does not modify the current production client in
`welopenvpn-clean`.

## Current Status

`P1 - LAN relay validation` is implemented.

The build contains two independent tools:

- `WEL无网卡联机测试.exe`: injects the virtual Socket Hook and performs a real
  two-computer search/join/match test through a UDP room relay.
- `WEL无网卡观测工具.exe`: retains the P0 observation workflow and does not
  alter network traffic.

The P1 data path is:

```text
WE8.exe
  -> welnpt.dll virtual UDP sockets
  -> one physical UDP transport socket
  -> welnptrelay.exe room relay
  -> peer welnpt.dll
  -> peer WE8.exe recvfrom queue
```

The Hook virtualizes `socket`, `bind`, `getsockname`, `sendto`, `recvfrom`,
`WSASendTo`, `WSARecvFrom`, and `closesocket`. Each game Socket keeps its own
logical source port and receive queue. Empty nonblocking reads return
`WSAEWOULDBLOCK (10035)`, matching the successful real-LAN trace.

## Build on Windows

Open an **x86 Native Tools Command Prompt for Visual Studio** and run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-windows.ps1
```

Outputs are written to `build\x86`:

```text
WEL无网卡联机测试.exe
welnpt.dll
welnptrelay.exe
WEL无网卡观测工具.exe
welnpttrace.dll
welnptgame.exe
```

All injected DLLs are x86 because the tested `WE8.exe` is x86.

## Two-Computer LAN Test

Keep the complete artifact extracted on both computers.

Host computer:

1. Open `WEL无网卡联机测试.exe`.
2. Select `主机（自动启动本机中继）`.
3. Keep relay `127.0.0.1:22333`, room `wel-test-room`, logical IP
   `10.250.1.1`.
4. Select `WE8.exe`, start, then create a host inside WE8.
5. Keep the GUI open while testing because it owns the local relay process.

Client computer:

1. Open the same GUI and select `客机（连接已有中继）`.
2. Replace the relay host with the host computer's real LAN IPv4, for example
   `192.168.3.124:22333`.
3. Use the same room and a different logical IP, for example `10.250.1.2`.
4. Select `WE8.exe`, start, then search and join normally.

Allow `welnptrelay.exe` on the host if Windows Firewall prompts. P1 uses one
host-side inbound UDP port, `22333`; it does not require any WE8, random game
port, virtual-subnet, or ICMP firewall rules.

Both machines write a JSONL diagnostic log to the Desktop. Send both files
together after each test, whether it succeeds or fails.

## Scope and Security

P1 proves the virtual-Socket and relay data path. Its room name is a routing
key, not authentication, and the LAN relay has no encryption or rate limiting.
Do not expose this prototype relay directly to the public Internet.

Before production integration, the relay protocol must use platform-issued
session credentials, replay protection, packet limits, and a managed server.
P2P can be added later as an optimization; a reliable authenticated relay
remains the baseline.

Detailed design and the confirmed WE8 Socket timeline are in
[`docs/NO_TAP_ARCHITECTURE_ZH.md`](docs/NO_TAP_ARCHITECTURE_ZH.md).
