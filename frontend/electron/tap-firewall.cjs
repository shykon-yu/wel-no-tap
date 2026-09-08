const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { runProcess } = require('./tap-network.cjs')

const EDGE_INBOUND_RULE = 'WEL n2n edge inbound'
const WE8_INBOUND_RULE = 'WEL WE8 inbound'
const ROOM_UDP_INBOUND_RULE = 'WEL room UDP inbound'
const ROOM_UDP_OUTBOUND_RULE = 'WEL room UDP outbound'
const ROOM_ICMP_INBOUND_RULE = 'WEL room ICMPv4 inbound'
const ROOM_ICMP_OUTBOUND_RULE = 'WEL room ICMPv4 outbound'
const WEL_ROOM_FIREWALL_SUBNET_CIDR = '10.222.0.0/16'
const FIREWALL_RULE_VERSION = 8
const FIREWALL_WARNING_RETRY_COOLDOWN_MS = 30000
const ROOM_WARNING_BASE = 40
const ROOM_UDP_WARNING = 1
const ROOM_EDGE_WARNING = 2
const ROOM_ICMP_WARNING = 4
const WE8_BLOCK_REMAINS = 31
const WE8_ALLOW_WARNING = 32

let activeRoomFirewallKey = null
let activeRoomFirewallResult = null
let activeRoomFirewallAttemptAt = 0

function firewallHelperCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'welfirewall.exe'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'welfirewall.exe'),
  ].filter(Boolean)
}

function locateFirewallHelper() {
  return firewallHelperCandidates().find((candidate) => fs.existsSync(candidate)) || null
}

function firewallHelperExitReason(code) {
  if (Number(code) === 10) return '用户取消了 Windows 防火墙授权'
  if (Number(code) === 11) return 'Windows 无法启动防火墙授权程序'
  if (Number(code) === 12) return 'Windows 防火墙规则写入失败'
  if (Number(code) === 21) return 'Windows 防火墙无法放行房间 UDP 入站流量'
  if (Number(code) === WE8_BLOCK_REMAINS) return '当前 WE8.exe 仍存在有效的入站阻止规则'
  if (Number(code) === WE8_ALLOW_WARNING) return 'Windows 未能创建 WE8.exe 入站允许规则'
  return `Windows 防火墙授权程序退出（代码 ${code ?? '未知'}）`
}

function firewallError(code, message) {
  const error = new Error(message)
  error.firewallCode = Number(code)
  return error
}

function roomFirewallWarnings(error) {
  const code = Number(error?.firewallCode)
  const warnings = []
  if (code >= ROOM_WARNING_BASE && code <= ROOM_WARNING_BASE + 7) {
    const flags = code & 7
    if (flags & ROOM_UDP_WARNING) warnings.push('房间 UDP 入站放行失败；开启防火墙时，可能影响搜索或连接。')
    if (flags & ROOM_EDGE_WARNING) warnings.push('网络组件入站放行失败；可能降低 P2P 直连成功率，但仍会继续尝试中继连接。')
    if (flags & ROOM_ICMP_WARNING) warnings.push('Ping 放行规则创建失败；对手可能无法 Ping 到你，但不影响进入房间。')
    return warnings
  }
  if (code === 21) return ['房间 UDP 入站放行失败；开启防火墙时，可能影响搜索或连接。']
  return [`防火墙规则未完全配置：${String(error?.message || '未知错误')}。仍将继续连接房间。`]
}

function firewallLogPath() {
  return path.join(
    process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'),
    'WELPlatform',
    'logs',
    'firewall.log',
  )
}

function cidrToFirewallSubnet(subnetCidr) {
  const match = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\/(\d|[12]\d|3[0-2])$/.exec(String(subnetCidr || '').trim())
  if (!match) throw new Error('WEL 房间子网格式不正确')

  const octets = match.slice(1, 5).map(Number)
  if (octets.some((octet) => octet > 255)) throw new Error('WEL 房间子网格式不正确')

  const prefixLength = Number(match[5])
  const mask = Array.from({ length: 4 }, (_, index) => {
    const remainingBits = Math.max(0, Math.min(8, prefixLength - index * 8))
    return remainingBits === 0 ? 0 : 256 - (2 ** (8 - remainingBits))
  })
  const network = octets.map((octet, index) => octet & mask[index])
  return `${network.join('.')}/${mask.join('.')}`
}

