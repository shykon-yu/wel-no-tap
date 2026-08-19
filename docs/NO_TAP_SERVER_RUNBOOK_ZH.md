# WEL 无网卡版换服务器部署运行手册

> 适用范围：`wel-no-tap` P3（ICE 直连优先，云中继回退）
>
> 用途：新服务器上线、迁移服务器或灾难恢复时按本文执行。
>
> 最后更新：2026-08-19，客户端基线：`v0.0.44`

本文是操作手册，不是架构设计。开始前先阅读
[`NO_TAP_DEPLOYMENT_ZH.md`](NO_TAP_DEPLOYMENT_ZH.md) 了解组件边界。

## 1. 必须部署的组件

无网卡版不是只部署一个 UDP 程序；以下组件必须完整可用。

| 组件 | 来源/管理方式 | 是否必需 | 作用 | 对外端口 |
|---|---|---:|---|---|
| Laravel 账号服务 | 现有 `soccer_php` 部署 | 是 | 登录、账号状态和平台期限校验 | `443/TCP` |
| Go 平台 API | `platform/backend`，现有 Docker 部署 | 是 | JWT、房间、逻辑 IP 租约、SDP/ICE 交换、下发 relay/STUN 地址 | 建议经 `443/TCP`，现网可为 `8082/TCP` |
| MySQL | 现有平台基础设施 | 是 | `platform_*` 和 `no_tap_*` 数据 | 仅内网/容器网络 |
| Redis | 现有平台基础设施 | 是 | Go API 会话与在线状态 | 仅内网/容器网络 |
| `welnpt-notap-relay` | 本仓库，systemd | 是 | 搜索、加入、直连未成功时的游戏数据中继 | `22333/UDP` |
| coturn（STUN-only） | 本仓库配置，systemd | 是 | 给 ICE 收集公网 `srflx` candidate | `3478/UDP` |
| Nginx/TLS | 现有 Web 入口 | 推荐 | HTTPS、域名和 API 反向代理 | `80/443 TCP` |

`welnptice.exe`、`welnpt.dll` 在玩家电脑上运行；服务器不运行 ICE agent。STUN
只协助打洞，不能代替中继。没有 TURN：无法打洞的玩家必须能访问 `22333/UDP` 回退。

如果新服务器还承载旧 TAP/n2n 平台，旧服务可继续运行，但必须保持独立：

| 旧服务 | 无网卡版的要求 |
|---|---|
| n2n / TAP supernode，通常 `22222/UDP` | 不重启、不改端口、不复用配置 |
| TAP 房间和 `10.222.x.x` 租约 | 不修改、不复用数据库表 |
| OpenVPN / SoftEther | 不依赖、不作为无网卡流量路径 |

无网卡逻辑网段为 `10.122.1.0/24` 至 `10.122.4.0/24`。它们是应用层逻辑地址，
不是服务器网卡地址，也不应在服务器上创建路由或虚拟网卡。

## 2. 迁移前记录与备份

在旧服务器仍可用时完成以下事项。没有这些资料不要切换 DNS 或客户端配置。

### 2.1 填写部署记录

将下面内容保存在密码库或受控的运维文件中，禁止提交 Git：

```text
新服务器公网 IP：
API 对外域名/URL：
Laravel 登录接口 URL：
No-TAP relay 地址：<域名或公网 IP>:22333
STUN 地址：<域名或公网 IP>:3478

Go MySQL DSN：
Go Redis 地址与密码：
Laravel SOCCER_AUTH_URL：
Go JWT_SECRET：
No-TAP WEL_NOTAP_RELAY_TOKEN：
Laravel APP_KEY、数据库和 Redis 参数：
TLS 证书及私钥来源：
现有 Nginx 配置位置：
```

`WEL_NOTAP_RELAY_TOKEN` 必须同时写入 Go API 和 relay 的 systemd 环境文件；两边任一
字符不同都会导致进房间后无法通信。该密钥不得写进客户端，也不得写入日志。

### 2.2 备份

至少备份以下数据，并在新服务器恢复后抽样验证：

```bash
# 在数据库所在位置执行；库名和账号按实际替换。
mysqldump --single-transaction --routines --triggers -u <user> -p <platform_database> \
  > platform-$(date +%F).sql
mysqldump --single-transaction --routines --triggers -u <user> -p <laravel_database> \
  > laravel-$(date +%F).sql
```

