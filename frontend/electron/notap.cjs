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
let pendingRelayPing = null
let pendingRelayPeerPing = null
let relayPingSequence = Date.now() >>> 0
const relayPeerPingQueue = []
let lastRemoteDescription = ''
let lastLogPath = ''
let sessionLogPath = ''
let transportLogOffset = 0
let transportLogRemainder = ''
let transportPath = 'pending'
let formalGameStarted = false
let activeGamePeerIp = ''
let iceOptions = null
const gamePeerListeners = new Set()
const probeAgents = new Map()
let standbyAgentKey = ''
let standbyPromise = null
let standbyGeneration = 0

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

function describeLaunchFailure(detail, code) {
  const raw = String(detail || '').trim()
  if (code === 3 || raw.includes('Game executable not found')) return '游戏程序 WE8.exe 不存在或路径无法访问。' + (raw ? '\n' + raw : '')
  if (code === 4 || raw.includes('Hook module not found')) return '游戏网络组件 welnpt.dll 缺失或无法读取。' + (raw ? '\n' + raw : '')
  if (code === 6 || raw.includes('CreateProcess failed')) return '游戏程序 WE8.exe 启动失败。' + (raw ? '\n' + raw : '')
  if (code === 7 || raw.includes('Hook module injection failed')) return '游戏网络组件 welnpt.dll 加载失败。' + (raw ? '\n' + raw : '')
  if (code === 8 || raw.includes('Hook module did not initialize')) return '游戏网络组件 welnpt.dll 初始化超时。' + (raw ? '\n' + raw : '')
  if (code === 9 || raw.includes('ResumeThread')) return '游戏程序 WE8.exe 恢复运行失败。' + (raw ? '\n' + raw : '')
  if (code === 10 || raw.includes('Game executable exited')) return '游戏程序 WE8.exe 启动后立即退出。' + (raw ? '\n' + raw : '')
  if (code === 124) return '管理员权限启动器等待游戏组件超时。' + (raw ? '\n' + raw : '')
  return raw || '游戏启动辅助程序 welnptgame.exe 未能完成启动'
}

function ensureLogPath() {
  fs.mkdirSync(logDirectory, { recursive: true })
  const now = new Date()
  const stamp = now.toISOString().replace(/[-:TZ.]/g, '').slice(0, 17)
  return path.join(logDirectory, 'room-session-' + stamp + '.jsonl')
}

function candidateStats(description) {
  const text = String(description || '')
  const stats = { total: 0, host: 0, srflx: 0, relay: 0, prflx: 0, other: 0, bytes: Buffer.byteLength(text, 'utf8'), capacityRisk: false }
  for (const line of text.split(/\r?\n/)) {
    if (!line.startsWith('a=candidate:')) continue
    stats.total += 1
    const type = line.match(/\btyp\s+(host|srflx|relay|prflx)\b/)?.[1]
    if (type) stats[type] += 1
    else stats.other += 1
  }
  stats.capacityRisk = stats.total >= 60
  return stats
}

function ensureSessionLogPath() {
  if (!sessionLogPath) sessionLogPath = ensureLogPath()
  return sessionLogPath
}

function appendAgentEvent(role, event, details = {}) {
  const logPath = sessionLogPath || lastLogPath
  if (!logPath) return
  try {
    fs.appendFileSync(logPath, JSON.stringify({
      timestamp: new Date().toISOString(), api: 'ice-agent', role, event, ...details,
    }) + '\n', 'utf8')
  } catch {}
}

function rememberAgentLine(role, line) {
  const value = String(line || '').trim()
  if (!value || value === 'LOCAL_SDP_BEGIN' || value === 'LOCAL_SDP_END') return
  if (value.startsWith('GATHERING_STARTED ')) {
    const [, stunHost = '', stunPort = ''] = value.split(/\s+/)
    appendAgentEvent(role, 'gathering-start', { stunHost, stunPort: Number(stunPort) || 0 })
  } else if (value.startsWith('CANDIDATE ')) {
    appendAgentEvent(role, 'candidate', { type: value.slice(10).trim() || 'unknown' })
  } else if (value.startsWith('STATE ')) {
    appendAgentEvent(role, 'state', { state: value.slice(6).trim() || 'unknown' })
  } else if (value.startsWith('SELECTED_CANDIDATES ')) {
    appendAgentEvent(role, 'selected-candidates', { value: value.slice(20) })
  } else if (value.startsWith('SELECTED_ADDRESSES ')) {
    appendAgentEvent(role, 'selected-addresses', { value: value.slice(19) })
  } else {
    appendAgentEvent(role, 'diagnostic', { value: value.slice(0, 500) })
  }
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
    icePath: ice,
    ice: Boolean(ice),
    directState: iceState,
  }
}

function resetTransportTracking(logPath = '') {
  lastLogPath = logPath
  transportLogOffset = 0
  transportLogRemainder = ''
  transportPath = 'pending'
  formalGameStarted = false
}

