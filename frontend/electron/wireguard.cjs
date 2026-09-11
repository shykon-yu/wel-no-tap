const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const dgram = require('node:dgram')
const { spawn, spawnSync } = require('node:child_process')

const appData = path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'WELPlatform', 'wireguard')
const bundledRuntimeRoot = path.join(appData, 'runtime')
const tunnelName = 'WELGame'
let bridgeProcess = null
let bridgeLineBuffer = ''
let bridgeWaiters = new Set()
let transportListeners = new Set()
let gamePeerListeners = new Set()
let transportState = { path: 'relay', directState: 'idle', summary: '中继' }

function emptyIdentity() { return { runtime: null, privateKey: '', publicKey: '', virtualIp: '', subnetCidr: '', server: null, agentPort: 0, hookPort: 0, configPath: '' } }
let identityState = emptyIdentity()

function runtimeRoots() {
  return [
    path.join(process.resourcesPath || '', 'welhelper'),
    path.join(__dirname, '..', 'resources', 'welhelper'),
    path.join(__dirname, '..', 'build'),
    bundledRuntimeRoot,
  ].filter(Boolean)
}

function findRuntime() {
  const roots = runtimeRoots().flatMap((root) => [
    root,
    path.join(root, 'wireguard'),
    path.join(root, 'WireGuard'),
    path.join(root, 'wireguard', 'runtime'),
    path.join(root, 'wireguard', 'runtime', 'WireGuard'),
    path.join(root, 'WireGuard', 'runtime'),
  ])
  if (process.env.ProgramFiles) roots.push(path.join(process.env.ProgramFiles, 'WireGuard'))
  if (process.env['ProgramFiles(x86)']) roots.push(path.join(process.env['ProgramFiles(x86)'], 'WireGuard'))
  const bridge = runtimeRoots().map((root) => path.join(root, 'welnptwg.exe')).find(fs.existsSync) || null
  const wireguard = roots.map((root) => path.join(root, 'wireguard.exe')).find(fs.existsSync) || null
  const wg = roots.map((root) => path.join(root, 'wg.exe')).find(fs.existsSync) || null
  return { wireguard, wg, bridge, available: Boolean(wireguard && wg && bridge), adapterReady: Boolean(identityState.configPath) }
}

function wireguardInstallerCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'wireguard', 'wireguard-amd64-0.5.3.msi'),
    path.join(__dirname, '..', 'resources', 'wireguard', 'wireguard-amd64-0.5.3.msi'),
  ].filter(Boolean)
}

function locateWireGuardInstaller() {
  return wireguardInstallerCandidates().find((candidate) => fs.existsSync(candidate)) || null
}

function status() {
  const runtime = findRuntime()
  if (!runtime.bridge) return { ...runtime, message: '网卡数据组件缺失，当前使用云中继' }
  if (!runtime.available) return { ...runtime, message: '系统未安装 WireGuard，当前使用云中继' }
  return { ...runtime, message: identityState.configPath ? '网卡组件已就绪，等待比赛对手' : '网卡运行组件已就绪' }
}

function runSync(executable, args, options = {}) {
  const result = spawnSync(executable, args, { encoding: 'utf8', windowsHide: true, ...options })
  if (result.status !== 0) {
    const detail = String(result.stderr || result.stdout || result.error?.message || '网卡组件执行失败').trim()
    throw new Error(`${detail}${result.status == null ? '' : ` (exit ${result.status})`}`)
  }
  return String(result.stdout || '').trim()
}

function powershellLiteral(value) {
  return "'" + String(value).replace(/'/g, "''") + "'"
}

function runElevated(executable, args, timeoutMs = 30000) {
  const script = `$p=Start-Process -FilePath ${powershellLiteral(executable)} -ArgumentList @(${args.map(powershellLiteral).join(',')}) -Verb RunAs -WindowStyle Hidden -Wait -PassThru; if ($null -eq $p) { exit 1 }; exit $p.ExitCode`
  const encoded = Buffer.from(script, 'utf16le').toString('base64')
  return new Promise((resolve, reject) => {
    const child = spawn('powershell.exe', ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', encoded], {
      windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
    })
    const output = []
    let settled = false
    const finish = (error) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      if (error) reject(error)
      else resolve()
    }
    const timer = setTimeout(() => {
      try { child.kill() } catch {}
      finish(new Error('WireGuard 服务权限请求超时'))
    }, timeoutMs)
    child.stdout.on('data', (chunk) => output.push(chunk.toString('utf8')))
    child.stderr.on('data', (chunk) => output.push(chunk.toString('utf8')))
    child.once('error', (error) => finish(error))
    child.once('close', (code) => {
      if (code === 0) return finish()
      finish(new Error(String(output.join('').trim() || `WireGuard 服务操作失败（代码 ${code ?? '未知'}）`)))
    })
  })
}