同时备份：Laravel `.env`、Go API 生产环境文件/Compose 配置、Nginx 配置与证书、
relay `/etc/welnpt-notap.env`，以及当前客户端 `wel-no-tap.env`。不要只备份
`no_tap_*` 表，因为账号、平台权限和 Go API 的其他表也需要保留。

## 3. 新服务器基础准备

建议使用受支持的 Linux 发行版和一个固定公网 IPv4。安装 Docker/Compose、Git、
OpenSSL、编译 relay 所需的 GCC/OpenSSL 开发包和 coturn。包管理器依发行版不同：

```bash
# Rocky / AlmaLinux / Alibaba Cloud Linux 等 dnf 系统
sudo dnf install -y git gcc openssl-devel openssl coturn

# CentOS 7 等 yum 系统（coturn 可能需要先启用 EPEL）
sudo yum install -y git gcc openssl-devel openssl coturn

# Ubuntu / Debian
sudo apt-get update
sudo apt-get install -y git build-essential libssl-dev openssl coturn
```

安装 Docker Engine 与 Docker Compose plugin 后，确认：

```bash
docker --version
docker compose version
systemctl is-active docker
```

数据库、Redis、Nginx 和业务容器必须使用持久化卷。不要将 `3306`、`6379` 暴露到公网；
若现有 Compose 已映射这些端口，限制到 `127.0.0.1` 或内网安全组。

## 4. 防火墙与云安全组

先在云厂商安全组放行，再配置系统防火墙。无网卡版最低入站规则：

| 端口 | 协议 | 来源 | 用途 |
|---:|---|---|---|
| `80` | TCP | 公网 | HTTP 到 HTTPS 跳转/证书申请 |
| `443` | TCP | 公网 | Laravel、Go API 的 HTTPS |
| `22333` | UDP | 公网 | 无网卡中继，必需 |
| `3478` | UDP | 公网 | STUN，必需 |

只有仍保留旧 TAP/n2n 服务时才额外保留其原端口，例如 `22222/UDP`。绝不开放 MySQL、
Redis 或 Docker daemon 到公网。

firewalld 示例：

```bash
sudo firewall-cmd --permanent --add-service=http
sudo firewall-cmd --permanent --add-service=https
sudo firewall-cmd --permanent --add-port=22333/udp
sudo firewall-cmd --permanent --add-port=3478/udp
sudo firewall-cmd --reload
```

UFW 示例：

```bash
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow 22333/udp
sudo ufw allow 3478/udp
```

## 5. 恢复 Web、Laravel、Go、MySQL 与 Redis

这几个服务由现有 `soccer_php` 和 `platform` 项目管理，不由 `wel-no-tap` 仓库自动
创建生产环境。应恢复现有生产 Compose/镜像和环境变量，再增加下列 No-TAP 配置。
不要把各仓库的开发 `docker-compose.yml` 原样用于生产：其中含开发密码、调试设置和
数据库端口映射。

### 5.1 启动顺序

1. 恢复 MySQL 数据与持久化卷。
2. 启动 Redis。
3. 启动 Laravel，完成迁移并确认登录接口正常。
4. 启动 Go API；它会执行自己的平台迁移。
5. 启动 Nginx/TLS，确认外部 API URL 正常。

Go API 的生产环境至少要有：

```env
API_PORT=8080
MYSQL_DSN=<platform MySQL DSN>
REDIS_ADDR=<redis host>:6379
REDIS_PASSWORD=<redis password>
JWT_SECRET=<long random secret>
JWT_AUDIENCE=wel-no-tap
CORS_ORIGIN=https://<platform web domain>
SOCCER_AUTH_URL=http://<laravel internal endpoint>/api/v1/auth/platform-login

WEL_NOTAP_RELAY_HOST=<new public IP or DNS name>
WEL_NOTAP_RELAY_PORT=22333
WEL_NOTAP_RELAY_TOKEN=<same relay token>
WEL_NOTAP_ICE_STUN_HOST=<new public IP or DNS name>
WEL_NOTAP_ICE_STUN_PORT=3478
```