function updateTransportPathFromLog() {
  if (!lastLogPath || !fs.existsSync(lastLogPath)) return
  try {
    const contents = fs.readFileSync(lastLogPath, 'utf8')
    if (contents.length < transportLogOffset) {
      transportLogOffset = 0
      transportLogRemainder = ''
      transportPath = 'pending'
    }
    const chunk = transportLogRemainder + contents.slice(transportLogOffset)
    transportLogOffset = contents.length
    const lines = chunk.split(/\r?\n/)
    transportLogRemainder = lines.pop() || ''
    for (const line of lines) {
      if (!line.trim()) continue
      let event
      try { event = JSON.parse(line) } catch { continue }
      if (event.api === 'direct-target') {
        transportPath = 'pending'
      } else if (event.api === 'session-state' && event.state === 'WAIT_JOIN') {
        transportPath = 'pending'
        formalGameStarted = false
      } else if (event.api === 'game-start' || (event.api === 'session-state' && event.state === 'PLAYING')) {
        formalGameStarted = true
      } else if (event.api === 'transport-lock') {
        if (event.path === 'direct' || event.path === 'relay') transportPath = event.path
      } else if (event.api === 'direct-fallback') {
        transportPath = 'relay'
      } else if (event.api === 'transport-recv' && event.broadcast === false) {
        if (event.path === 'direct') transportPath = 'direct'
        else if (event.path === 'relay' && event.length !== 64 && event.length !== 84 && transportPath === 'pending') transportPath = 'relay'
      }
    }
  } catch {}
}

function transportStatus() {
  updateTransportPathFromLog()
  const pathName = transportPath
  const summary = pathName === 'direct'
    ? '当前联机：P2P 直连'
    : pathName === 'relay'
      ? '当前联机：云中继'
      : '游戏已启动，正在选择本场连接'
  return { path: pathName, directState: iceState, gameStarted: formalGameStarted, summary }
}

function chooseHookPort() {
  return 40000 + ((process.pid + Date.now()) % 18000)
}

function nextRelayPingNonce() {
  relayPingSequence = (relayPingSequence + 1) >>> 0
  if (relayPingSequence === 0) relayPingSequence = 1
  return String(relayPingSequence)
}

function rememberIceDiagnostic(rawLine) {
  const line = String(rawLine || '').replace(/[\r\n]+/g, ' ').trim()
  if (!line || line === 'LOCAL_SDP_BEGIN' || line === 'LOCAL_SDP_END' || line.startsWith('a=')) return
  iceDiagnostics.push(line.slice(0, 300))
  if (iceDiagnostics.length > 12) iceDiagnostics.shift()
}