function needsElevation(error) {
  return /(?:exit\s+740|elevation|required|access is denied|拒绝访问|管理员)/i.test(String(error?.message || error || ''))
}

async function installBundledWireGuard(installer) {
  if (process.platform !== 'win32') throw new Error('WireGuard 自动安装仅支持 Windows')
  if (!installer || !fs.existsSync(installer)) throw new Error('当前安装包缺少 WireGuard 驱动安装文件')
  // Administrative extraction keeps the signed WireGuard/Wintun files but
  // does not register the official manager in Add/Remove Programs or launch
  // its GUI. The tunnel service is installed later for the temporary WELGame
  // config by wireguard.exe itself.
  fs.rmSync(bundledRuntimeRoot, { recursive: true, force: true })
  fs.mkdirSync(bundledRuntimeRoot, { recursive: true })
  await runElevated('msiexec.exe', ['/a', installer, `TARGETDIR=${bundledRuntimeRoot}`, '/qn', '/norestart', 'DO_NOT_LAUNCH=1'], 120000)
}

async function ensureWireGuardRuntime(current) {
  if (current.available) return current
  const installer = locateWireGuardInstaller()
  if (!installer) return current
  try {
    await installBundledWireGuard(installer)
  } catch (error) {
    return { ...current, message: `WireGuard 自动安装失败：${error?.message || error}` }
  }
  for (let attempt = 0; attempt < 20; attempt += 1) {
    const runtime = findRuntime()
    if (runtime.available) return runtime
    await new Promise((resolve) => setTimeout(resolve, 500))
  }
  return { ...findRuntime(), message: 'WireGuard 安装完成，但运行组件尚未就绪，请重试' }
}

async function runTunnelCommand(executable, args, { ignoreFailure = false } = {}) {
  try {
    runSync(executable, args)
    return
  } catch (error) {
    if (process.platform === 'win32' && needsElevation(error)) {
      await runElevated(executable, args)
      return
    }
    if (!ignoreFailure) throw error
  }
}

function endpoint(host, port) {
  const value = String(host || '').trim()
  if (!value || !Number(port)) return ''
  return value.includes(':') ? `[${value}]:${Number(port)}` : `${value}:${Number(port)}`
}

function serverVirtualIp(subnetCidr) {
  const match = String(subnetCidr || '').match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.\d{1,3}\/(?:24|25|26|27|28)$/)
  return match ? `${match[1]}.${match[2]}.${match[3]}.1` : ''
}

function writeInitialConfig() {
  const server = identityState.server || {}
  const serverIp = serverVirtualIp(identityState.subnetCidr)
  const serverEndpoint = endpoint(server.host, server.port)
  if (!identityState.privateKey || !identityState.virtualIp || !server.publicKey || !serverEndpoint || !serverIp) throw new Error('WireGuard 服务器配置不完整')
  // The server only learns the NAT endpoint after it has accepted this peer.
  // Use a brief rendezvous keepalive while the player remains in the room;
  // this does not create peers for other room members.
  const contents = ['[Interface]', `PrivateKey = ${identityState.privateKey}`, `Address = ${identityState.virtualIp}/32`, '', '[Peer]', `PublicKey = ${server.publicKey}`, `AllowedIPs = ${serverIp}/32`, `Endpoint = ${serverEndpoint}`, 'PersistentKeepalive = 5', ''].join('\r\n')
  fs.mkdirSync(appData, { recursive: true })
  const configPath = path.join(appData, `${tunnelName}.conf`)
  fs.writeFileSync(configPath, contents, { encoding: 'utf8', mode: 0o600 })
  identityState.configPath = configPath
  return configPath
}

async function prepare(options = {}) {
  await disconnect()
  const current = await ensureWireGuardRuntime(status())
  if (!current.available) return current
  const server = options.server || {}
  if (!options.virtualIp || !options.subnetCidr || !server.host || !server.port || !server.publicKey) return { ...current, available: false, adapterReady: false, message: '服务器未下发完整网卡配置，当前使用云中继' }
  const privateKey = runSync(current.wg, ['genkey'])
  const publicKey = runSync(current.wg, ['pubkey'], { input: privateKey + '\n' })
  identityState = { ...emptyIdentity(), runtime: current, privateKey, publicKey, virtualIp: String(options.virtualIp), subnetCidr: String(options.subnetCidr), server: { host: String(server.host), port: Number(server.port), publicKey: String(server.publicKey) } }
  const configPath = writeInitialConfig()
  try {
    await runTunnelCommand(current.wireguard, ['/uninstalltunnelservice', tunnelName], { ignoreFailure: true })
    await runTunnelCommand(current.wireguard, ['/installtunnelservice', configPath])
  } catch (error) {
    // Do not expose a half-initialized identity to the next game attempt.
    try { fs.unlinkSync(configPath) } catch {}
    identityState = emptyIdentity()
    throw error
  }
  return { ...status(), available: true, adapterReady: true, publicKey }
}