`SOCCER_AUTH_URL` 是 Go 容器可访问的 Laravel 内部地址；它不是玩家客户端 URL。
`WEL_NOTAP_RELAY_HOST` 与 `WEL_NOTAP_ICE_STUN_HOST` 是下发给玩家电脑的公网可达地址，
不能填写 Docker 容器名、`127.0.0.1` 或内网地址。

确认 Go API 已创建无网卡房间：

```sql
SELECT code, name, connection_mode, subnet_cidr, status
FROM no_tap_rooms ORDER BY id;

SELECT version, applied_at
FROM platform_schema_migrations ORDER BY applied_at;
```

应能看到 `notap-01`、`notap-02`（直连）和 `notap-03`、`notap-04`（中继）。

### 5.2 API 与反向代理验收

在服务器上：

```bash
curl -fsS http://127.0.0.1:<go-api-port>/healthz
```

在外部网络：

```bash
curl -fsS https://<api-domain>/api/v1/healthz
```

若现网仍使用 `http://<IP>:8082/api/v1`，客户端可以继续使用该地址；迁移时保持 URL
不变最稳妥。若改为 HTTPS 域名，必须同时更新客户端配置，且 CORS 与 Nginx 路径必须
匹配 `/api/v1`。

## 6. 部署无网卡 Relay

在构建机或新服务器获取 `wel-no-tap` 的指定提交，构建 Linux relay：

```bash
git clone https://github.com/shykon-yu/wel-no-tap.git /opt/src/wel-no-tap
cd /opt/src/wel-no-tap
git checkout <approved-commit-or-tag>
./scripts/build-linux-relay.sh
```

安装 systemd 服务。以下命令是可重复执行的；替换 `<relay-token>`，不要把真实值放进
shell 历史或文档：

```bash
sudo useradd --system --home-dir /opt/welnpt-notap --shell /sbin/nologin welnpt 2>/dev/null || true
sudo install -d -o root -g root -m 0755 /opt/welnpt-notap
sudo install -m 0755 build/linux-x64/welnpt-relay /opt/welnpt-notap/welnpt-relay
sudo install -m 0644 deploy/systemd/welnpt-notap-relay.service /etc/systemd/system/welnpt-notap-relay.service
sudo install -m 0600 /dev/null /etc/welnpt-notap.env
read -rsp '请输入 relay token: ' RELAY_TOKEN; echo
sudo env RELAY_TOKEN="$RELAY_TOKEN" sh -c 'printf "WEL_NOTAP_PORT=22333\nWEL_NOTAP_TOKEN=%s\n" "$RELAY_TOKEN" > /etc/welnpt-notap.env'
unset RELAY_TOKEN
sudo systemctl daemon-reload
sudo systemctl enable --now welnpt-notap-relay
```

验证：

```bash
sudo systemctl status welnpt-notap-relay --no-pager
sudo ss -lunp | grep ':22333'
sudo journalctl -u welnpt-notap-relay -n 100 --no-pager
```

期望日志包含 `listening=0.0.0.0:22333/udp`。relay 每 60 秒输出活动玩家与收发包、
鉴权失败、畸形包和无路由包计数。

## 7. 部署 STUN

安装本仓库的 STUN-only coturn 配置。此配置不启用 TURN，因此只需要 `3478/UDP`，不需要
开放 TURN relay 端口段。

```bash
sudo install -d -o coturn -g coturn -m 0755 /var/log/coturn
sudo install -m 0644 deploy/coturn/wel-stun.conf /etc/coturn/wel-stun.conf
sudo install -m 0644 deploy/systemd/wel-stun.service /etc/systemd/system/wel-stun.service
sudo systemctl daemon-reload
sudo systemctl enable --now wel-stun
sudo systemctl status wel-stun --no-pager
sudo ss -lunp | grep ':3478'
sudo tail -n 100 /var/log/coturn/wel-stun.log
```

`3478/UDP` 可用只说明玩家能收集公网 candidate；不代表任意两名玩家都能 P2P 直连。
对称 NAT、CGNAT 或严格防火墙组合仍会由 relay 回退。

## 8. 更新客户端配置与发布

客户端只应改 API 基址；relay 和 STUN 地址由 Go API 的 room-session 动态下发。

