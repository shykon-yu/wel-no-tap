const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { spawn } = require('node:child_process')

const appData = path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'WELPlatform')
const logDirectory = path.join(appData, 'logs')

let lastProcess = null

function helperCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'welnptgame.exe'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'welnptgame.exe'),
    path.join(__dirname, '..', 'build', 'welnptgame.exe'),
  ].filter(Boolean)
}

function hookCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'welnpt.dll'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'welnpt.dll'),
    path.join(__dirname, '..', 'build', 'welnpt.dll'),
  ].filter(Boolean)
}

function locate(candidates) {
  return candidates.find((candidate) => fs.existsSync(candidate)) || null
}

function ensureLogPath() {
  fs.mkdirSync(logDirectory, { recursive: true })
  const now = new Date()
  const stamp = now.toISOString().replace(/[-:TZ.]/g, '').slice(0, 14)
  return path.join(logDirectory, 'room-session-' + stamp + '.jsonl')
}

function status() {
  const helper = locate(helperCandidates())
  const hook = locate(hookCandidates())
  return {
    ready: Boolean(helper && hook),
    connected: Boolean(lastProcess && !lastProcess.killed),
    message: helper && hook ? '无网卡联机组件已准备' : '缺少无网卡联机组件，请重新解压完整客户端',
    helper,
    hook,
  }
}

function resolveGamePath(gamePath) {
  const normalized = path.normalize(String(gamePath || '').trim().replace(/^"(.*)"$/, '$1'))
  if (!normalized || !fs.existsSync(normalized) || !fs.statSync(normalized).isFile()) {
    throw new Error('找不到 WE8 游戏程序')
  }
  if (path.extname(normalized).toLowerCase() !== '.exe') throw new Error('选择的游戏路径不是 EXE 文件')
  return normalized
}

function launch({ gamePath, relay, room, logicalIp, token }) {
  const helper = locate(helperCandidates())
  const hook = locate(hookCandidates())
  const executable = resolveGamePath(gamePath)
  if (!helper || !hook) throw new Error('缺少 welnptgame.exe 或 welnpt.dll，请重新解压完整客户端')
  if (!relay || !room || !logicalIp || !token) throw new Error('房间连接凭据不完整，请退出房间后重新进入')

  const logPath = ensureLogPath()
  const environment = {
    ...process.env,
    WEL_NOTAP_RELAY: String(relay),
    WEL_NOTAP_ROOM: String(room),
    WEL_NOTAP_LOGICAL_IP: String(logicalIp),
    WEL_NOTAP_TOKEN: String(token),
    WEL_NOTAP_LOG_PATH: logPath,
  }
  const child = spawn(helper, ['--game', executable, '--hook', hook], {
    cwd: path.dirname(executable),
    env: environment,
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  lastProcess = child
  const output = []
  child.stdout.on('data', (chunk) => output.push(chunk.toString('utf8')))
  child.stderr.on('data', (chunk) => output.push(chunk.toString('utf8')))
  return new Promise((resolve, reject) => {
    child.once('error', reject)
    child.once('close', (code) => {
      lastProcess = null
      const detail = output.join('').trim()
      if (code !== 0) reject(new Error(detail || '无网卡联机启动失败（代码 ' + (code ?? '未知') + '）'))
      else resolve({ started: true, detail: detail || 'WE8 已启动，日志：' + logPath, warnings: [] })
    })
  })
}

async function disconnect() {
  if (lastProcess && !lastProcess.killed) {
    try { lastProcess.kill() } catch {}
  }
  lastProcess = null
  return { stopped: true }
}

function pingHost(host) {
  return {
    host: String(host || ''),
    reachable: false,
    summary: '无网卡逻辑 IP 不提供系统 ICMP Ping',
  }
}

module.exports = { disconnect, launch, pingHost, status }
