# WEL 无虚拟网卡方案设计与 P1 实现基准

> 更新日期：2026-08-14
> 当前阶段：P1 局域网中继联机验证
> 当前限制：未接平台鉴权，不可直接暴露到公网

## 1. 目标与边界

本仓库在不安装以下组件的前提下，让 WE8 完成搜索、加入和比赛：

- TAP-Windows、OpenVPN、n2n。
- WinDivert 或其他内核包过滤驱动。
- 虚拟 IP、系统路由和 ARP 项。

平台只搬运 WE8 原始 UDP 数据报，不分析比赛操作、比分或游戏状态。
现有 `welopenvpn-clean` TAP/n2n 生产客户端保持不变。

## 2. 成功 Socket 样本

2026-08-14 的真实局域网成功样本确认 WE8 只调用了：

```text
socket
bind
sendto
recvfrom
closesocket
```

没有观察到 `select`、`WSAEventSelect`、`WSAAsyncSelect`、
`WSAWaitForMultipleEvents`、`WSAPoll`、`WSASendTo` 或 `WSARecvFrom`。

WE8 约每 15 至 16 毫秒轮询一次非阻塞 `recvfrom`：

- 队列为空：`SOCKET_ERROR / WSAEWOULDBLOCK (10035)`。
- 队列有包：返回一个完整 UDP 数据报及其来源地址。
- `INVALID_SOCKET`：保持 Windows 原始行为，返回 `10038`。

### 2.1 完整联机时间线

```text
搜索：
  客机 192.168.3.89:51440 -> 255.255.255.255:5739  24 字节
  主机 192.168.3.124:5739 -> 客机 :51440          136 字节

加入请求：
  客机新 Socket :65396 -> 主机 :5739             64 字节
  未接受前约每 3 秒重发

主机接受：
  主机创建临时 Socket :53228 -> 客机 :65396      84 字节
  发送后立即关闭该临时 Socket

正式联机：
  客机 :65396 <-> 主机 :5739
  持续双向 UDP，保持每个数据报边界
```

客机成功发出 `3951` 个 UDP 数据报，主机样本成功收到 `3951` 个。

### 2.2 必须保留的语义

1. 搜索 Socket 和加入 Socket 是两个不同 Socket，随机端口不同。
2. 主机固定监听 `5739`，但不是所有主机出站包都来自 `5739`。
3. 主机接受加入时必须保留临时随机源端口和 84 字节数据报。
4. 不能把所有包归并成 `5739 -> 5739`。
5. 不能合并、拆分、解析或修改 WE8 载荷。

## 3. P1 架构

```text
WE8.exe
  |
  | 虚拟 UDP Socket 调用
  v
welnpt.dll
  |  每个 Socket：逻辑端口 + 独立接收队列
  |  每个玩家：一个真实物理 UDP transport Socket
  v
welnptrelay.exe:22333/UDP
  |  广播：按房间扇出给其他成员
  |  单播：按目标逻辑 IP 投递
  v
对端 welnpt.dll -> 对端 WE8.exe
```

P1 为缩短验证路径，由 DLL 直接持有 transport Socket，没有再增加一个本机代理
进程。接入正式平台时可以把 transport 和认证移到平台主进程，但游戏可见的
Socket 契约保持不变。

## 4. 虚拟 Socket 契约

### 4.1 Socket 生命周期

Hook 跟踪 WE8 创建的每个 IPv4 UDP Socket：

- `bind(...:5739)` 记录逻辑端口 `5739`，不在物理网卡真正监听。
- 未显式绑定的 Socket 第一次发送或接收时分配 `49152-65535` 逻辑随机端口。
- `getsockname` 返回本玩家逻辑 IP 和该 Socket 的逻辑端口。
- `closesocket` 删除映射并释放未消费的接收队列。
- 不认识的句柄直接交还 Winsock，保证 `INVALID_SOCKET -> 10038`。

### 4.2 出站

`sendto` 将一次游戏调用封装为一个中继数据报：