function chooseUdpPort() { return new Promise((resolve, reject) => { const socket = dgram.createSocket('udp4'); socket.once('error', reject); socket.bind(0, '127.0.0.1', () => { const port = Number(socket.address().port) || 0; socket.close(() => resolve(port)) }) }) }
function setTransport(path, directState, summary) {
  const next = { path, directState, summary }
  if (transportState.path === next.path && transportState.directState === next.directState && transportState.summary === next.summary) return
  transportState = next
  for (const listener of [...transportListeners]) { try { listener(next) } catch {} }
}
function parseGamePeerLine(line) {
  const value = String(line || '').trim()
  if (!value.startsWith('GAME_PEER ')) return null
  const parts = value.slice(10).split('|')
  if (parts.length !== 4) return null
  const [logicalIp, sourcePortText, targetPortText, generationText] = parts
  const octets = logicalIp.split('.')
  const sourcePort = Number(sourcePortText)
  const targetPort = Number(targetPortText)
  const generation = Number(generationText)
  if (octets.length !== 4 || octets.some((part) => !/^\d{1,3}$/.test(part) || Number(part) > 255)) return null
  if (!/^\d{1,5}$/.test(sourcePortText) || !/^\d{1,5}$/.test(targetPortText) ||
      sourcePort < 1 || sourcePort > 65535 || targetPort < 1 || targetPort > 65535) return null
  if (!/^\d+$/.test(generationText) || !Number.isSafeInteger(generation) || generation < 1) return null
  return { logicalIp, transactionKey: `${logicalIp}|${sourcePort}|${targetPort}|${generation}` }
}
function handleBridgeLine(line) {
  const value = String(line || '').trim()
  if (!value) return
  if (value === 'STATE connected') setTransport('direct', 'connected', '直连')
  else if (value === 'STATE failed') setTransport('relay', 'failed', '中继')
  else if (value === 'STATE disconnected') setTransport('relay', 'disconnected', '中继')
  const gamePeer = parseGamePeerLine(value)
  if (gamePeer) {
    for (const listener of [...gamePeerListeners]) { try { listener(gamePeer) } catch {} }
  }
  for (const waiter of [...bridgeWaiters]) waiter(value)
}

async function prepareGame() {
  const current = status()
  if (!current.available || !identityState.configPath) return { ready: false, agentPort: 0, hookPort: 0, message: current.message }
  if (bridgeProcess && bridgeProcess.exitCode === null && identityState.agentPort && identityState.hookPort) return { ready: true, agentPort: identityState.agentPort, hookPort: identityState.hookPort, message: '网卡数据通道已准备' }
  const agentPort = await chooseUdpPort(); let hookPort = await chooseUdpPort(); while (hookPort === agentPort) hookPort = await chooseUdpPort()
  let readyResolve
  let readyReject
  const readyPromise = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject })
  const ready = (line) => { if (line.startsWith('READY ')) { bridgeWaiters.delete(ready); readyResolve() } }
  bridgeWaiters.add(ready)
  const spawnedProcess = spawn(current.bridge, ['--agent-port', String(agentPort), '--hook-port', String(hookPort), '--virtual-ip', identityState.virtualIp, '--data-port', '51830'], { windowsHide: true, stdio: ['ignore', 'pipe', 'ignore'] })
  bridgeProcess = spawnedProcess
  bridgeLineBuffer = ''
  spawnedProcess.stdout.on('data', (chunk) => { bridgeLineBuffer += chunk.toString('utf8'); let newline; while ((newline = bridgeLineBuffer.indexOf('\n')) >= 0) { handleBridgeLine(bridgeLineBuffer.slice(0, newline)); bridgeLineBuffer = bridgeLineBuffer.slice(newline + 1) } })
  const failStart = (error) => {
    clearTimeout(timer)
    bridgeWaiters.delete(ready)
    if (bridgeProcess === spawnedProcess) {
      try { if (spawnedProcess.exitCode === null) spawnedProcess.kill() } catch {}
      bridgeProcess = null
      identityState.agentPort = 0
      identityState.hookPort = 0
    }
    setTransport('relay', 'failed', '中继')
    readyReject(error)
  }
  const timer = setTimeout(() => failStart(new Error('网卡数据通道启动超时')), 2500)
  spawnedProcess.once('error', failStart)
  spawnedProcess.once('exit', (code) => failStart(new Error(`网卡数据通道退出：${code}`)))
  await readyPromise
  clearTimeout(timer)
  identityState.agentPort = agentPort; identityState.hookPort = hookPort
  setTransport('pending', 'ready', '连接中')
  return { ready: true, agentPort, hookPort, message: '网卡数据通道已准备' }
}