在 Windows 客户端主程序旁创建或更新 `wel-no-tap.env`：

```env
WEL_API_BASE_URL=https://<api-domain>/api/v1
WEL_API_LOGIN_PATH=/auth/login
WEL_API_ROOM_SESSION_PATH=/notap/me/room-session
WEL_API_ROOMS_PATH=/notap/rooms
WEL_API_ROOM_ICE_PATH=/notap/rooms/{roomId}/ice
WEL_API_ROOM_PEER_PROBES_PATH=/notap/rooms/{roomId}/peer-probes
```

其余路径保留 [`frontend/wel-no-tap.env.example`](../frontend/wel-no-tap.env.example)
的默认值。不要在客户端 env 中写 relay token。

发布包必须包含：主程序、`welnptgame.exe`、`welnpt.dll`、`welnptice.exe` 和
`wel-no-tap.env`。仅替换主 EXE 会导致无网卡组件版本不匹配。

## 9. 上线验收

按顺序执行，任一步失败不得切流：

1. 服务端确认 `443/TCP`、`22333/UDP`、`3478/UDP` 对公网可达。
2. 两个不同网络、不同账号的客户端能登录并进入同一个 `notap-01` 房间。
3. 两端均能看到不同的 `10.122.*` 逻辑 IP，且成员列表与心跳正常。
4. 主机搜索、客机加入、选队和开赛均正常。
5. 检查双方 `%LOCALAPPDATA%\WELPlatform\logs\room-session-*.jsonl`：
   - 直连成功：`ice-decision result:"direct"`，随后有真实比赛单播 `path:"direct"`。
   - 直连失败：`ice-decision reason:"ice-failed"` 或 `"decision-timeout"`，比赛仍有
     `path:"relay"` 的持续双向数据。
6. 至少完成一场 10 分钟比赛，检查没有持续队列丢包、频繁切换或服务端 route drop 异常。
7. 使用一名已知无法直连的网络复测，确认会立即可靠回退中继而不是无法联机。

日常检查命令：

```bash
sudo systemctl is-active welnpt-notap-relay wel-stun
sudo ss -lunp | grep -E ':22333|:3478'
sudo journalctl -u welnpt-notap-relay -n 100 --no-pager
sudo journalctl -u wel-stun -n 100 --no-pager
```

## 10. 切换与回滚

### 10.1 推荐切换顺序

1. 在新服务器完整完成第 1 至 9 节，不改变旧服务器。
2. 用测试客户端把 API、relay、STUN 都指向新服务器完成双机验证。
3. 降低 DNS TTL，修改 API 域名或发布新的客户端 `wel-no-tap.env`。
4. 观察 relay、STUN、Go API 和数据库至少一个高峰周期。
5. 旧服务器保留运行和数据库备份，确认稳定后再按变更流程下线。

### 10.2 仅回滚无网卡 Relay/STUN

```bash
sudo systemctl disable --now welnpt-notap-relay wel-stun
```

然后将 Go API 的 `WEL_NOTAP_RELAY_*`、`WEL_NOTAP_ICE_STUN_*` 和客户端 API 地址恢复为
旧服务器值。不要删除 `no_tap_*` 表，也不要因为无网卡故障重启或修改旧 TAP/n2n 服务。

## 11. 故障定位速查

| 现象 | 首先检查 |
|---|---|
| 登录失败 | Laravel `platform-login`、`SOCCER_AUTH_URL`、账号状态/平台期限 |
| 房间为空或进房失败 | Go `/healthz`、JWT、`no_tap_rooms`、MySQL 迁移、客户端 API URL |
| 能进房但搜索不到 | `22333/UDP` 安全组/防火墙、relay token、relay 日志 `route_drops` |
| 没有 `srflx` candidate | `3478/UDP`、coturn 服务、STUN 公网地址和客户端本机安全软件 |
| P2P 失败但中继正常 | NAT/运营商限制是常见原因；查看 `ice-decision`，中继是设计内回退 |
| 显示 P2P 但怀疑实际走中继 | 双方日志必须出现实际比赛单播 `path:"direct"`，不能只看 ICE state |
| 所有玩家都不能联机 | relay 没监听、token 不一致、Go API 下发错误公网地址或 UDP 安全组未放行 |
