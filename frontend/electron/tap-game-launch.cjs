const fs = require('node:fs')
const path = require('node:path')
const { spawn } = require('node:child_process')
const { formatProcessExitCode } = require('./tap-network.cjs')

const GAME_LAUNCH_TIMEOUT_MS = 15000

function ipv4ToNumber(value) {
  const octets = String(value || '').trim().split('.').map(Number)
  if (octets.length !== 4 || octets.some((octet) => !Number.isInteger(octet) || octet < 0 || octet > 255)) return null
  return octets.reduce((number, octet) => ((number << 8) | octet) >>> 0, 0)
}

function numberToIPv4(value) {
  return [24, 16, 8, 0].map((shift) => (value >>> shift) & 255).join('.')
}

function broadcastAddressFromCidr(cidr) {
  const [networkText, prefixText] = String(cidr || '').trim().split('/')
  const network = ipv4ToNumber(networkText)
  const prefix = Number(prefixText)
  if (network === null || !Number.isInteger(prefix) || prefix < 1 || prefix > 30) {
    throw new Error('WEL 房间子网无法生成游戏广播地址')
  }
  const mask = (0xffffffff << (32 - prefix)) >>> 0
  return numberToIPv4(((network & mask) | (~mask >>> 0)) >>> 0)
}

function subnetMaskFromCidr(cidr) {
  const prefix = Number(String(cidr || '').trim().split('/')[1])
  if (!Number.isInteger(prefix) || prefix < 1 || prefix > 30) {
    throw new Error('WEL 房间子网无法生成游戏子网掩码')
  }
  return numberToIPv4((0xffffffff << (32 - prefix)) >>> 0)
}

function gameRuntimeCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'game-runtime'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'game-runtime'),
  ].filter(Boolean)
}

function locateGameRuntime() {
  for (const directory of gameRuntimeCandidates()) {
    const launcher = path.join(directory, 'welgame.exe')
    const hook = path.join(directory, 'welhook.dll')
    if (fs.existsSync(launcher) && fs.existsSync(hook)) return { launcher, hook }
  }
  return null
}

function normalizeNetwork(network) {
  const tapIP = String(network?.actualIp || '').trim()
  const reportedInterfaceIndex = Number(network?.interfaceIndex)
  const interfaceIndex = Number.isInteger(reportedInterfaceIndex) && reportedInterfaceIndex > 0
    ? reportedInterfaceIndex
    : 0
  const subnetCidr = String(network?.subnetCidr || '').trim()
  if (ipv4ToNumber(tapIP) === null) {
    throw new Error('当前房间的网卡信息不完整，请重新进入房间')
  }
  return {
    tapIP,
    interfaceIndex,
    subnetCidr,
    broadcastIP: broadcastAddressFromCidr(subnetCidr),
    subnetMask: subnetMaskFromCidr(subnetCidr),
  }
}

function launchGameBound(gamePath, network) {
  if (process.platform !== 'win32') throw new Error('WE8 Socket 绑定仅支持 Windows')
  const runtime = locateGameRuntime()
  if (!runtime) throw new Error('未找到 WEL 游戏网络组件，请安装最新完整客户端')
  const target = normalizeNetwork(network)
  const warnings = target.interfaceIndex > 0
    ? []
    : ['未能确认 TAP 接口索引；广播改写已启用，但 WE8 UDP Socket 和房间单播只能依赖 Windows 路由。']

  return new Promise((resolve, reject) => {
    const child = spawn(runtime.launcher, [
      '--game', gamePath,
      '--hook', runtime.hook,
      '--tap-ip', target.tapIP,
      '--broadcast-ip', target.broadcastIP,
      '--subnet-mask', target.subnetMask,
      '--interface-index', String(target.interfaceIndex),
    ], { cwd: path.dirname(gamePath), windowsHide: true })
    const output = []
    const timer = setTimeout(() => {
      try { child.kill() } catch {}
      reject(new Error('启动 WE8 Socket 绑定组件超时'))
    }, GAME_LAUNCH_TIMEOUT_MS)
    child.stdout.on('data', (chunk) => output.push(chunk))
    child.stderr.on('data', (chunk) => output.push(chunk))
    child.once('error', (error) => {
      clearTimeout(timer)
      reject(new Error(`WE8 Socket 绑定组件无法启动：${error.message}`))
    })
    child.once('close', (code) => {
      clearTimeout(timer)
      const detail = Buffer.concat(output).toString('utf8').trim()
      if (code === 0 && /STARTED\s+pid=\d+/i.test(detail)) resolve({ detail, warnings })
      else reject(new Error(`WE8 Socket 绑定失败（代码 ${formatProcessExitCode(code)}）${detail ? `：${detail}` : ''}`))
    })
  })
}

module.exports = {
  GAME_LAUNCH_TIMEOUT_MS,
  broadcastAddressFromCidr,
  gameRuntimeCandidates,
  launchGameBound,
  locateGameRuntime,
  normalizeNetwork,
  subnetMaskFromCidr,
}