async function addPeer(peer) {
  const peerEndpoint = endpoint(peer.endpointHost, peer.endpointPort)
  if (!peer.publicKey || !peer.virtualIp || !peerEndpoint) throw new Error('WireGuard 对手信息不完整')
  await runTunnelCommand(identityState.runtime.wg, ['set', tunnelName, 'peer', String(peer.publicKey), 'allowed-ips', `${peer.virtualIp}/32`, 'endpoint', peerEndpoint, 'persistent-keepalive', '5'])
}

function sendBridgeTarget(peerIp) { return new Promise((resolve, reject) => { const socket = dgram.createSocket('udp4'); socket.send(Buffer.from('TARGET ' + peerIp), identityState.agentPort, '127.0.0.1', (error) => { socket.close(); error ? reject(error) : resolve() }) }) }

async function connectPeer(options = {}) {
  const current = status(); const peer = options.peer || {}
  if (!current.available || !identityState.configPath) { setTransport('relay', 'unavailable', '中继'); return { path: 'relay', summary: '网卡不可用，当前使用中继' } }
  if (!identityState.agentPort || !identityState.hookPort || !bridgeProcess || bridgeProcess.exitCode !== null) { setTransport('relay', 'unavailable', '中继'); return { path: 'relay', summary: '网卡数据通道未启动，当前使用中继' } }
  try {
    await addPeer(peer)
    setTransport('pending', 'checking', '连接中')
    let connectedResolve
    let connectedReject
    const connectedPromise = new Promise((resolve, reject) => { connectedResolve = resolve; connectedReject = reject })
    const waiter = (line) => {
      if (line === 'STATE connected') { bridgeWaiters.delete(waiter); connectedResolve() }
      else if (line === 'STATE failed' || line === 'STATE disconnected') { bridgeWaiters.delete(waiter); connectedReject(new Error('网卡直连未连通')) }
    }
    bridgeWaiters.add(waiter)
    await sendBridgeTarget(peer.virtualIp)
    const timer = setTimeout(() => { bridgeWaiters.delete(waiter); connectedReject(new Error('网卡直连握手超时')) }, 12000)
    try { await connectedPromise } finally { clearTimeout(timer); bridgeWaiters.delete(waiter) }
    setTransport('direct', 'connected', '直连')
    return { path: 'direct', summary: '直连' }
  } catch {
    setTransport('relay', 'failed', '中继')
    return { path: 'relay', summary: '中继' }
  }
}

async function clearPeer() {
  if (!identityState.runtime?.wg || !identityState.configPath) return { cleared: true }
  // A match peer is short lived. Keep the room adapter and its server peer
  // alive for the next game, but remove the old opponent route.
  try {
    const output = runSync(identityState.runtime.wg, ['show', tunnelName, 'peers'])
    const serverKey = String(identityState.server?.publicKey || '')
    for (const key of output.split(/\r?\n/).map((value) => value.trim()).filter(Boolean)) {
      if (key !== serverKey) await runTunnelCommand(identityState.runtime.wg, ['set', tunnelName, 'peer', key, 'remove'], { ignoreFailure: true })
    }
  } catch {}
  if (identityState.agentPort && bridgeProcess && bridgeProcess.exitCode === null) {
    try { await sendBridgeTarget('') } catch {}
  }
  setTransport('relay', 'idle', '中继')
  return { cleared: true }
}

async function disconnect() {
  for (const waiter of [...bridgeWaiters]) { try { waiter('STATE disconnected') } catch {} }
  bridgeWaiters.clear(); if (bridgeProcess && bridgeProcess.exitCode === null) bridgeProcess.kill(); bridgeProcess = null
  if (identityState.runtime?.wireguard) {
    await runTunnelCommand(identityState.runtime.wireguard, ['/uninstalltunnelservice', tunnelName], { ignoreFailure: true })
  }
  if (identityState.configPath) { try { fs.unlinkSync(identityState.configPath) } catch {} }
  identityState = emptyIdentity()
  setTransport('relay', 'idle', '中继')
  return { stopped: true }
}

function identity() { return { publicKey: identityState.publicKey, virtualIp: identityState.virtualIp } }
function transportStatus() { return transportState }
function onTransportChange(listener) {
  if (typeof listener !== 'function') return () => {}
  transportListeners.add(listener)
  return () => transportListeners.delete(listener)
}
function onGamePeer(listener) {
  if (typeof listener !== 'function') return () => {}
  gamePeerListeners.add(listener)
  return () => gamePeerListeners.delete(listener)
}
module.exports = { status, prepare, prepareGame, connectPeer, clearPeer, disconnect, identity, transportStatus, onTransportChange, onGamePeer, serverVirtualIp, parseGamePeerLine, locateWireGuardInstaller }
