# P2 云中继旁路部署

完整的无网卡平台上线组件、Laravel/Go/Relay/STUN 依赖和验收顺序见：
[`docs/NO_TAP_DEPLOYMENT_ZH.md`](../docs/NO_TAP_DEPLOYMENT_ZH.md)。

无网卡中继与现有 TAP/n2n 平台完全隔离：

| 服务 | 端口 | 管理方式 |
|---|---:|---|
| 现有 n2n supernode | `22222/UDP+TCP` | `weln2n-supernode.service` |
| 现有平台 API/Nginx | `8082/80/443 TCP` | Docker/Nginx |
| P2 无网卡中继 | `22333/UDP` | `welnpt-notap-relay.service` |
| P2 ICE STUN | `3478/UDP` | `wel-stun.service` |

部署 P2 时禁止重启 Docker、Nginx、OpenVPN 或 `weln2n-supernode`。

## 安装

```bash
sudo useradd --system --home-dir /opt/welnpt-notap --shell /sbin/nologin welnpt
sudo install -d -o root -g root -m 0755 /opt/welnpt-notap
sudo install -m 0755 welnpt-relay /opt/welnpt-notap/welnpt-relay
sudo install -m 0644 deploy/systemd/welnpt-notap-relay.service \
  /etc/systemd/system/welnpt-notap-relay.service
sudo sh -c 'umask 077; openssl rand -hex 24 > /etc/welnpt-notap.token'
sudo sh -c 'printf "WEL_NOTAP_PORT=22333\nWEL_NOTAP_TOKEN=%s\n" \
  "$(cat /etc/welnpt-notap.token)" > /etc/welnpt-notap.env'
sudo chmod 0600 /etc/welnpt-notap.env /etc/welnpt-notap.token
sudo systemctl daemon-reload
sudo systemctl enable --now welnpt-notap-relay
```

Go API 同时配置同一公网中继地址和同一密钥：

```env
WEL_NOTAP_RELAY_HOST=8.155.145.132
WEL_NOTAP_RELAY_PORT=22333
WEL_NOTAP_RELAY_TOKEN=<与 WEL_NOTAP_TOKEN 相同的值>
```

这些变量只属于 Go API 的 No-TAP 控制器，不替换现有 TAP/n2n 房间配置。No-TAP
使用独立的 `no_tap_rooms`、`no_tap_room_leases` 和 `10.122.1.0/24` 至
`10.122.3.0/24`；TAP 客户端继续使用原有表和 `10.222.x.x`。

只开放新端口：

```bash
sudo firewall-cmd --zone=public --permanent --add-port=22333/udp
sudo firewall-cmd --zone=public --add-port=22333/udp
```

阿里云安全组还需要单独增加 `UDP 22333` 入方向规则。

## 自建 STUN

libjuice 使用 STUN 收集公网 candidate 和协助 NAT 打洞。STUN 只参与探测，比赛
数据在 ICE 成功后直接走两台玩家之间；直连失败仍由 `22333/UDP` 云中继兜底。

CentOS 7 可使用 EPEL 的 coturn：

```bash
sudo yum install -y coturn
sudo install -m 0644 deploy/coturn/wel-stun.conf /etc/coturn/wel-stun.conf
sudo install -m 0644 deploy/systemd/wel-stun.service /etc/systemd/system/wel-stun.service
sudo systemctl daemon-reload
sudo systemctl enable --now wel-stun
sudo firewall-cmd --zone=public --permanent --add-port=3478/udp
sudo firewall-cmd --reload
```

Go API 环境变量必须指向同一台公网服务器：

```env
WEL_NOTAP_ICE_STUN_HOST=8.155.145.132
WEL_NOTAP_ICE_STUN_PORT=3478
```

阿里云安全组增加 `UDP 3478` 入方向规则，来源可以先设为 `0.0.0.0/0`。仅运行
`stun-only` 时不需要开放 TURN 中继端口段。

## 验证

```bash
systemctl is-active welnpt-notap-relay
systemctl is-active wel-stun
systemctl is-active weln2n-supernode
ss -lunp | grep -E '22222|22333|3478'
curl -fsS http://127.0.0.1:8082/healthz
journalctl -u welnpt-notap-relay -n 50 --no-pager
```

中继每 60 秒输出活动玩家、收发包、鉴权失败、畸形包和无路由包计数。

## 回滚

```bash
sudo systemctl disable --now welnpt-notap-relay
sudo firewall-cmd --zone=public --permanent --remove-port=22333/udp
sudo firewall-cmd --zone=public --remove-port=22333/udp
```

回滚不需要也不允许操作现有 TAP/n2n 服务。
