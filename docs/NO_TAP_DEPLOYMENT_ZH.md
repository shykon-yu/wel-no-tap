# WEL 无网卡平台上线部署清单

> 换服务器、首次完整部署或灾难恢复时，请按
> [`NO_TAP_SERVER_RUNBOOK_ZH.md`](NO_TAP_SERVER_RUNBOOK_ZH.md) 逐步执行；本文保留架构、
> 配置项与验收依据。

> 适用版本：`wel-no-tap` 当前 P3 直连 + P2 云中继架构
> 更新日期：2026-08-18
> 目标：明确 Laravel、Go、云中继、STUN、数据库和反向代理分别需要部署什么

## 1. 总体结构

无网卡平台由两个仓库和多个独立进程组成：

```text
玩家客户端
  -> HTTPS/HTTP -> Laravel 登录接口
  -> HTTPS/HTTP -> Go 平台 API
                    |-> MySQL：平台用户同步、房间和租约
                    |-> Redis：会话与在线状态
                    |-> Laravel platform-login：校验账号密码和平台期限
                    |-> UDP 22333：无网卡云中继
                    |-> UDP 3478：STUN，收集公网 candidate

游戏数据：
  WE8 -> welnpt.dll -> libjuice 直连（成功时）
                     -> UDP 22333 云中继（直连失败或广播时）
```

无网卡客户端不安装 TAP、n2n、OpenVPN、WinDivert 或其他虚拟网卡。`10.122.x.x`
是 Hook 和 Go 租约里的逻辑 IP，不是 Windows 网络适配器地址。

## 2. 服务清单

| 服务 | 所属 | 作用 | 是否无网卡必需 | 默认端口 |
|---|---|---|---|---:|
| Laravel API | `soccer_php` | 登录、账号状态、平台使用期限、后台管理 | 是 | 现有 HTTP/HTTPS |
| Laravel 前端 | `soccer_v3` | 用户管理和后台页面 | 管理员必需，客户端非必需 | 现有 HTTP/HTTPS |
| Go `platform-api` | `platform/backend` | No-TAP 登录桥接、房间、租约、ICE SDP 和 Ping | 是 | `8080/TCP`，生产可配置为 `8082` |
| MySQL | 平台基础设施 | Go 平台数据库和 `no_tap_*` 表 | 是 | `3306/TCP`，不应公网开放 |
| Redis | 平台基础设施 | Go JWT 会话/在线状态依赖 | 是 | `6379/TCP`，不应公网开放 |
| `welnpt-notap-relay` | `wel-no-tap/server` | 鉴权 UDP 中继，搜索广播和比赛回退 | 是 | `22333/UDP` |
| `wel-stun` / coturn | `wel-no-tap/deploy` | STUN candidate 收集和 ICE 探测 | 推荐 | `3478/UDP` |
| Nginx/HTTPS | 现有部署 | Laravel 和 Go API 的域名、TLS、反向代理 | 推荐 | `80/443 TCP` |
| TAP/n2n supernode | 现有 TAP 平台 | 只服务有虚拟网卡版本 | 无网卡不依赖 | `22222/UDP` |

TAP/n2n 服务可以和无网卡服务部署在同一台服务器，但必须保持端口、配置、数据库
房间和 systemd 单元独立。上线无网卡版本时不能重启 `weln2n-supernode` 或现有
TAP 平台 API。

## 3. Laravel 部署

Laravel 是账号权威来源。Go 不直接接管账号密码，而是调用 Laravel：

```text
POST /api/v1/auth/platform-login
```

该接口需要返回账号、用户 ID、昵称、状态和 `platform_access_expires_at`。用户必须
启用且平台权限未过期，Go 才会签发自己的平台 JWT。

Laravel 侧需要确认：

1. 生产 `.env` 的 APP_KEY、数据库、JWT 配置和 URL 已配置。
2. `platform-login` 可以被 Go API 服务器访问，不要只绑定到玩家本机地址。
3. Laravel 数据库迁移已完成，用户表和平台权限字段存在。
4. Nginx/PHP-FPM 正常，Laravel API 返回 JSON 而不是登录页或 302。
5. Laravel 账号的密码、状态和平台到期时间是最终判断依据。
6. 用户管理的固定超级管理员规则已部署，普通管理员不能在用户管理中修改自己。

建议用内部地址让 Go 调 Laravel，例如：

```env
SOCCER_AUTH_URL=http://127.0.0.1/api/v1/auth/platform-login
```

如果 Go 和 Laravel 不在同一台机器，使用内网域名或 HTTPS 地址，不要把数据库端口
直接暴露给公网。