function waitForIceCandidate(child, timeoutMs = 26000) {
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

// The Hook only needs the local ICE agent port at game launch. Candidate
// gathering may continue in the background and must not delay the relay path.
function waitForIceAgent(timeoutMs = 1500) {
  return new Promise((resolve) => {
    const deadline = Date.now() + timeoutMs
    const timer = setInterval(() => {
      if (iceProcess && iceAgentPort && iceHookPort) {
        clearInterval(timer)
        resolve(true)
        return
      }
      if (Date.now() >= deadline || !iceProcess) {
        clearInterval(timer)
        resolve(false)
      }
    }, 50)
  })
}

function hasUsableIceCandidate(description) {
  return String(description || '').split(/\r?\n/).some((line) => line.startsWith('a=candidate:'))
}

function activeIceAgentIsReadyAndClean() {
  return Boolean(
    iceProcess && !iceProcess.killed && iceAgentPort && iceHookPort &&
    hasUsableIceCandidate(iceLocalDescription) &&
    !lastRemoteDescription && !activeGamePeerIp && iceState !== 'failed',
  )
}

function activeIceAgentSnapshot() {
  return {
    localDescription: iceLocalDescription,
    directState: iceState,
    agentPort: iceAgentPort,
    hookPort: iceHookPort,
  }
}

function handleIceLine(rawLine) {
  const line = rawLine.replace(/[\r\n]+$/, '')
  if (readingIceSdp) {
    if (line === 'LOCAL_SDP_END') {
      readingIceSdp = false
      iceLocalDescription = iceSdpBuffer
      iceSdpBuffer = ''
      appendAgentEvent('active', 'local-sdp', candidateStats(iceLocalDescription))
    } else {
      iceSdpBuffer += line + '\n'
    }
    return
  }
  if (line === 'LOCAL_SDP_BEGIN') { readingIceSdp = true; iceSdpBuffer = ''; return }
  if (line.startsWith('LOCAL_PORT ')) {
    iceAgentPort = Number(line.slice(11)) || 0
    appendAgentEvent('active', 'local-port', { port: iceAgentPort })
    return
  }
  if (line.startsWith('GATHERING_STARTED ')) { iceState = 'gathering'; rememberAgentLine('active', line); return }
  if (line.startsWith('STATE ')) {
    iceState = line.slice(6) || 'unknown'
    rememberAgentLine('active', line)
    // The next slot is prepared as soon as the active agent has a peer. This
    // keeps a quick game exit from leaving the next launch cold.
    if ((iceState === 'connected' || iceState === 'completed') && lastRemoteDescription) void prewarmIce()
    return
  }
  if (line.startsWith('CANDIDATE ') || line.startsWith('SELECTED_CANDIDATES ') || line.startsWith('SELECTED_ADDRESSES ')) {
    rememberAgentLine('active', line)
    return
  }
  if (line.startsWith('GAME_PEER ')) {
    const payload = line.slice(10).trim()
    const [logicalIp, sourcePort = '', targetPort = '', generation = '0'] = payload.split('|')
    if (logicalIp && /^\d{1,5}$/.test(sourcePort) && /^\d{1,5}$/.test(targetPort) && /^\d+$/.test(generation)) {
      for (const listener of gamePeerListeners) {
        try { listener({ logicalIp, transactionKey: `${logicalIp}|${sourcePort}|${targetPort}|${generation}` }) } catch {}
      }
    }
    return
  }
  if (line.startsWith('RELAY_PING_RESULT ')) {
    const [, nonce, milliseconds] = line.split(' ')
    if (pendingRelayPing && pendingRelayPing.nonce === nonce) {
      const pending = pendingRelayPing
      pendingRelayPing = null
      if (pending.timer) clearTimeout(pending.timer)
      pending.resolve(Number(milliseconds) || 0)
    }
    return
  }
  if (line.startsWith('RELAY_PING_UNAVAILABLE ')) {
    const [, nonce] = line.split(' ')
    if (pendingRelayPing && pendingRelayPing.nonce === nonce) {
      const pending = pendingRelayPing
      pendingRelayPing = null
      if (pending.timer) clearTimeout(pending.timer)
      pending.reject(new Error('中继探测不可用'))
    }
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
      startNextRelayPeerPing()
    }
    return
  }
  if (line.startsWith('RELAY_PEER_PING_UNAVAILABLE ')) {
    const [, nonce] = line.split(' ')
    if (pendingRelayPeerPing && pendingRelayPeerPing.nonce === nonce) {
      const pending = pendingRelayPeerPing
      pendingRelayPeerPing = null
      if (pending.retryTimer) clearInterval(pending.retryTimer)
      if (pending.timeoutTimer) clearTimeout(pending.timeoutTimer)
      pending.reject(new Error('中继玩家探测不可用'))
      startNextRelayPeerPing()
    }
  }
}

function startIceAgent({ stunHost, stunPort, relay, room, logicalIp, token, hookPort = 0 }) {
  const executable = locate(iceCandidates())
  if (!executable) throw new Error('缺少 welnptice.exe，请重新解压完整客户端')
  if (iceProcess && !iceProcess.killed) return waitForIceCandidate(iceProcess)
  iceHookPort = Number(hookPort) || chooseHookPort()
  iceOptions = { stunHost, stunPort, relay, room, logicalIp, token }
  iceLocalDescription = ''
  iceAgentPort = 0
  iceState = 'gathering'
  iceLineBuffer = ''
  iceSdpBuffer = ''
  readingIceSdp = false
  iceExitError = ''
  iceDiagnostics = []
  lastRemoteDescription = ''
  activeGamePeerIp = ''
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
  appendAgentEvent('active', 'started', { stunHost, stunPort, hookPort: iceHookPort })
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
    appendAgentEvent('active', 'stopped', { code: code ?? null })
    if (iceProcess === child) {
      iceProcess = null
      iceState = 'failed'
      const detail = iceDiagnostics.length ? '：' + iceDiagnostics.slice(-3).join(' | ') : ''
      iceExitError = 'ICE 辅助程序提前退出（代码 ' + (code ?? '未知') + '）' + detail
    }
  })
  return waitForIceCandidate(child)
}

function stopIceChild(child) {
  if (!child || child.killed) return Promise.resolve()
  return new Promise((resolve) => {
    let settled = false
    const finish = () => { if (!settled) { settled = true; resolve() } }
    const timer = setTimeout(() => { try { child.kill() } catch {}; finish() }, 1500)
    child.once('close', () => { clearTimeout(timer); finish() })
    try { child.stdin.write('EXIT\n') } catch { try { child.kill() } catch {} }
  })
}

async function clearStandbyAgent() {
  standbyGeneration += 1
  const key = standbyAgentKey
  standbyAgentKey = ''
  const pendingKey = standbyPromise?.probeKey
  standbyPromise = null
  if (key) stopProbeIce(key)
  if (pendingKey && pendingKey !== key) stopProbeIce(pendingKey)
}