function buildRoomFirewallArgs(edgePath, subnetCidr = WEL_ROOM_FIREWALL_SUBNET_CIDR) {
  const normalizedEdgePath = String(edgePath || '').trim()
  if (!normalizedEdgePath) throw new Error('网络组件路径为空')
  return ['--subnet', cidrToFirewallSubnet(subnetCidr), '--edge', normalizedEdgePath]
}

function buildWe8FirewallArgs(programPath) {
  const normalizedPath = String(programPath || '').trim()
  if (!normalizedPath) throw new Error('防火墙程序路径为空')
  return ['--game', normalizedPath]
}

async function runFirewallHelper(args) {
  if (process.platform !== 'win32') return false
  const helper = locateFirewallHelper()
  if (!helper) throw new Error('未找到 Windows 防火墙授权组件，请安装最新完整客户端')
  try {
    await runProcess(helper, args, 45000)
  } catch (error) {
    const match = /退出代码\s+(-?\d+)/.exec(String(error?.message || ''))
    if (match) {
      const code = Number(match[1])
      throw firewallError(code, `${firewallHelperExitReason(code)}\n防火墙日志：${firewallLogPath()}`)
    }
    throw new Error(`Windows 防火墙授权失败：${error.message}`)
  }
  return true
}

async function ensureRoomFirewall(edgePath, subnetCidr = WEL_ROOM_FIREWALL_SUBNET_CIDR) {
  const normalizedSubnetCidr = String(subnetCidr || WEL_ROOM_FIREWALL_SUBNET_CIDR).trim()
  const key = `${String(edgePath || '').trim().toLowerCase()}|${normalizedSubnetCidr}`
  const hasWarnings = Boolean(activeRoomFirewallResult?.warnings?.length)
  const warningCacheIsFresh = Date.now() - activeRoomFirewallAttemptAt < FIREWALL_WARNING_RETRY_COOLDOWN_MS
  if (activeRoomFirewallKey === key && activeRoomFirewallResult && (!hasWarnings || warningCacheIsFresh)) {
    return activeRoomFirewallResult
  }
  let result
  try {
    await runFirewallHelper(buildRoomFirewallArgs(edgePath, normalizedSubnetCidr))
    result = { warnings: [] }
  } catch (error) {
    result = { warnings: roomFirewallWarnings(error) }
  }
  activeRoomFirewallKey = key
  activeRoomFirewallResult = result
  activeRoomFirewallAttemptAt = Date.now()
  return result
}

async function ensureEdgeFirewall(programPath) {
  return ensureRoomFirewall(programPath)
}

async function ensureRoomUdpFirewall(subnetCidr, edgePath) {
  return ensureRoomFirewall(edgePath, subnetCidr)
}

async function ensureWe8Firewall(programPath) {
  try {
    await runFirewallHelper(buildWe8FirewallArgs(programPath))
    return { warnings: [] }
  } catch (error) {
    if (Number(error?.firewallCode) === WE8_BLOCK_REMAINS) throw error
    return {
      warnings: [`WE8 防火墙放行未完成：${String(error?.message || '未知错误')}。游戏仍会启动。`],
    }
  }
}

module.exports = {
  EDGE_INBOUND_RULE,
  FIREWALL_RULE_VERSION,
  ROOM_ICMP_INBOUND_RULE,
  ROOM_ICMP_OUTBOUND_RULE,
  ROOM_UDP_INBOUND_RULE,
  ROOM_UDP_OUTBOUND_RULE,
  WE8_INBOUND_RULE,
  WEL_ROOM_FIREWALL_SUBNET_CIDR,
  buildRoomFirewallArgs,
  buildWe8FirewallArgs,
  cidrToFirewallSubnet,
  ensureEdgeFirewall,
  ensureRoomFirewall,
  ensureRoomUdpFirewall,
  ensureWe8Firewall,
  firewallHelperCandidates,
  firewallHelperExitReason,
  firewallLogPath,
  locateFirewallHelper,
  roomFirewallWarnings,
}