## 4. Go 平台 API 部署

### 4.1 必需环境变量

Go API 运行前至少配置：

```env
API_PORT=8080
MYSQL_DSN=wel_platform:change-me@tcp(127.0.0.1:3306)/pes8_platform?parseTime=true&charset=utf8mb4&loc=UTC
REDIS_ADDR=127.0.0.1:6379
REDIS_PASSWORD=change-me
JWT_SECRET=use-a-long-random-production-secret
JWT_AUDIENCE=wel-no-tap
CORS_ORIGIN=https://admin.example.com,https://notap.example.com
SOCCER_AUTH_URL=https://api.example.com/api/v1/auth/platform-login

WEL_NOTAP_RELAY_HOST=relay.example.com
WEL_NOTAP_RELAY_PORT=22333
WEL_NOTAP_RELAY_TOKEN=与中继服务完全相同的随机密钥

WEL_NOTAP_ICE_STUN_HOST=stun.example.com
WEL_NOTAP_ICE_STUN_PORT=3478
```

如果 Go 需要直接同步 Laravel 用户，可配置：

```env
SOCCER_MYSQL_DSN=wel_laravel:change-me@tcp(127.0.0.1:3306)/laravel_database?parseTime=true&charset=utf8mb4&loc=UTC
```

账号登录仍以 `SOCCER_AUTH_URL` 的 Laravel 校验为准；`SOCCER_MYSQL_DSN` 不是把
Laravel 认证移到 Go 的替代方案。

### 4.2 数据库迁移

Go API 启动时会执行自己的平台迁移，并创建或升级：

```text
platform_schema_migrations
platform_users
no_tap_rooms
no_tap_room_leases
no_tap_peer_probes
```

当前无网卡房间固定为：

```text
直连 01 -> 10.122.1.0/24
直连 02 -> 10.122.2.0/24
中继 03 -> 10.122.3.0/24
中继 04 -> 10.122.4.0/24

`connection_mode=direct` 的房间进入时必须完成本机 ICE candidate 发布；
`connection_mode=relay` 的房间完全跳过 ICE，适合无法使用直连组件的玩家。
```

迁移前必须备份 `pes8_platform`。第一次启动后检查 `no_tap_rooms` 有 4 条记录，
并确认 `platform_schema_migrations` 已记录：

```text
20260814_create_no_tap_rooms
20260815_add_no_tap_ice_description
20260816_rename_no_tap_room_labels
20260816_add_no_tap_peer_probes
```

### 4.3 Go API 路由

无网卡控制器使用独立前缀，不能改成 TAP 的房间接口：

```text
/api/v1/notap/me/room-session
/api/v1/notap/rooms
/api/v1/notap/rooms/{roomID}/join
/api/v1/notap/rooms/{roomID}/heartbeat
/api/v1/notap/rooms/{roomID}/leave
/api/v1/notap/rooms/{roomID}/members
/api/v1/notap/rooms/{roomID}/ice
/api/v1/notap/rooms/{roomID}/peer-probes/*
```

启动后检查：

```bash
curl -fsS http://127.0.0.1:8080/healthz
```

如果生产把 Go API 绑定到 `8082`，客户端 `wel-no-tap.env` 中的
`WEL_API_BASE_URL` 必须使用对应的外部 URL；不要让客户端继续指向 `8080` 的旧地址。

## 5. 云中继部署

### 5.1 编译和安装

在 Linux 服务器编译：

```bash
./scripts/build-linux-relay.sh
```

安装 `build/linux-x64/welnpt-relay`，并使用仓库现有的：

```text
deploy/systemd/welnpt-notap-relay.service
deploy/welnpt-notap.env.example
```

推荐目录和账户：

```text
用户：welnpt
程序：/opt/welnpt-notap/welnpt-relay
配置：/etc/welnpt-notap.env
端口：22333/UDP
```

生成中继密钥，不要使用仓库中的示例值：

```bash
sudo useradd --system --home-dir /opt/welnpt-notap --shell /sbin/nologin welnpt
sudo install -d -m 0755 /opt/welnpt-notap
sudo install -m 0755 welnpt-relay /opt/welnpt-notap/welnpt-relay
sudo sh -c 'umask 077; openssl rand -hex 32 > /etc/welnpt-notap.token'
sudo sh -c 'printf "WEL_NOTAP_PORT=22333\nWEL_NOTAP_TOKEN=%s\n" "$(cat /etc/welnpt-notap.token)" > /etc/welnpt-notap.env'
sudo chmod 0600 /etc/welnpt-notap.env /etc/welnpt-notap.token
sudo install -m 0644 deploy/systemd/welnpt-notap-relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now welnpt-notap-relay
```

