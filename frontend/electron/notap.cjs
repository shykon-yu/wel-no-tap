const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { spawn } = require('node:child_process')

const appData = path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'WELPlatform')
const logDirectory = path.join(appData, 'logs')

let lastProcess = null
let iceProcess = null
let iceState = 'waiting'
let iceLocalDescription = ''
let iceAgentPort = 0
let iceHookPort = 0
let iceLineBuffer = ''
let iceSdpBuffer = ''
let readingIceSdp = false
let pendingIcePing = null
let pendingRelayPing = null
let lastRemoteDescription = ''
let lastLogPath = ''

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

function iceCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'welnptice.exe'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'welnptice.exe'),
    path.join(__dirname, '..', 'build', 'welnptice.exe'),
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
  const ice = locate(iceCandidates())
  return {
    ready: Boolean(helper && hook),
    connected: Boolean(lastProcess && !lastProcess.killed),
    message: helper && hook ? '无网卡联机组件已准备' : '缺少无网卡联机组件，请重新解压完整客户端',
    helper,
    hook,
    ice: Boolean(ice),
    directState: iceState,
  }
}

function transportStatus() {
  let pathName = 'relay'
  if (lastLogPath && fs.existsSync(lastLogPath)) {
    try {
      const contents = fs.readFileSync(lastLogPath, 'utf8')
      const lines = contents.trim().split(/\r?\n/).slice(-500).reverse()
      for (const line of lines) {
        if (line.includes('"api":"direct-fallback"') || (line.includes('"api":"direct-state"') && !line.includes('"state":"connected"') && !line.includes('"state":"completed"'))) { pathName = 'relay'; break }
        if (line.includes('"path":"direct"')) { pathName = 'direct'; break }
      }
    } catch {}
  }
  return { path: pathName, directState: iceState, summary: pathName === 'direct' ? '当前比赛路径：P2P 直连' : '当前比赛路径：云中继' }
}

function chooseHookPort() {
  return 40000 + ((process.pid + Date.now()) % 18000)
}

function waitForIceCandidate(child, timeoutMs = 12000) {
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + timeoutMs
    const timer = setInterval(() => {
      if (iceLocalDescription && iceAgentPort && iceProcess === child) {
        clearInterval(timer)
        resolve()
        return
      }
      if (Date.now() >= deadline || iceProcess !== child) {
        clearInterval(timer)
        reject(new Error('ICE candidate 收集超时'))
      }
    }, 100)
  })
}

function handleIceLine(rawLine) {
  const line = rawLine.replace(/[\r\n]+$/, '')
  if (readingIceSdp) {
    if (line === 'LOCAL_SDP_END') {
      readingIceSdp = false
      iceLocalDescription = iceSdpBuffer
      iceSdpBuffer = ''
    } else {
      iceSdpBuffer += line + '\n'
    }
    return
  }
  if (line === 'LOCAL_SDP_BEGIN') { readingIceSdp = true; iceSdpBuffer = ''; return }
  if (line.startsWith('LOCAL_PORT ')) { iceAgentPort = Number(line.slice(11)) || 0; return }
  if (line.startsWith('STATE ')) { iceState = line.slice(6) || 'unknown'; return }
  if (line.startsWith('PING_RESULT ')) {
    const [, nonce, milliseconds] = line.split(' ')
    if (pendingIcePing && pendingIcePing.nonce === nonce) {
      const pending = pendingIcePing
      pendingIcePing = null
      pending.resolve(Number(milliseconds) || 0)
    }
    return
  }
  if (line.startsWith('PING_UNAVAILABLE ')) {
    if (pendingIcePing) { const pending = pendingIcePing; pendingIcePing = null; pending.reject(new Error('直连探测不可用')); }
    return
  }
  if (line.startsWith('RELAY_PING_RESULT ')) {
    const [, nonce, milliseconds] = line.split(' ')
    if (pendingRelayPing && pendingRelayPing.nonce === nonce) {
      const pending = pendingRelayPing
      pendingRelayPing = null
      pending.resolve(Number(milliseconds) || 0)
    }
    return
  }
  if (line.startsWith('RELAY_PING_UNAVAILABLE ')) {
    if (pendingRelayPing) { const pending = pendingRelayPing; pendingRelayPing = null; pending.reject(new Error('中继探测不可用')); }
  }
}