async function prepareIce(options) {
  ensureSessionLogPath()
  await startIceAgent(options || {})
  // A is now clean and ready for the first match. Gather B immediately while
  // the player is in the room, rather than waiting for game launch or failure.
  void prewarmIce()
  return { localDescription: iceLocalDescription, directState: iceState, agentPort: iceAgentPort, hookPort: iceHookPort }
}

async function resetIce() {
  if (!iceOptions) throw new Error('直连组件尚未准备')
  await clearStandbyAgent()
  const previous = iceProcess
  iceProcess = null
  await stopIceChild(previous)
  await startIceAgent({ ...iceOptions, hookPort: iceHookPort })
  return { localDescription: iceLocalDescription, directState: iceState, agentPort: iceAgentPort, hookPort: iceHookPort }
}

async function prepareGameIce() {
  if (!iceOptions) throw new Error('直连组件尚未准备')
  if (activeIceAgentIsReadyAndClean()) return activeIceAgentSnapshot()

  // Prefer a fully gathered standby from the previous match. If none exists,
  // create a fresh active agent and wait for its candidate in the existing
  // game-launch loading state.
  const activated = await activateIce()
  if (activated && activeIceAgentIsReadyAndClean()) return activeIceAgentSnapshot()

  const fresh = await resetIce()
  if (!activeIceAgentIsReadyAndClean()) throw new Error('直连组件未返回可用 candidate')
  return fresh
}

function attachActiveIceProcess(child) {
  let buffer = ''
  const consume = (chunk) => {
    buffer += chunk.toString('utf8')
    let newline
    while ((newline = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, newline + 1)
      buffer = buffer.slice(newline + 1)
      rememberIceDiagnostic(line)
      handleIceLine(line)
    }
  }
  child.stdout.on('data', consume)
  child.stderr.on('data', (chunk) => { rememberIceDiagnostic('stderr: ' + chunk.toString('utf8')) })
  child.once('close', (code) => {
    if (iceProcess !== child) return
    iceProcess = null
    iceState = 'failed'
    const detail = iceDiagnostics.length ? '：' + iceDiagnostics.slice(-3).join(' | ') : ''
    iceExitError = 'ICE 辅助程序提前退出（代码 ' + (code ?? '未知') + '）' + detail
  })
}

async function prewarmIce() {
  if (!iceOptions) return { ready: false, state: 'waiting' }
  if (standbyAgentKey && probeAgents.has(standbyAgentKey)) return { ready: true, state: 'ready' }
  if (standbyPromise) return standbyPromise
  const generation = standbyGeneration
  const options = { ...iceOptions, hookPort: iceHookPort }
  const gathering = createProbeIce({ ...options, standby: true, hookPort: iceHookPort })
  standbyPromise = gathering
    .then((result) => {
      if (generation !== standbyGeneration) {
        stopProbeIce(result.probeKey)
        return { ready: false, state: 'cancelled' }
      }
      standbyAgentKey = result.probeKey
      return { ready: true, state: 'ready', localDescription: result.localDescription }
    })
    .catch((error) => ({ ready: false, state: 'failed', error: String(error?.message || error) }))
    .finally(() => { if (generation === standbyGeneration) standbyPromise = null })
  standbyPromise.probeKey = gathering.probeKey
  return standbyPromise
}

async function activateIce() {
  if (!iceOptions) return null
  const key = standbyAgentKey
  const standby = key ? probeAgents.get(key) : null
  if (!standby || !standby.localDescription || !standby.localPort) return null

  const previous = iceProcess
  iceProcess = null
  await stopIceChild(previous)
  probeAgents.delete(key)
  standbyAgentKey = ''
  standbyPromise = null

  iceProcess = standby.child
  iceLocalDescription = standby.localDescription
  iceAgentPort = standby.localPort
  iceHookPort = standby.hookPort || iceHookPort
  iceOptions = standby.options || iceOptions
  iceState = 'gathering'
  iceExitError = ''
  iceDiagnostics = []
  iceLineBuffer = ''
  iceSdpBuffer = ''
  readingIceSdp = false
  lastRemoteDescription = ''
  activeGamePeerIp = ''
  appendAgentEvent('active', 'activated', { key, port: iceAgentPort, hookPort: iceHookPort })
  attachActiveIceProcess(iceProcess)
  try { iceProcess.stdin.write('ACTIVATE\n') } catch {
    await stopIceChild(iceProcess)
    iceProcess = null
    iceState = 'failed'
    return null
  }
  return { localDescription: iceLocalDescription, directState: iceState, agentPort: iceAgentPort, hookPort: iceHookPort }
}

