# WEL No-TAP Prototype

This is an isolated prototype for the future WEL client that does not install
TAP-Windows, n2n, a packet driver, or system routes. It does not change the
current production client in `welopenvpn-clean`.

## Status

`P0 - Winsock observation` is implemented. `WEL无网卡观测工具.exe` is the
preferred graphical launcher: choose `WE8.exe`, choose the local role, then
click start. It starts WE8 in a suspended state, injects `welnpttrace.dll`,
waits for the hook-ready event, and then resumes the game. The DLL writes a
line-delimited trace for the network APIs that determine whether a later
virtual-socket layer can deliver packets correctly:

- `bind`, `sendto`, `WSASendTo`
- `recvfrom`, `WSARecvFrom`
- `select`, `WSAEventSelect`, `WSAAsyncSelect`, `WSAWaitForMultipleEvents`,
  `WSAPoll`
- `closesocket`

The prototype deliberately does **not** change a packet destination, source
address, bind address, or receive result. It is observation-only and cannot
join an online room yet.

## Why P0 Is Required

The successful TAP captures establish the game packet contract:

```text
Search: client logical-IP:random-port -> room broadcast:5739
Reply:  host logical-IP:5739           -> client logical-IP:original-random-port
Join:   client random-port <-> host 5739
```

Without a virtual adapter, the Windows UDP stack cannot naturally receive a
packet whose source is a remote logical IP. The final Hook must synthesize
`recvfrom`/`WSARecvFrom` results and must also make WE8 observe that its socket
is readable. We already have packet data; this probe establishes the missing
runtime fact: which receive-ready mechanism this WE8 build actually uses.

## Build on Windows

Open a **x86 Native Tools Command Prompt for Visual Studio** in this directory
and run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-windows.ps1
```

The output is written to `build\x86`:

```text
welnptgame.exe
welnpttrace.dll
WEL无网卡观测工具.exe
```

The DLL is x86 because the tested `WE8.exe` is x86. Do not inject it into a
64-bit game process.

## Run a Local Trace

Keep `WEL无网卡观测工具.exe` and `welnpttrace.dll` in the same extracted
directory. On each computer:

1. Open `WEL无网卡观测工具.exe`.
2. Select `WE8.exe`.
3. Select `Host-A` on the hosting computer and `Client-B` on the joining computer.
4. Click **Start observation and launch WE8**.
5. Complete the normal LAN host, search, join, and match test.
6. Exit WE8 and use **Open results directory** to find the Desktop `.jsonl` file.

The console `welnptgame.exe` is retained for automation only. Send the two
generated trace files together with the existing `.welcap.zip` packages.

## Next Stages

1. Confirm the receive-ready API from P0 trace data.
2. Add a per-socket logical address table and a localhost proxy protocol.
3. Replace outgoing WE8 UDP sends with room-relay envelopes.
4. Feed proxy packets back through the observed receive and readiness APIs.
5. Build a room relay with authenticated fan-out and unicast delivery.
6. Verify search, join, and a 10-minute match with Windows Firewall enabled.

Detailed packet and compatibility requirements are in
[`docs/NO_TAP_ARCHITECTURE_ZH.md`](docs/NO_TAP_ARCHITECTURE_ZH.md).