Go API 的 `WEL_NOTAP_RELAY_TOKEN` 必须和中继的 `WEL_NOTAP_TOKEN` 完全相同。密钥
不放进客户端，不写入客户端 JSONL 日志，不提交 Git。

### 5.2 中继功能边界

中继负责：

- 客户端注册和 UDP NAT 映射。
- 房间内广播扇出。
- 指定逻辑 IP 的单播转发。
- 中继服务器 Ping 和中继玩家 Ping。
- 直连尚未建立或失败时的比赛回退。

中继不负责：

- 分配 `10.122.x.x`，这由 Go API 负责。
- Laravel 登录，这由 Go 调 Laravel 负责。
- 解析 WE8 载荷中的比赛状态。
- 代替 STUN 做 ICE candidate 收集。

## 6. STUN/ICE 服务部署

### 6.1 “直连服务”到底是什么

当前没有一个替玩家转发比赛数据的独立 P2P 服务器。直连由两台玩家电脑上的
`welnptice.exe`/libjuice 完成，Go API 只负责房间成员鉴权、candidate/SDP 交换和
按比赛对手建立探测，coturn 只负责 STUN。只有直连检查成功后，WE8 单播才从中继
切换到玩家之间的 UDP；失败时继续使用 `welnpt-notap-relay`。

因此上线直连能力至少需要三部分同时存在：

```text
客户端 welnptice.exe + welnpt.dll
Go No-TAP 控制器 /notap/*
STUN coturn 3478/UDP
```

`22333/UDP` 中继仍然是搜索广播和比赛可靠回退所必需的，不能因为部署了 STUN
就关闭。

当前使用 coturn 的 `stun-only` 模式：

```text
配置：deploy/coturn/wel-stun.conf
systemd：deploy/systemd/wel-stun.service
端口：3478/UDP
```

STUN 只用于收集公网 candidate 和 connectivity check。它不是比赛中继，也不能用
`3478` 代替 `22333`。目前没有部署 TURN，直连失败仍依赖 `22333/UDP` 云中继。

启动后检查：

```bash
sudo systemctl is-active wel-stun
sudo ss -lunp | grep ':3478'
sudo tail -n 50 /var/log/coturn/wel-stun.log
```

玩家进入房间时只收集 candidate；玩家点击详情 Ping 或游戏真正加入时，客户端才
针对具体对手执行检查。candidate 就绪不等于直连已建立。

## 7. 防火墙和阿里云安全组

### 7.1 服务器入站

至少放行：

| 端口 | 协议 | 用途 | 来源 |
|---:|---|---|---|
| `80` | TCP | HTTP/证书跳转 | `0.0.0.0/0` |
| `443` | TCP | Laravel/Go API HTTPS | `0.0.0.0/0` |
| `22333` | UDP | 无网卡云中继 | `0.0.0.0/0` |
| `3478` | UDP | STUN | `0.0.0.0/0` |

只在同一台机器承载 TAP 版本时额外保留原有 `22222` 规则。`3306`、`6379`、Go
内部管理端口不应对公网开放。

### 7.2 玩家电脑

无网卡版本不创建虚拟网卡，也不依赖玩家电脑的固定入站 `5739`。游戏 Socket 被
Hook 到本地用户态队列，relay 使用玩家主动发出的 UDP 映射。严格防火墙仍可能让
ICE 直连失败，但只要玩家能访问 `22333/UDP`，比赛应回退到中继。

客户端防火墙规则只绑定 `welnptice.exe` 的 UDP 入站。该进程负责 STUN、ICE
connectivity check 和 P2P 游戏数据；`WE8.exe` 的 Socket 已由 Hook 接入用户态队列，
不需要额外创建 `WE8.exe` 公网入站规则，也不需要单独创建出站规则。规则已经存在且
绑定同一完整程序路径时，客户端不修改、不弹窗；规则缺失才先静默写入，失败后再请求
一次管理员授权。已有同路径阻止规则必须删除或禁用，因为阻止规则优先于允许规则。

## 8. 客户端发布和配置

Windows 客户端使用完整安装包或绿色包，不能只复制主 EXE。至少要保留：

```text
WEL 无网卡客户端主程序
resources/welhelper/welnptgame.exe
resources/welhelper/welnpt.dll
resources/welhelper/welnptice.exe
resources/wel-no-tap.env
```

绿色版可以在主程序旁放置 `wel-no-tap.env`，覆盖打包默认值：