```text
房间
来源逻辑 IP + 当前 Socket 逻辑端口
目标逻辑 IP + 游戏目标端口
广播标记
原始载荷长度
原始载荷
诊断序号
```

返回值保持游戏语义：中继 transport 成功接收封装后，Hook 向 WE8 返回原始
载荷长度。

### 4.3 入站

DLL 的 transport 线程接收中继数据，按目标逻辑端口找到 WE8 Socket，并将
数据报加入该 Socket 的 FIFO 队列。

WE8 调用 `recvfrom` 时：

- 队列非空：返回原始载荷，并把 `sockaddr_in` 合成为远端逻辑 IP 和原始逻辑
  源端口。
- 队列为空：返回 `SOCKET_ERROR` 并设置 `10035`。
- 不依赖可读事件，因为当前 WE8 样本没有使用等待 API。

实现仍保留同步 `WSASendTo/WSARecvFrom` 防御性兼容；重叠 I/O 在 P1 明确返回
不支持，因为成功样本没有走该路径。

## 5. 中继协议

P1 使用固定 58 字节头部，所有地址、端口和数字均使用网络字节序：

```text
magic[4] = WNP1
version[1]
type[1] = register | data
flags[1] = broadcast
reserved[1]
room[32]
sourceIPv4[4]
sourcePort[2]
targetIPv4[4]
targetPort[2]
payloadLength[2]
sequence[4]
payload[n]
```

最大 WE8 载荷为 4096 字节。成功样本的实际包远小于该上限，因此不会产生
IP 分片风险；正式公网版本仍应把安全载荷上限收紧到经完整样本确认的值。

中继按 `(room, logical IP)` 保存玩家的真实 UDP endpoint，30 秒无活动后淘汰，
客户端每 2 秒注册一次以维持 NAT 映射。

## 6. P1 局域网测试

主机：

```text
角色：主机
中继：127.0.0.1:22333
房间：wel-test-room
逻辑 IP：10.250.1.1
```

主机 GUI 会隐藏启动 `welnptrelay.exe 22333`。测试期间不能关闭主机 GUI。

客机：

```text
角色：客机
中继：<主机真实局域网 IPv4>:22333
房间：wel-test-room
逻辑 IP：10.250.1.2
```

两端必须使用同一房间和不同逻辑 IP。Windows 首次询问
`welnptrelay.exe` 网络访问权限时，需要允许专用网络。该测试只需要主机
`22333/UDP` 入站，不需要放行 WE8 或逻辑随机端口，因为 WE8 数据从不直接
进入物理网络。

## 7. 验收标准

P1 依次验证：

1. 客机搜索广播出现在客机日志，目标端口为 `5739`。
2. 主机日志收到 24 字节搜索包，来源为客机逻辑随机端口。
3. 客机收到来源 `主机逻辑 IP:5739` 的 136 字节回复并显示主机。
4. 主机收到客机新随机端口发出的 64 字节加入请求。
5. 客机收到主机临时随机端口发出的 84 字节接受包。
6. 两端进入比赛并稳定交换 UDP 至少 10 分钟。
7. 设备管理器、`ipconfig` 和路由表中没有新增 WEL 网卡、IP 或路由。

失败时必须同时提交两端桌面生成的 `WEL-NoTap-*.jsonl`。

## 8. 上线前缺口

P1 的房间名只是路由键，不是安全凭证。公网版本必须补齐：

- 平台签发的短期房间凭证和玩家身份绑定。
- 数据包认证、防伪造和重放保护。
- 每房间、每用户的速率与带宽限制。
- 中继健康检查、监控、容量规划和多节点调度。
- 断线重注册和明确的会话过期语义。
- Windows 10/11、多实体网卡、其他 VPN 共存和防火墙开启测试。

P2P 只作为后续降低延迟和带宽成本的优化。正式版本必须始终保留可靠中继
作为基线，不能再次让 NAT 类型决定玩家是否能联机。