function setRemoteIce(remoteDescription, remoteIp = '') {
  if (!iceProcess || !remoteDescription) return false
  const normalized = String(remoteDescription).replace(/\r?\n/g, '\n').replace(/\n*$/, '\n')
  const peerIp = String(remoteIp || '').trim()
  if (lastRemoteDescription === normalized) return !peerIp || peerIp === activeGamePeerIp
  if (lastRemoteDescription) return false
  if (peerIp) {
    iceProcess.stdin.write('TARGET ' + peerIp + '\n')
    activeGamePeerIp = peerIp
  }
  iceProcess.stdin.write('REMOTE_SDP_BEGIN\n' + normalized + 'REMOTE_SDP_END\n')
  lastRemoteDescription = normalized
  appendAgentEvent('active', 'remote-sdp', { ...candidateStats(normalized), remoteIp: peerIp })
  // The active agent is now committed to this peer. Start the next slot
  // immediately so a quick exit or first ICE failure does not leave a cold
  // agent for the next launch.
  void prewarmIce()
  return true
}

function onGamePeer(listener) {
  if (typeof listener !== 'function') return () => {}
  gamePeerListeners.add(listener)
  return () => gamePeerListeners.delete(listener)
}

function randomProbeKey() {
  return 'probe-' + Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 10)
}

function stopProbeIce(probeKey) {
  const probe = probeAgents.get(probeKey)
  if (!probe) return { stopped: true }
  appendAgentEvent(probe.standby ? 'standby' : 'probe', 'stopping', { key: probeKey, reason: 'rotation-or-disconnect' })
  probeAgents.delete(probeKey)
  if (probe.timeoutTimer) clearTimeout(probe.timeoutTimer)
  if (probe.retryTimer) clearInterval(probe.retryTimer)
  if (probe.pendingPing) {
    const pending = probe.pendingPing
    probe.pendingPing = null
    pending.reject(new Error('直连探测已结束'))
  }
  if (probe.remoteWaiter) {
    const waiter = probe.remoteWaiter
    probe.remoteWaiter = null
    clearTimeout(waiter.timer)
    waiter.reject(new Error('临时 ICE 探测已结束'))
  }
  try { probe.child.stdin.write('EXIT\n') } catch {}
  try { probe.child.kill() } catch {}
  return { stopped: true }
}

