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
let iceExitError = ''
let iceDiagnostics = []
let pendingIcePing = null
let pendingRelayPing = null
let pendingRelayPeerPing = null
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
  const stamp = now.toISOString().replace(/[-:TZ.]/g, '').slice(0, 17)
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
  let pathName = 'pending'
  if (lastLogPath && fs.existsSync(lastLogPath)) {
    try {
      const contents = fs.readFileSync(lastLogPath, 'utf8')
      const lines = contents.trim().split(/\r?\n/).slice(-500).reverse()
      let latestDirectStateSeen = false
      for (const line of lines) {
        if (line.includes('"api":"direct-fallback"')) { pathName = 'relay'; break }
        if (line.includes('"api":"direct-state"')) {
          if (latestDirectStateSeen) continue
          latestDirectStateSeen = true
          if (line.includes('"state":"failed"') || line.includes('"state":"disconnected"')) { pathName = 'relay'; break }
          continue
        }
        if (!line.includes('"api":"transport-recv"') || !line.includes('"broadcast":false')) continue
        if (line.includes('"path":"direct"')) { pathName = 'direct'; break }
        if (line.includes('"path":"relay"')) { pathName = 'relay'; break }
      }
    } catch {}
  }
  const summary = pathName === 'direct'
    ? '当前联机：P2P 直连'
    : pathName === 'relay'
      ? '当前联机：云中继'
      : iceState === 'connected' || iceState === 'completed'
        ? 'P2P 直连已建立，等待游戏单播'
        : '游戏已启动，等待网络数据'
  return { path: pathName, directState: iceState, summary }
}

function chooseHookPort() {
  return 40000 + ((process.pid + Date.now()) % 18000)
}

function rememberIceDiagnostic(rawLine) {
  const line = String(rawLine || '').replace(/[\r\n]+/g, ' ').trim()
  if (!line || line === 'LOCAL_SDP_BEGIN' || line === 'LOCAL_SDP_END' || line.startsWith('a=')) return
  iceDiagnostics.push(line.slice(0, 300))
  if (iceDiagnostics.length > 12) iceDiagnostics.shift()
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
        reject(new Error(iceProcess !== child ? (iceExitError || 'ICE 辅助程序已退出') : 'ICE candidate 收集超时'))
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
  if (line.startsWith('GATHERING_STARTED ')) { iceState = 'gathering'; return }
  if (line.startsWith('STATE ')) { iceState = line.slice(6) || 'unknown'; return }
  if (line.startsWith('PING_RESULT ')) {
    const [, nonce, milliseconds] = line.split(' ')
    if (pendingIcePing && pendingIcePing.nonce === nonce) {
      const pending = pendingIcePing
      pendingIcePing = null
      if (pending.retryTimer) clearInterval(pending.retryTimer)
      if (pending.timeoutTimer) clearTimeout(pending.timeoutTimer)
      pending.resolve(Number(milliseconds) || 0)
    }
    return
  }
  if (line.startsWith('PING_UNAVAILABLE ')) {
    // The helper may report unavailable while ICE is transitioning to connected.
    // Keep retrying until the probe deadline so a stale Electron state cannot
    // turn an already usable game path into a false ICE timeout.
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
    return
  }
  if (line.startsWith('RELAY_PEER_PING_RESULT ')) {
    const [, nonce, milliseconds] = line.split(' ')
    if (pendingRelayPeerPing && pendingRelayPeerPing.nonce === nonce) {
      const pending = pendingRelayPeerPing
      pendingRelayPeerPing = null
      if (pending.retryTimer) clearInterval(pending.retryTimer)
      if (pending.timeoutTimer) clearTimeout(pending.timeoutTimer)
      pending.resolve(Number(milliseconds) || 0)
    }
    return
  }
  if (line.startsWith('RELAY_PEER_PING_UNAVAILABLE ')) {
    if (pendingRelayPeerPing) { const pending = pendingRelayPeerPing; pendingRelayPeerPing = null; pending.reject(new Error('中继玩家探测不可用')); }
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
  iceLineBuffer = ''
  iceSdpBuffer = ''
  readingIceSdp = false
  iceExitError = ''
  iceDiagnostics = []
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
      rememberIceDiagnostic(line)
      handleIceLine(line)
    }
  }
  child.stdout.on('data', consume)
  child.stderr.on('data', (chunk) => { rememberIceDiagnostic('stderr: ' + chunk.toString('utf8')) })
  child.once('close', (code) => {
    if (iceProcess === child) {
      iceProcess = null
      iceState = 'failed'
      const detail = iceDiagnostics.length ? '：' + iceDiagnostics.slice(-3).join(' | ') : ''
      iceExitError = 'ICE 辅助程序提前退出（代码 ' + (code ?? '未知') + '）' + detail
    }
  })
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