function startIceAgent({ stunHost, stunPort, relay, room, logicalIp, token }) {
  const executable = locate(iceCandidates())
  if (!executable) throw new Error('缺少 welnptice.exe，请重新解压完整客户端')
  if (iceProcess && !iceProcess.killed) return waitForIceCandidate(iceProcess)
  iceHookPort = chooseHookPort()
  iceLocalDescription = ''
  iceAgentPort = 0
  iceState = 'gathering'
  const environment = { ...process.env }
  if (relay && room && logicalIp && token) {
    environment.WEL_NOTAP_RELAY = String(relay)
    environment.WEL_NOTAP_ROOM = String(room)
    environment.WEL_NOTAP_LOGICAL_IP = String(logicalIp)
    environment.WEL_NOTAP_TOKEN = String(token)
  }
  const child = spawn(executable, ['--stun-host', String(stunHost || ''), '--stun-port', String(stunPort || 0), '--hook-port', String(iceHookPort)], {
    windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: environment,
  })
  iceProcess = child
  const consume = (chunk) => {
    iceLineBuffer += chunk.toString('utf8')
    let newline
    while ((newline = iceLineBuffer.indexOf('\n')) >= 0) {
      const line = iceLineBuffer.slice(0, newline + 1)
      iceLineBuffer = iceLineBuffer.slice(newline + 1)
      handleIceLine(line)
    }
  }
  child.stdout.on('data', consume)
  child.stderr.on('data', (chunk) => { /* the helper reports protocol state on stdout */ void chunk })
  child.once('close', () => { if (iceProcess === child) { iceProcess = null; iceState = 'failed' } })
  return waitForIceCandidate(child)
}

async function prepareIce(options) {
  await startIceAgent(options || {})
  return { localDescription: iceLocalDescription, directState: iceState, agentPort: iceAgentPort, hookPort: iceHookPort }
}

function setRemoteIce(remoteDescription) {
  if (!iceProcess || !remoteDescription) return false
  const normalized = String(remoteDescription).replace(/\r?\n/g, '\n').replace(/\n*$/, '\n')
  if (lastRemoteDescription === normalized) return true
  iceProcess.stdin.write('REMOTE_SDP_BEGIN\n' + normalized + 'REMOTE_SDP_END\n')
  lastRemoteDescription = normalized
  return true
}

function pingIce(remoteDescription) {
  if (!setRemoteIce(remoteDescription)) return Promise.reject(new Error('对方尚未完成 ICE candidate'))
  const nonce = Math.random().toString(36).slice(2, 12)
  return new Promise((resolve, reject) => {
    pendingIcePing = { nonce, resolve, reject }
    iceProcess.stdin.write('PING ' + nonce + '\n')
    setTimeout(() => {
      if (pendingIcePing?.nonce !== nonce) return
      pendingIcePing = null
      reject(new Error('直连探测超时'))
    }, 8000)
  })
}

function pingRelay() {
  if (!iceProcess) return Promise.reject(new Error('中继探测未准备'))
  const nonce = String(Date.now() >>> 0)
  return new Promise((resolve, reject) => {
    pendingRelayPing = { nonce, resolve, reject }
    iceProcess.stdin.write('PING_RELAY ' + nonce + '\n')
    setTimeout(() => {
      if (pendingRelayPing?.nonce !== nonce) return
      pendingRelayPing = null
      reject(new Error('中继探测超时'))
    }, 5000)
  })
}

function resolveGamePath(gamePath) {
  const normalized = path.normalize(String(gamePath || '').trim().replace(/^"(.*)"$/, '$1'))
  if (!normalized || !fs.existsSync(normalized) || !fs.statSync(normalized).isFile()) {
    throw new Error('找不到 WE8 游戏程序')
  }
  if (path.extname(normalized).toLowerCase() !== '.exe') throw new Error('选择的游戏路径不是 EXE 文件')
  return normalized
}

function launch({ gamePath, relay, room, logicalIp, token, remoteIp, remoteDescription }) {
  const helper = locate(helperCandidates())
  const hook = locate(hookCandidates())
  const executable = resolveGamePath(gamePath)
  if (!helper || !hook) throw new Error('缺少 welnptgame.exe 或 welnpt.dll，请重新解压完整客户端')
  if (!relay || !room || !logicalIp || !token) throw new Error('房间连接凭据不完整，请退出房间后重新进入')

  const logPath = ensureLogPath()
  lastLogPath = logPath
  const environment = {
    ...process.env,
    WEL_NOTAP_RELAY: String(relay),
    WEL_NOTAP_ROOM: String(room),
    WEL_NOTAP_LOGICAL_IP: String(logicalIp),
    WEL_NOTAP_TOKEN: String(token),
    WEL_NOTAP_LOG_PATH: logPath,
  }
  if (iceProcess && iceAgentPort && iceHookPort) {
    environment.WEL_NOTAP_DIRECT_AGENT_PORT = String(iceAgentPort)
    environment.WEL_NOTAP_DIRECT_HOOK_PORT = String(iceHookPort)
    if (remoteIp) environment.WEL_NOTAP_DIRECT_PEER_IP = String(remoteIp)
    if (remoteDescription) setRemoteIce(remoteDescription)
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
  if (iceProcess && !iceProcess.killed) {
    try { iceProcess.stdin.write('EXIT\n') } catch {}
    try { iceProcess.kill() } catch {}
  }
  iceProcess = null
  iceState = 'waiting'
  iceLocalDescription = ''
  pendingIcePing = null
  pendingRelayPing = null
  lastRemoteDescription = ''
  lastLogPath = ''
  return { stopped: true }
}

function pingHost(host) {
  return {
    host: String(host || ''),
    reachable: false,
    summary: '无网卡逻辑 IP 不提供系统 ICMP Ping',
  }
}

module.exports = { configureIce: setRemoteIce, disconnect, launch, pingHost, status, transportStatus, prepareIce, pingIce, pingRelay }