function createProbeIce({ stunHost, stunPort, standby = false, hookPort = 0, relay = '', room = '', logicalIp = '', token = '' }) {
  const executable = locate(iceCandidates())
  if (!executable) return Promise.reject(new Error('缺少 welnptice.exe，请重新解压完整客户端'))
  const key = randomProbeKey()
  const environment = { ...process.env }
  if (standby && relay && room && logicalIp && token) {
    environment.WEL_NOTAP_RELAY = String(relay)
    environment.WEL_NOTAP_ROOM = String(room)
    environment.WEL_NOTAP_LOGICAL_IP = String(logicalIp)
    environment.WEL_NOTAP_TOKEN = String(token)
  } else {
    delete environment.WEL_NOTAP_RELAY
    delete environment.WEL_NOTAP_ROOM
    delete environment.WEL_NOTAP_LOGICAL_IP
    delete environment.WEL_NOTAP_TOKEN
  }
  const args = ['--stun-host', String(stunHost || ''), '--stun-port', String(stunPort || 0)]
  if (standby) args.push('--hook-port', String(hookPort || 0), '--standby')
  else args.push('--no-hook')
  const child = spawn(executable, args, {
    windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: environment,
  })
  const probe = {
    key, child, state: 'gathering', localDescription: '', localPort: 0,
    buffer: '', sdpBuffer: '', readingSdp: false, error: '', remoteError: '',
    remoteWaiter: null, pingUnavailable: false, pendingPing: null,
    retryTimer: null, timeoutTimer: null, standby, hookPort: Number(hookPort) || 0,
    options: { stunHost, stunPort, relay, room, logicalIp, token },
  }
  appendAgentEvent(standby ? 'standby' : 'probe', 'started', { key, stunHost, stunPort, hookPort: Number(hookPort) || 0 })
  probeAgents.set(key, probe)
  const consume = (chunk) => {
    probe.buffer += chunk.toString('utf8')
    let newline
    while ((newline = probe.buffer.indexOf('\n')) >= 0) {
      const line = probe.buffer.slice(0, newline).replace(/\r$/, '')
      probe.buffer = probe.buffer.slice(newline + 1)
      if (probe.readingSdp) {
        if (line === 'LOCAL_SDP_END') {
          probe.readingSdp = false
          probe.localDescription = probe.sdpBuffer
          probe.sdpBuffer = ''
          appendAgentEvent(standby ? 'standby' : 'probe', 'local-sdp', { key, ...candidateStats(probe.localDescription) })
        } else probe.sdpBuffer += line + '\n'
        continue
      }
      if (line === 'LOCAL_SDP_BEGIN') { probe.readingSdp = true; probe.sdpBuffer = ''; continue }
      if (line.startsWith('LOCAL_PORT ')) {
        probe.localPort = Number(line.slice(11)) || 0
        appendAgentEvent(standby ? 'standby' : 'probe', 'local-port', { key, port: probe.localPort })
        continue
      }
      if (line.startsWith('CANDIDATE ')) { rememberAgentLine(standby ? 'standby' : 'probe', line); continue }
      if (line.startsWith('STATE ')) { probe.state = line.slice(6) || 'unknown'; rememberAgentLine(standby ? 'standby' : 'probe', line); continue }
      if (line === 'REMOTE_SET') {
        if (probe.remoteWaiter) {
          const waiter = probe.remoteWaiter
          probe.remoteWaiter = null
          clearTimeout(waiter.timer)
          waiter.resolve(true)
        }
        continue
      }
      if (line === 'ERROR remote-description') {
        probe.remoteError = '远端 candidate 无效，ICE 辅助程序拒绝了远端 SDP'
        if (probe.remoteWaiter) {
          const waiter = probe.remoteWaiter
          probe.remoteWaiter = null
          clearTimeout(waiter.timer)
          waiter.reject(new Error(probe.remoteError))
        }
        continue
      }
      if (line.startsWith('PING_UNAVAILABLE ')) {
        probe.pingUnavailable = true
        continue
      }
      if (line.startsWith('PING_RESULT ') && probe.pendingPing) {
        const [, nonce, milliseconds] = line.split(' ')
        if (probe.pendingPing.nonce === nonce) {
          const pending = probe.pendingPing
          probe.pendingPing = null
          if (probe.retryTimer) clearInterval(probe.retryTimer)
          if (probe.timeoutTimer) clearTimeout(probe.timeoutTimer)
          probe.retryTimer = null
          probe.timeoutTimer = null
          pending.resolve(Number(milliseconds) || 0)
        }
      }
    }
  }
  child.stdout.on('data', consume)
  child.stderr.on('data', (chunk) => { probe.error = String(chunk).trim().slice(0, 300) || probe.error })
  child.once('close', (code) => {
    appendAgentEvent(standby ? 'standby' : 'probe', 'stopped', { key, code: code ?? null })
    if (probeAgents.get(key) !== probe) return
    probe.error ||= '临时 ICE 辅助程序提前退出（代码 ' + (code ?? '未知') + '）'
    if (probe.remoteWaiter) {
      const waiter = probe.remoteWaiter
      probe.remoteWaiter = null
      clearTimeout(waiter.timer)
      waiter.reject(new Error(probe.error))
    }
    if (probe.pendingPing) {
      const pending = probe.pendingPing
      probe.pendingPing = null
      pending.reject(new Error(probe.error))
    }
  })
  const gatheringPromise = new Promise((resolve, reject) => {
    const deadline = Date.now() + (standby ? 26000 : 12000)
    const timer = setInterval(() => {
      if (probe.localDescription && probe.localPort && probeAgents.get(key) === probe) {
        clearInterval(timer)
        resolve({ probeKey: key, localDescription: probe.localDescription })
      } else if (Date.now() >= deadline || probeAgents.get(key) !== probe || child.killed) {
        clearInterval(timer)
        stopProbeIce(key)
        reject(new Error(probe.error || '临时 ICE candidate 收集超时'))
      }
    }, 100)
  })
  gatheringPromise.probeKey = key
  return gatheringPromise
}

function configureProbeIce(probeKey, remoteDescription) {
  const probe = probeAgents.get(probeKey)
  if (!probe || !remoteDescription) return Promise.resolve(false)
  if (probe.remoteWaiter) return Promise.reject(new Error('临时 ICE 正在设置远端 candidate'))
  try {
    const normalized = String(remoteDescription).replace(/\r?\n/g, '\n').replace(/\n*$/, '\n')
    appendAgentEvent('probe', 'remote-sdp', { key: probeKey, ...candidateStats(normalized) })
    probe.child.stdin.write('REMOTE_SDP_BEGIN\n' + normalized + 'REMOTE_SDP_END\n')
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (!probe.remoteWaiter) return
        probe.remoteWaiter = null
        reject(new Error('临时 ICE 未确认远端 candidate'))
      }, 3000)
      probe.remoteWaiter = { resolve, reject, timer }
    })
  } catch {
    return Promise.resolve(false)
  }
}

function pingProbeIce(probeKey) {
  const probe = probeAgents.get(probeKey)
  if (!probe) return Promise.reject(new Error('直连探测已结束'))
  const nonceBase = Math.random().toString(36).slice(2, 12)
  return new Promise((resolve, reject) => {
    const pending = { nonce: '', resolve, reject, attempt: 0 }
    probe.pendingPing = pending
    const send = () => {
      if (probe.pendingPing !== pending || probe.child.killed) return
      pending.nonce = nonceBase + '-' + String(++pending.attempt)
      try { probe.child.stdin.write('PING ' + pending.nonce + '\n') } catch {}
    }
    send()
    probe.retryTimer = setInterval(send, 500)
    probe.timeoutTimer = setTimeout(() => {
      if (probe.pendingPing !== pending) return
      probe.pendingPing = null
      if (probe.retryTimer) clearInterval(probe.retryTimer)
      probe.retryTimer = null
      const detail = probe.remoteError || (probe.pingUnavailable ? 'ICE 尚未建立，PING_UNAVAILABLE' : '')
      const prefix = probe.state === 'connected' || probe.state === 'completed' ? '直连 Ping 超时' : 'ICE 直连检查超时'
      reject(new Error(detail ? `${prefix}（${detail}）` : prefix))
    }, 12000)
  })
}