function waitForIceConnection(timeoutMs = 12000) {
  const child = iceProcess
  if (!child || child.killed) return Promise.reject(new Error('直连组件未运行'))
  if (iceState === 'connected' || iceState === 'completed') return Promise.resolve()
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + timeoutMs
    const timer = setInterval(() => {
      if (iceProcess !== child || child.killed) {
        clearInterval(timer)
        reject(new Error(iceExitError || '直连组件已退出'))
        return
      }
      if (iceState === 'connected' || iceState === 'completed') {
        clearInterval(timer)
        resolve()
        return
      }
      if (iceState === 'failed') {
        clearInterval(timer)
        reject(new Error('ICE 直连检查失败'))
        return
      }
      if (Date.now() >= deadline) {
        clearInterval(timer)
        reject(new Error('ICE 直连检查超时'))
      }
    }, 100)
  })
}

async function pingIce(remoteDescription) {
  if (!setRemoteIce(remoteDescription)) return Promise.reject(new Error('对方尚未完成 ICE candidate'))
  const nonceBase = Math.random().toString(36).slice(2, 12)
  return new Promise((resolve, reject) => {
    const pending = { nonce: '', resolve, reject, retryTimer: null, timeoutTimer: null, attempt: 0 }
    pendingIcePing = pending
    const send = () => {
      if (pendingIcePing !== pending || !iceProcess) return
      pending.nonce = nonceBase + '-' + String(++pending.attempt)
      try { iceProcess.stdin.write('PING ' + pending.nonce + '\n') } catch { /* timeout reports the failed probe */ }
    }
    send()
    pending.retryTimer = setInterval(send, 500)
    pending.timeoutTimer = setTimeout(() => {
      if (pendingIcePing !== pending) return
      if (pending.retryTimer) clearInterval(pending.retryTimer)
      pendingIcePing = null
      reject(new Error(iceState === 'connected' || iceState === 'completed' ? '直连 Ping 超时' : 'ICE 直连检查超时'))
    }, 12000)
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

function pingRelayPeer(remoteIp) {
  if (!iceProcess) return Promise.reject(new Error('中继玩家探测未准备'))
  if (!remoteIp) return Promise.reject(new Error('对方逻辑 IP 未知'))
  return new Promise((resolve, reject) => {
    const pending = { nonce: '', resolve, reject, retryTimer: null, timeoutTimer: null, remoteIp: String(remoteIp).trim(), sequence: Date.now() >>> 0 }
    pendingRelayPeerPing = pending
    const send = () => {
      if (pendingRelayPeerPing !== pending || !iceProcess) return
      pending.nonce = String(pending.sequence = (pending.sequence + 1) >>> 0)
      try { iceProcess.stdin.write('PING_RELAY_PEER ' + pending.nonce + ' ' + pending.remoteIp + '\n') } catch { /* timeout reports the failed probe */ }
    }
    send()
    pending.retryTimer = setInterval(send, 500)
    pending.timeoutTimer = setTimeout(() => {
      if (pendingRelayPeerPing !== pending) return
      if (pending.retryTimer) clearInterval(pending.retryTimer)
      pendingRelayPeerPing = null
      reject(new Error('中继玩家探测超时'))
    }, 8000)
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
  if (iceProcess && iceLocalDescription && iceAgentPort && iceHookPort) {
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
  if (pendingIcePing) {
    const pending = pendingIcePing
    pendingIcePing = null
    if (pending.retryTimer) clearInterval(pending.retryTimer)
    if (pending.timeoutTimer) clearTimeout(pending.timeoutTimer)
    pending.reject(new Error('已退出房间，直连探测已取消'))
  }
  pendingRelayPing = null
  if (pendingRelayPeerPing) {
    const pending = pendingRelayPeerPing
    pendingRelayPeerPing = null
    if (pending.retryTimer) clearInterval(pending.retryTimer)
    if (pending.timeoutTimer) clearTimeout(pending.timeoutTimer)
    pending.reject(new Error('已退出房间，中继玩家探测已取消'))
  }
  lastRemoteDescription = ''
  lastLogPath = ''
  iceExitError = ''
  iceDiagnostics = []
  return { stopped: true }
}

function pingHost(host) {
  return {
    host: String(host || ''),
    reachable: false,
    summary: '无网卡逻辑 IP 不提供系统 ICMP Ping',
  }
}

module.exports = { configureIce: setRemoteIce, disconnect, launch, pingHost, status, transportStatus, prepareIce, pingIce, pingRelay, pingRelayPeer }