```env
WEL_PLATFORM_NAME=WEL对战平台
WEL_PLATFORM_SHORT_NAME=WEL
WEL_GAME_NAME=WE8
WEL_API_BASE_URL=https://api.example.com:8082/api/v1
```

服务器地址、平台显示名和接口路径由客户端 `config` 读取，修改 env 后重启客户
端即可，不需要重新编译客户端。

客户端版本要和服务端协议匹配。当前 P3 版本包含：

- 按真实比赛对手锁定 ICE，而不是选房间第一个成员。
- Ping 使用临时 pair-specific ICE，不污染比赛通道。
- 直连成功后显示 `当前联机：P2P 直连`，失败时保留 `云中继`。
- 非管理员启动 WE8 的 UAC 流程不会等待游戏退出；当前版本是 `v0.0.40`。

## 9. 上线顺序

推荐按以下顺序部署和验证：

1. 备份 Laravel 数据库和 Go `pes8_platform` 数据库。
2. 确认 Laravel API、PHP-FPM、Nginx、MySQL 正常。
3. 启动 Redis。
4. 配置并启动 Go API，等待迁移完成。
5. 确认 Go `/healthz` 返回成功，检查 `no_tap_rooms` 和迁移记录。
6. 编译并启动 `welnpt-notap-relay`，确认 `22333/UDP` 监听。
7. 启动 `wel-stun`，确认 `3478/UDP` 监听。
8. 配置阿里云安全组和服务器防火墙。
9. 修改客户端 `wel-no-tap.env`，先用两台电脑进入同一个 No-TAP 房间。
10. 验证登录、IP 分配、搜索、加入、10 分钟比赛、直连状态和中继回退。
11. 再开放更多玩家，观察 relay 包数、鉴权失败、路由失败和数据库租约数。

## 10. 验收命令

服务端：

```bash
systemctl is-active nginx php-fpm redis welnpt-notap-relay wel-stun
ss -lntup | grep -E ':80|:443|:8080|:8082|:22333|:3478'
curl -fsS https://api.example.com/healthz
journalctl -u welnpt-notap-relay -n 100 --no-pager
journalctl -u wel-stun -n 100 --no-pager
```

数据库：

```sql
SELECT code, name, subnet_cidr, status FROM no_tap_rooms ORDER BY id;
SELECT room_id, COUNT(*) FROM no_tap_room_leases
WHERE released_at IS NULL GROUP BY room_id;
SELECT version, applied_at FROM platform_schema_migrations ORDER BY applied_at;
```

客户端：

```text
%LOCALAPPDATA%\\WELPlatform\\logs\\room-session-*.jsonl
```

必须同时收集主机和客机日志。有效比赛中应看到 `transport-recv` 或 `sendto` 的
`path:"direct"`；只有 `direct-state: completed` 但没有实际 direct 单播时，不能
对外宣称比赛已经直连。

## 11. 回滚与故障隔离

无网卡服务回滚只操作：

```bash
sudo systemctl disable --now welnpt-notap-relay
sudo systemctl disable --now wel-stun
```

然后从 Nginx/安全组移除 `22333/UDP`、`3478/UDP`。不要删除 `no_tap_*` 表，以便
保留诊断和租约数据；需要回滚数据库时先备份后执行人工确认。

禁止为了修复无网卡问题执行以下操作：

- 重启或删除现有 TAP/n2n supernode。
- 改动 `10.222.x.x` 房间租约。
- 把无网卡客户端改指向 TAP 房间接口。
- 把 STUN 端口当作中继端口。
- 把 relay token 写进绿色版客户端或前端 env。

## 12. 常见上线故障

| 现象 | 优先检查 |
|---|---|
| 登录失败 | Laravel `platform-login`、平台期限、`SOCCER_AUTH_URL` |
| 房间列表为空 | Go API JWT、MySQL、`no_tap_rooms`、客户端 API URL |
| 进入房间报中继未配置 | Go 的 relay host/port/token 与中继 env |
| 搜索不到但能进房间 | 客户端到 `22333/UDP`、中继 `route_drops`、双方逻辑 IP |
| 候选超时 | `3478/UDP`、STUN 服务、玩家本机安全软件；比赛仍应走中继 |
| 直连显示但实际中继 | 是否有真实 `path:"direct"` 单播；不能只看 candidate |
| 多人房间直连错人 | 是否按对手逻辑 IP 与加入 Socket 端口建立会话；`64/84` 仅作候选信号 |
| UAC 后房间按钮一直转 | 客户端必须是 `v0.0.23` 或更高，不能使用旧 `-Wait` 逻辑 |
| 最后出现 `direct-state: failed` | 检查玩家是否已经退出；结合最后一条 direct 数据包判断 |