function pingRelay() {
  if (!iceProcess) return Promise.reject(new Error('中继探测未准备'))
  if (pendingRelayPing) return pendingRelayPing.promise
  const nonce = nextRelayPingNonce()
  let pending
  const promise = new Promise((resolve, reject) => {
    pending = { nonce, resolve, reject, timer: null, promise: null }
    pendingRelayPing = pending
    pending.timer = setTimeout(() => {
      if (pendingRelayPing !== pending) return
      pendingRelayPing = null
      reject(new Error('中继探测超时'))
    }, 5000)
    try { iceProcess.stdin.write('PING_RELAY ' + nonce + '\n') } catch {
      clearTimeout(pending.timer)
      pendingRelayPing = null
      reject(new Error('中继探测不可用'))
    }
  })
  pending.promise = promise
  return promise
}

function startNextRelayPeerPing() {
  if (pendingRelayPeerPing || !iceProcess) return
  const pending = relayPeerPingQueue.shift()
  if (!pending) return
  pendingRelayPeerPing = pending
  const send = () => {
    if (pendingRelayPeerPing !== pending || !iceProcess) return
    pending.nonce = nextRelayPingNonce()
    try { iceProcess.stdin.write('PING_RELAY_PEER ' + pending.nonce + ' ' + pending.remoteIp + '\n') } catch {}
  }
  send()
  pending.retryTimer = setInterval(send, 500)
  pending.timeoutTimer = setTimeout(() => {
    if (pendingRelayPeerPing !== pending) return
    if (pending.retryTimer) clearInterval(pending.retryTimer)
    pendingRelayPeerPing = null
    pending.reject(new Error('中继玩家探测超时'))
    startNextRelayPeerPing()
  }, 8000)
}

function pingRelayPeer(remoteIp) {
  if (!iceProcess) return Promise.reject(new Error('中继玩家探测未准备'))
  if (!remoteIp) return Promise.reject(new Error('对方逻辑 IP 未知'))
  return new Promise((resolve, reject) => {
    relayPeerPingQueue.push({ nonce: '', resolve, reject, retryTimer: null, timeoutTimer: null, remoteIp: String(remoteIp).trim() })
    startNextRelayPeerPing()
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

function powerShellLiteral(value) {
  return "'" + String(value).replace(/'/g, "''") + "'"
}

function windowsCommandArgument(value) {
  const argument = String(value)
  if (!/[\s"]/u.test(argument)) return argument
  return '"' + argument.replace(/(\\*)"/g, '$1$1\\"').replace(/(\\+)$/g, '$1$1') + '"'
}

function elevatedLauncherArguments({ gamePath, relay, room, logicalIp, token, direct = true }) {
  const helper = locate(helperCandidates())
  const hook = locate(hookCandidates())
  const executable = resolveGamePath(gamePath)
  if (!helper) throw new Error('游戏启动辅助程序 welnptgame.exe 缺失，请重新安装完整客户端')
  if (!hook) throw new Error('游戏网络组件 welnpt.dll 缺失，请重新安装完整客户端')
  if (!relay || !room || !logicalIp || !token) throw new Error('房间连接凭据不完整，请退出房间后重新进入')
  const logPath = ensureSessionLogPath()
  resetTransportTracking(logPath)
  const args = ['--game', executable, '--hook', hook, '--relay', String(relay), '--room', String(room),
    '--logical-ip', String(logicalIp), '--token', String(token), '--log', logPath]
  if (direct && iceProcess && iceAgentPort && iceHookPort) {
    args.push('--direct-agent-port', String(iceAgentPort), '--direct-hook-port', String(iceHookPort))
  }
  return { helper, args, logPath }
}

async function launchElevated(options) {
  // Match the normal launch path: validate or rotate to a clean direct agent
  // before the elevated helper injects the Hook.
  if (options?.direct !== false) {
    await prepareGameIce()
  }
  const { helper, args, logPath } = elevatedLauncherArguments(options || {})
  const argumentList = args.map(windowsCommandArgument).join(' ')
  // Start-Process -Wait also follows WE8.exe, which the helper launches. Wait for
  // the helper process itself so the UI can resume as soon as Hook injection ends.
  const script = `$process = Start-Process -FilePath ${powerShellLiteral(helper)} -ArgumentList ${powerShellLiteral(argumentList)} -Verb RunAs -WindowStyle Hidden -PassThru; if (-not $process.WaitForExit(30000)) { exit 124 }; exit $process.ExitCode`
  const encoded = Buffer.from(script, 'utf16le').toString('base64')
  const child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', encoded], {
    windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
  })
  lastProcess = child
  void prewarmIce()
  const output = []
  child.stdout.on('data', (chunk) => output.push(chunk.toString('utf8')))
  child.stderr.on('data', (chunk) => output.push(chunk.toString('utf8')))
  return new Promise((resolve, reject) => {
    child.once('error', (error) => reject(new Error('管理员权限启动器 powershell.exe 无法运行：' + error.message)))
    child.once('close', (code) => {
      lastProcess = null
      const detail = output.join('').trim()
      if (code !== 0) reject(new Error(describeLaunchFailure(detail || '用户取消了管理员授权，或提权启动失败', code)))
      else resolve({ started: true, detail: 'WE8 已通过管理员授权启动，日志：' + logPath, warnings: [] })
    })
  })
}

async function launch({ gamePath, relay, room, logicalIp, token, direct = true }) {
  const helper = locate(helperCandidates())
  const hook = locate(hookCandidates())
  const executable = resolveGamePath(gamePath)
  if (!helper) throw new Error('游戏启动辅助程序 welnptgame.exe 缺失，请重新安装完整客户端')
  if (!hook) throw new Error('游戏网络组件 welnpt.dll 缺失，请重新安装完整客户端')
  if (!relay || !room || !logicalIp || !token) throw new Error('房间连接凭据不完整，请退出房间后重新进入')

  if (direct) {
    await prepareGameIce()
  }

  const logPath = ensureSessionLogPath()
  resetTransportTracking(logPath)
  const environment = {
    ...process.env,
    WEL_NOTAP_RELAY: String(relay),
    WEL_NOTAP_ROOM: String(room),
    WEL_NOTAP_LOGICAL_IP: String(logicalIp),
    WEL_NOTAP_TOKEN: String(token),
    WEL_NOTAP_LOG_PATH: logPath,
  }
  if (direct && iceProcess && iceAgentPort && iceHookPort) {
    environment.WEL_NOTAP_DIRECT_AGENT_PORT = String(iceAgentPort)
    environment.WEL_NOTAP_DIRECT_HOOK_PORT = String(iceHookPort)
  }
  const child = spawn(helper, ['--game', executable, '--hook', hook], {
    cwd: path.dirname(executable),
    env: environment,
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  lastProcess = child
  void prewarmIce()
  const output = []
  child.stdout.on('data', (chunk) => output.push(chunk.toString('utf8')))
  child.stderr.on('data', (chunk) => output.push(chunk.toString('utf8')))
  return new Promise((resolve, reject) => {
    child.once('error', (error) => reject(new Error('游戏启动辅助程序 welnptgame.exe 无法运行：' + error.message)))
    child.once('close', (code) => {
      lastProcess = null
      const detail = output.join('').trim()
      if (code !== 0) reject(new Error(describeLaunchFailure(detail, code)))
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
    appendAgentEvent('active', 'stopping', { reason: 'disconnect' })
    try { iceProcess.stdin.write('EXIT\n') } catch {}
    try { iceProcess.kill() } catch {}
  }
  iceProcess = null
  await clearStandbyAgent()
  iceState = 'waiting'
  iceLocalDescription = ''
  if (pendingRelayPing) {
    const pending = pendingRelayPing
    pendingRelayPing = null
    if (pending.timer) clearTimeout(pending.timer)
    pending.reject(new Error('已退出房间，中继探测已取消'))
  }
  if (pendingRelayPeerPing) {
    const pending = pendingRelayPeerPing
    pendingRelayPeerPing = null
    if (pending.retryTimer) clearInterval(pending.retryTimer)
    if (pending.timeoutTimer) clearTimeout(pending.timeoutTimer)
    pending.reject(new Error('已退出房间，中继玩家探测已取消'))
  }
  while (relayPeerPingQueue.length) {
    relayPeerPingQueue.shift().reject(new Error('已退出房间，中继玩家探测已取消'))
  }
  lastRemoteDescription = ''
  activeGamePeerIp = ''
  resetTransportTracking()
  sessionLogPath = ''
  iceExitError = ''
  iceDiagnostics = []
  for (const probeKey of [...probeAgents.keys()]) stopProbeIce(probeKey)
  return { stopped: true }
}

function pingHost(host) {
  return {
    host: String(host || ''),
    reachable: false,
    summary: '无网卡逻辑 IP 不提供系统 ICMP Ping',
  }
}

module.exports = {
  configureIce: setRemoteIce,
  onGamePeer,
  disconnect,
  launch,
  launchElevated,
  pingHost,
  status,
  transportStatus,
  prepareIce,
  prepareGameIce,
  resetIce,
  prewarmIce,
  activateIce,
  pingRelay,
  pingRelayPeer,
}
