const fs = require('node:fs')
const dgram = require('node:dgram')
const os = require('node:os')
const path = require('node:path')
const { spawn } = require('node:child_process')
const { formatProcessExitCode, inspectVpnNetwork, isIPv4InCIDR, runPowerShell, runProcess, waitForVpnNetwork } = require('./tap-network.cjs')
const { ensureRoomFirewall } = require('./tap-firewall.cjs')

const DEFAULT_HOST = '8.155.145.132'
const DEFAULT_PORT = 22222
const TAP_NAME = 'TAP-Windows Adapter V9'
const WEL_TAP_NAME = /^(?:WEL Virtual LAN|WEL TAP|TAP-Windows Adapter V9|OpenVPN TAP-Windows6|以太网|Ethernet|本地连接|Local Area Connection)(?: \d+| #\d+)?$/i
const TAP_DESCRIPTION = /(?:TAP-Windows Adapter|OpenVPN TAP-Windows|WEL TAP|WEL Virtual LAN)/i
const N2N_PROGRESS = /(?:supernode|register|edge|tuntap|wintap|tap|peer|packet|created local tap|successfully joined)/i
const EDGE_MANAGEMENT_PORT = 5645
const EDGE_MANAGEMENT_TIMEOUT_MS = 700
const CONNECT_TIMEOUT_MS = 45000
const CONNECT_MAX_ATTEMPTS = 4
const APP_DATA_DIRECTORY = path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'WELPlatform')
const LOG_DIRECTORY = path.join(APP_DATA_DIRECTORY, 'logs')
const TAP_STATE_PATH = path.join(APP_DATA_DIRECTORY, 'tap-adapter.json')
const INSTALLER_TAP_STATE_PATH = path.join(process.env.PROGRAMDATA || 'C:\\ProgramData', 'WELPlatform', 'tap-create.txt')
const STOP_TIMEOUT_MS = 3000
const NETWORK_ADAPTER_CLASS_KEY = 'HKLM\\SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}'
const NETWORK_CONNECTIONS_KEY = 'HKLM\\SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}'

let connection = null
let preparedTap = null
let manualRoomRoute = null

function runtimeCandidates() {
  const resources = process.resourcesPath || ''
  return [
    path.join(resources, 'welhelper', 'edge.exe'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'edge.exe'),
    path.join(__dirname, '..', 'build', 'x86', 'edge.exe'),
    'C:\\Program Files\\WEL\\welhelper\\edge.exe',
  ]
}

function locateEdge() {
  return runtimeCandidates().find((candidate) => fs.existsSync(candidate)) || null
}

function tapInstallerCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'weltap.exe'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'weltap.exe'),
    path.join(__dirname, '..', 'resources', 'tap', 'weltap.exe'),
  ].filter(Boolean)
}

function locateTapInstaller() {
  return tapInstallerCandidates().find((candidate) => fs.existsSync(candidate)) || null
}

function tapctlCandidates() {
  return [
    path.join(process.resourcesPath || '', 'welhelper', 'tapctl.exe'),
    path.join(__dirname, '..', 'resources', 'welhelper', 'tapctl.exe'),
    path.join(process.resourcesPath || '', 'welhelper', 'tapctl.exe'),
    path.join(__dirname, '..', 'resources', 'tap', 'tapctl.exe'),
  ].filter(Boolean)
}

function locateTapctl() {
  return tapctlCandidates().find((candidate) => fs.existsSync(candidate)) || null
}

function parseTapctlList(output) {
  return String(output || '')
    .replace(/\r/g, '')
    .split('\n')
    .map((line) => {
      const match = line.match(/\{([0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12})\}/i)
      if (!match) return null
      const name = line
        .slice((match.index || 0) + match[0].length)
        .trim()
        .replace(/^['"]|['"]$/g, '')
      // tapctl enumerates TAP devices. The GUID is authoritative; a missing
      // or localized connection name must not make an existing device look
      // absent and trigger another driver installation.
      // Keep the registry's original casing because n2n 3.0 compares the
      // Windows adapter ID with strcmp() when opening the device.
      return {
        guid: `{${match[1]}}`,
        name: name || TAP_NAME,
        connectionName: name || null,
        description: null,
        isTap: true,
      }
    })
    .filter(Boolean)
}

function decodeBase64Field(value) {
  if (!value) return ''
  try {
    return Buffer.from(String(value), 'base64').toString('utf8')
  } catch {
    return ''
  }
}

function parseWmiTapAdapters(output) {
  return String(output || '')
    .replace(/\r/g, '')
    .split('\n')
    .map((line) => {
      const fields = line.trim().split('|')
      if (fields.length < 3) return null
      const guid = decodeBase64Field(fields[0]).trim()
      const connectionName = decodeBase64Field(fields[1]).trim()
      const description = decodeBase64Field(fields[2]).trim()
      if (!guid) return null
      const exactGuid = extractTapGuid(`{${guid.replace(/[{}]/g, '')}}`)
      if (!exactGuid) return null
      return {
        guid: exactGuid,
        name: connectionName || description || TAP_NAME,
        connectionName: connectionName || null,
        description: description || null,
        isTap: true,
      }
    })
    .filter(Boolean)
}

function parseRegistryConnectionNames(output) {
  const sections = []
  let current = null
  for (const line of String(output || '').replace(/\r/g, '').split('\n')) {
    const trimmed = line.trim()
    if (/^HKEY_/i.test(trimmed)) {
      if (current) sections.push(current)
      current = { key: trimmed, values: {} }
      continue
    }
    if (!current) continue
    const value = line.match(/^\s+([^\s]+)\s+REG_[A-Z0-9_]+\s+(.*)$/i)
    if (value) current.values[value[1].toLowerCase()] = value[2].trim()
  }
  if (current) sections.push(current)

  const connections = new Map()
  for (const { key, values } of sections) {
    const match = key.match(/\\\{([0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12})\}\\connection$/i)
    const name = values.name?.replace(/^['"]|['"]$/g, '').trim()
    if (match && name) connections.set(match[1].toLowerCase(), name)
  }
  return connections
}

function parseRegistryTapAdapters(output, connectionNames = null) {
  const sections = []
  let current = null
  for (const line of String(output || '').replace(/\r/g, '').split('\n')) {
    const trimmed = line.trim()
    if (/^HKEY_/i.test(trimmed)) {
      if (current) sections.push(current)
      current = { key: trimmed, values: {} }
      continue
    }
    if (!current) continue
    const value = line.match(/^\s+([^\s]+)\s+REG_[A-Z0-9_]+\s+(.*)$/i)
    if (value) current.values[value[1].toLowerCase()] = value[2].trim()
  }
  if (current) sections.push(current)

  return sections.map(({ values }) => {
    const component = values.componentid || values.service || ''
    const description = values.driverdesc || ''
    const isTap = /(?:^|\\)tap0?(?:801|901)$/i.test(component)
      || /TAP-Windows Adapter|OpenVPN TAP-Windows|WEL TAP/i.test(description)
    const guid = extractTapGuid(values.netcfginstanceid)
    const connectionName = guid ? connectionNames?.get(guid.slice(1, -1).toLowerCase()) : null
    if (!isTap || !guid || (connectionNames && !connectionName)) return null
    return {
      guid,
      name: connectionName || description || TAP_NAME,
      connectionName: connectionName || null,
      description: description || null,
      isTap: true,
    }
  }).filter(Boolean)
}

function isWelTapAdapter(name) {
  const normalized = String(name || '').trim()
  return WEL_TAP_NAME.test(normalized)
}

function isTapAdapter(adapter) {
  if (!adapter) return false
  if (adapter.isTap === true) return true
  // A localized connection alias such as "以太网 2" is not enough to prove
  // that an adapter is TAP; ordinary physical adapters can use the same alias.
  return TAP_DESCRIPTION.test(String(adapter.description || '')) ||
    TAP_DESCRIPTION.test(String(adapter.name || ''))
}

function extractTapGuid(output) {
  const match = String(output || '').match(/\{([0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12})\}/i)
  return match ? `{${match[1]}}` : null
}

function parseTapGuid(output) {
  return extractTapGuid(output)?.toLowerCase() || null
}

function readRememberedTapGuid() {
  try {
    const installerGuid = parseTapGuid(fs.readFileSync(INSTALLER_TAP_STATE_PATH, 'utf8'))
    if (installerGuid) return installerGuid
  } catch {}
  try {
    const state = JSON.parse(fs.readFileSync(TAP_STATE_PATH, 'utf8'))
    return parseTapGuid(state.guid)
  } catch { return null }
}

function rememberTapAdapter(adapter) {
  try {
    fs.mkdirSync(APP_DATA_DIRECTORY, { recursive: true })
    fs.writeFileSync(TAP_STATE_PATH, JSON.stringify({ guid: adapter.guid, name: adapter.name }), 'utf8')
  } catch {
    // Remembering is an optimization; the current connection can still use
    // the adapter GUID returned by tapctl.
  }
}

async function ensureTapReady(adapter) {
  const enabledAdapter = await ensureTapEnabled(adapter)
  if (!enabledAdapter) return null
  rememberTapAdapter(enabledAdapter)
  // n2n on Windows matches -d against the TAP GUID or the network
  // connection name (for example "以太网 2"), not the driver description.
  const tapName = enabledAdapter.connectionName || enabledAdapter.name || TAP_NAME
  preparedTap = {
    tapName,
    tapNode: enabledAdapter.guid,
    tapGuid: enabledAdapter.guid,
    tapDescription: enabledAdapter.description || null,
  }
  return preparedTap
}

function selectWelTapAdapter(adapters, excludedGuids = new Set()) {
  const candidates = (adapters || []).filter(({ guid }) => {
    const normalized = parseTapGuid(guid)
    return Boolean(normalized) && !excludedGuids.has(normalized)
  })
  const owned = candidates.filter(isTapAdapter)
  return owned.find(({ description, name }) => String(description || name || '').toLowerCase() === TAP_NAME.toLowerCase())
    || owned.find(({ description, name }) => /^(?:WEL Virtual LAN|WEL TAP)(?: \d+)?$/i.test(String(description || name || '')))
    || owned.find(({ connectionName, name }) => /^(?:以太网|Ethernet|本地连接|Local Area Connection)(?: \d+| #\d+)?$/i.test(String(connectionName || name || '')))
    || owned[0]
    || null
}

function runTapctl(executable, args, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    const child = spawn(executable, args, { windowsHide: true })
    const output = []
    const timer = setTimeout(() => {
      try { child.kill() } catch {}
      reject(new Error(`tapctl ${args[0]} 执行超时`))
    }, timeoutMs)
    child.stdout.on('data', (chunk) => output.push(chunk))
    child.stderr.on('data', (chunk) => output.push(chunk))
    child.once('error', (error) => {
      clearTimeout(timer)
      reject(error)
    })
    child.once('close', (code) => {
      clearTimeout(timer)
      const detail = Buffer.concat(output).toString('utf8').trim()
      if (code === 0) resolve(detail)
      else reject(new Error(`tapctl ${args[0]} 失败（代码 ${code ?? '未知'}）${detail ? `：${detail}` : ''}`))
    })
  })
}

async function listTapAdapters(tapctl) {
  try {
    const adapters = parseTapctlList(await runTapctl(tapctl, ['list']))
    if (adapters.length > 0) return await enrichTapAdapters(adapters)
  } catch {
    // Win7 can have a healthy TAP driver while tapctl cannot enumerate it.
  }
  try {
    const adapters = await listTapAdaptersFromRegistry()
    if (adapters.length > 0) return adapters
  } catch {
    // Restricted Windows policies can deny access to the adapter class key.
  }
  try {
    return await listTapAdaptersFromWmi()
  } catch {
    // Detection failure must not prevent a green edition from running its
    // bundled TAP installer on a machine that has no adapter yet.
    return []
  }
}

async function enrichTapAdapters(adapters) {
  // tapctl identifies the device by GUID, while n2n needs the Windows
  // connection name when -d is used. Merge the two views so a localized name
  // such as "以太网 2" is not confused with the driver description.
  let metadata = []
  try {
    metadata = await listTapAdaptersFromRegistry()
  } catch {
    try { metadata = await listTapAdaptersFromWmi() } catch {}
  }
  if (!metadata.length) return adapters
  const byGuid = new Map(metadata.map((adapter) => [parseTapGuid(adapter.guid), adapter]))
  return adapters.map((adapter) => {
    const detail = byGuid.get(parseTapGuid(adapter.guid))
    return detail
      ? { ...adapter, ...detail, isTap: true }
      : adapter
  })
}

async function listTapAdaptersFromRegistry() {
  const [classOutput, connectionOutput] = await Promise.all([
    runProcess('reg.exe', ['query', NETWORK_ADAPTER_CLASS_KEY, '/s'], 10000),
    runProcess('reg.exe', ['query', NETWORK_CONNECTIONS_KEY, '/s'], 10000),
  ])
  return parseRegistryTapAdapters(classOutput, parseRegistryConnectionNames(connectionOutput))
}

async function listTapAdaptersFromWmi() {
  const output = await runPowerShell(`
function Encode-Value($value) {
  if ($null -eq $value) { return '' }
  [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([string]$value))
}

Get-WmiObject -Class Win32_NetworkAdapter -ErrorAction SilentlyContinue |
  Where-Object {
    $_.GUID -and (
      $_.ServiceName -match '^(?i:tap0?(801|901))$' -or
      $_.PNPDeviceID -match '(?i)TAP0?(801|901)' -or
      $_.Name -match '(?i)TAP-Windows Adapter|OpenVPN TAP-Windows|WEL TAP'
    )
  } |
  ForEach-Object {
    [Console]::Out.WriteLine(
      (Encode-Value $_.GUID) + '|' +
      (Encode-Value $_.NetConnectionID) + '|' +
      (Encode-Value $_.Name)
    )
  }
`, 8000)
  return parseWmiTapAdapters(output)
}

async function ensureTapEnabled(adapter) {
  if (!adapter) return adapter
  const guid = adapter.guid
  const bareGuid = guid.replace(/[{}]/g, '')
  try {
    const state = await runPowerShell(`
$guid = '${bareGuid}'
$adapter = Get-WmiObject Win32_NetworkAdapter -ErrorAction SilentlyContinue |
  Where-Object { $_.GUID -and $_.GUID.Trim('{}') -ieq $guid } |
  Select-Object -First 1
if ($null -eq $adapter) { [Console]::Out.WriteLine('MISSING'); exit 0 }
$errorCode = 0
try { $errorCode = [int]$adapter.ConfigManagerErrorCode } catch {}
if ($errorCode -ne 0) { [Console]::Out.WriteLine('ERROR'); exit 0 }
$enabled = $adapter.NetEnabled
# NetEnabled can be null on older Windows/WMI even when the device is healthy.
# Only explicitly disabled adapters need an Enable() call.
if ($enabled -eq $false) {
  $result = $adapter.Enable()
  $returnValue = [int]$result.ReturnValue
  if (@(0, 1) -notcontains $returnValue) { [Console]::Out.WriteLine('ERROR'); exit 0 }
  Start-Sleep -Milliseconds 500
}
[Console]::Out.WriteLine('READY')
`, 6000)
    return /(^|\r?\n)READY(\r?\n|$)/.test(state) ? adapter : null
  } catch {
    // A failed health query is not proof that the GUID is usable. Let the
    // caller try another TAP candidate instead of handing a broken device to n2n.
    return null
  }
}

async function prepare(excludedGuids = new Set(), repairState = { freshCreated: false }) {
  const current = status()
  if (!current.ready) throw new Error(current.message)
  if (process.platform !== 'win32') return current
  if (preparedTap?.tapNode) {
    return { ...current, adapterReady: true, ...preparedTap }
  }

  const tapctl = locateTapctl()
  if (!tapctl) throw new Error('未检测到 WEL 虚拟网卡管理组件，请重新安装客户端')

  let adapters = await listTapAdapters(tapctl)
  const existingTapGuids = new Set(adapters.map(({ guid }) => parseTapGuid(guid)).filter(Boolean))
  const rememberedGuid = readRememberedTapGuid()
  const orderedAdapters = []
  const rememberedAdapter = rememberedGuid
    ? adapters.find(({ guid }) => guid.toLowerCase() === rememberedGuid && !excludedGuids.has(rememberedGuid))
    : null
  if (rememberedAdapter) orderedAdapters.push(rememberedAdapter)
  const preferredAdapter = selectWelTapAdapter(adapters, excludedGuids)
  if (preferredAdapter && !orderedAdapters.some(({ guid }) => guid.toLowerCase() === preferredAdapter.guid.toLowerCase())) {
    orderedAdapters.push(preferredAdapter)
  }
  for (const adapter of adapters) {
    if (!excludedGuids.has(adapter.guid.toLowerCase()) &&
        !orderedAdapters.some(({ guid }) => guid.toLowerCase() === adapter.guid.toLowerCase())) {
      orderedAdapters.push(adapter)
    }
  }
  for (const adapter of orderedAdapters) {
    const prepared = await ensureTapReady(adapter)
    if (prepared) return { ...current, adapterReady: true, ...prepared }
  }

  // A device that was enumerated but cannot be opened is not repaired by
  // reinstalling the whole driver. Create one fresh TAP adapter and leave
  // every existing adapter untouched. Limit this repair to once per run so a
  // persistent driver failure cannot create an unbounded number of adapters.
  if (excludedGuids.size > 0 || existingTapGuids.size > 0) {
    if (repairState.freshCreated) {
      throw new Error('网卡已存在，但当前设备无法被网络组件打开；请在设备管理器中修复网卡驱动')
    }
    repairState.freshCreated = true
    const knownGuids = new Set([...existingTapGuids, ...excludedGuids])
    try { await runTapctl(tapctl, ['create', '--hwid', 'tap0901']) } catch {}
    const freshAdapter = await waitForNewTapAdapter(tapctl, knownGuids)
    if (freshAdapter) {
      const prepared = await ensureTapReady(freshAdapter)
      if (prepared) return { ...current, adapterReady: true, ...prepared }
    }
    throw new Error('检测到网卡，但所有设备实例均不可用；请在设备管理器中修复网卡驱动')
  }

  const installer = locateTapInstaller()
  if (!installer) throw new Error('未找到网卡，且当前安装包缺少网卡驱动文件')

  await installBundledTapDriver(installer)
  adapters = await waitForTapAdapter(tapctl)
  let installedAdapter = selectWelTapAdapter(adapters, excludedGuids)
  if (!installedAdapter) {
    await runTapctl(tapctl, ['create', '--hwid', 'tap0901'])
    adapters = await waitForTapAdapter(tapctl)
    installedAdapter = selectWelTapAdapter(adapters, excludedGuids)
  }
  if (!installedAdapter) throw new Error('网卡驱动安装后仍未检测到网卡，请重启 Windows 后重试')
  const installed = await ensureTapReady(installedAdapter)
  if (!installed) throw new Error('检测到网卡，但驱动设备状态异常，请在设备管理器中修复网卡驱动')
  return { ...current, adapterReady: true, ...installed }
}

function installBundledTapDriver(installer) {
  return new Promise((resolve, reject) => {
    const child = spawn(installer, ['/S'], { windowsHide: true })
    child.once('error', reject)
    child.once('close', (code) => {
      if ([0, 1641, 3010].includes(Number(code))) resolve()
      else reject(new Error(`网卡驱动安装失败（代码 ${code ?? '未知'}）`))
    })
  })
}

async function waitForTapAdapter(tapctl) {
  const deadline = Date.now() + 15000
  while (Date.now() < deadline) {
    const adapters = await listTapAdapters(tapctl)
    if (adapters.length > 0) return adapters
    await wait(500)
  }
  return []
}

async function waitForNewTapAdapter(tapctl, knownGuids) {
  const deadline = Date.now() + 15000
  while (Date.now() < deadline) {
    const adapters = await listTapAdapters(tapctl)
    const created = adapters.find(({ guid }) => !knownGuids.has(parseTapGuid(guid)))
    if (created) return created
    await wait(500)
  }
  return null
}

function safeFilePart(value) {
  return String(value || '').replace(/[^A-Za-z0-9._-]/g, '_').slice(0, 96)
}

function ensureRuntimeDirectory() {
  const directory = path.join(os.homedir(), 'AppData', 'Local', 'WELPlatform', 'runtime')
  fs.mkdirSync(directory, { recursive: true })
  return directory
}

function ensureLogDirectory() {
  fs.mkdirSync(LOG_DIRECTORY, { recursive: true })
  return LOG_DIRECTORY
}

function recentOutput(output, limit = 2000) {
  return output.join('').replace(/\r?\n/g, '\n').trim().slice(-limit)
}

function readRecentLog(filePath, limit = 2000) {
  try {
    return fs.existsSync(filePath) ? fs.readFileSync(filePath, 'utf8').replace(/\r?\n/g, '\n').trim().slice(-limit) : ''
  } catch {
    return ''
  }
}

function n2nExitReason(code) {
  const formatted = formatProcessExitCode(code)
  const hint = (Number(code) >>> 0) === 0xC0000135 ? '，缺少运行库 DLL' : ''
  return `网络组件进程提前退出（代码 ${formatted}${hint}）`
}

function transportConfigPath(filePath) {
  return String(filePath || '').replace(/\\/g, '/')
}

function subnetMaskFromCidr(cidr) {
  const prefix = Number(String(cidr || '').split('/')[1])
  if (!Number.isInteger(prefix) || prefix < 0 || prefix > 32) return '255.255.255.0'
  const mask = prefix === 0 ? 0 : (0xffffffff << (32 - prefix)) >>> 0
  return [24, 16, 8, 0].map((shift) => (mask >>> shift) & 255).join('.')
}

// 兜底：显式把房间网段路由钉到 TAP 网卡，防止 ZeroTier/Radmin 等第三方虚拟网卡
// 以更低 metric 抢占路由，导致出站广播/单播走错网卡（表现为"我能搜到别人，别人搜不到我"）。
// 对正常玩家是 no-op：TAP 网卡本就有该网段直连路由，change 只是等价地再显式指回 TAP。
async function ensureRoomRoute(subnetCidr, tapIP, interfaceIndex) {
  if (process.platform !== 'win32') return
  const network = String(subnetCidr || '').split('/')[0]
  const mask = subnetMaskFromCidr(subnetCidr)
  const ifIndex = Number(interfaceIndex)
  if (!network || !tapIP || !Number.isInteger(ifIndex) || ifIndex <= 0) return
  const args = [network, 'mask', mask, tapIP, 'metric', '1', 'if', String(ifIndex)]
  try {
    await runProcess('route', ['change', ...args], 5000)
    return { network, mask, ifIndex, added: false }
  } catch {
    try {
      await runProcess('route', ['add', ...args], 5000)
      manualRoomRoute = { network, mask, ifIndex }
      return { network, mask, ifIndex, added: true }
    } catch {
      // 路由修正失败不阻塞已建立的 n2n 连接，保持原有行为
    }
  }
  return null
}

async function removeRoomRoute() {
  const route = manualRoomRoute
  manualRoomRoute = null
  if (process.platform !== 'win32' || !route) return
  try {
    await runProcess('route', ['delete', route.network, 'mask', route.mask, 'if', String(route.ifIndex)], 5000)
  } catch {
    // Best effort: Windows may remove the route automatically with the adapter.
  }
}

function prefixFromCidr(cidr) {
  const prefix = Number(String(cidr || '').split('/')[1])
  if (!Number.isInteger(prefix) || prefix < 0 || prefix > 32) return 24
  return prefix
}

function buildConfig({ host, port, username, roomID, subnetCidr, virtualIP, community, transportKey, tapName, transportBindIP }) {
  const runtime = ensureRuntimeDirectory()
  const prefix = `room-${safeFilePart(roomID)}-${safeFilePart(username)}`
  const configPath = path.join(runtime, `${prefix}.n2n.txt`)
  const logPath = path.join(ensureLogDirectory(), `${prefix}.n2n.log`)
  fs.writeFileSync(logPath, '', { encoding: 'utf8' })
  const config = [
    `community=${community}`,
    `supernode=${host}:${port}`,
    `virtual_ip=${virtualIP}`,
    `netmask=${subnetMaskFromCidr(subnetCidr)}`,
    `tap_name=${tapName || ''}`,
    `transport_bind_ip=${transportBindIP || 'auto'}`,
    `identity=${username}`,
    `room_id=${roomID}`,
  ].join('\r\n') + '\r\n'
  fs.writeFileSync(configPath, config, { encoding: 'utf8', mode: 0o600 })
  return { configPath, logPath, community, transportKey }
}

function removeFiles(files) {
  for (const file of files || []) {
    try { fs.rmSync(file, { force: true }) } catch { /* temporary credential cleanup */ }
  }
}

function waitForProcessExit(process, timeoutMs) {
  if (process.exitCode !== null || process.killed) return Promise.resolve(true)
  return new Promise((resolve) => {
    let settled = false
    const finish = (value) => {
      if (settled) return
      settled = true
      resolve(value)
    }
    const timer = setTimeout(() => finish(false), timeoutMs)
    process.once('close', () => {
      clearTimeout(timer)
      finish(true)
    })
  })
}

async function stopConnection() {
  if (!connection) {
    await removeRoomRoute()
    return
  }
  const current = connection
  connection = null
  try {
    try { current.process.kill() } catch {}
    const exited = await waitForProcessExit(current.process, STOP_TIMEOUT_MS)
    if (!exited) try { current.process.kill('SIGKILL') } catch {}
  } finally {
    await removeRoomRoute()
    removeFiles(current.temporaryFiles)
  }
}

function status() {
  const executable = locateEdge()
  const tapctl = locateTapctl()
  const ready = Boolean(executable && tapctl)
  return {
    ready,
    openvpnInstalled: Boolean(executable),
    n2nInstalled: Boolean(executable),
    tapName: TAP_NAME,
    message: ready
      ? '联机组件已准备好'
      : '未检测到网络组件，请重新运行完整安装包。',
  }
}

function activeNetwork() {
  return connection?.network ? { ...connection.network } : null
}

function parsePingSummary(host, output) {
  const text = String(output || '').replace(/\r?\n/g, '\n')
  const reachable = /TTL=/i.test(text)
  const loss = text.match(/(\d+)%\s*(?:loss|丢失)/i)?.[1]
  const average = text.match(/(?:Average|平均)\s*[=<]\s*(\d+ms)/i)?.[1]
    || text.match(/平均\s*=\s*(\d+ms)/)?.[1]
  const parts = [reachable ? '可达' : '不可达']
  if (average) parts.push(`平均 ${average}`)
  if (loss !== undefined) parts.push(`丢包 ${loss}%`)
  return { host, reachable, summary: parts.join('，') }
}

function isValidIPv4(value) {
  const parts = String(value || '').trim().split('.').map(Number)
  return parts.length === 4 && parts.every((part) => Number.isInteger(part) && part >= 0 && part <= 255)
}

function pingHost(host) {
  const target = String(host || '').trim()
  if (!isValidIPv4(target)) return Promise.reject(new Error('Ping 地址不正确'))
  if (process.platform !== 'win32') return Promise.reject(new Error('房间 Ping 仅支持 Windows'))
  const network = activeNetwork()
  if (!network?.connected || !isValidIPv4(network.actualIp)) return Promise.reject(new Error('网卡尚未连接'))
  if (!isIPv4InCIDR(target, network.subnetCidr)) return Promise.reject(new Error('对手不在当前房间网段'))
  const ping = `${process.env.SystemRoot || 'C:\\Windows'}\\System32\\ping.exe`
  const source = network.actualIp
  const args = ['-n', '4', '-w', '1000']
  if (isValidIPv4(source)) args.push('-S', source)
  args.push(target)
  return new Promise((resolve) => {
    const child = spawn(ping, args, { windowsHide: true })
    const stdout = []
    const stderr = []
    child.stdout.on('data', (chunk) => stdout.push(chunk))
    child.stderr.on('data', (chunk) => stderr.push(chunk))
    child.once('error', (error) => resolve({ host: target, reachable: false, summary: `Ping 失败：${error.message}` }))
    child.once('close', () => {
      const output = Buffer.concat([...stdout, ...stderr]).toString('utf8')
      resolve(parsePingSummary(target, output))
    })
  })
}

function wait(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

function escapePowerShellString(value) {
  return String(value || '').replace(/'/g, "''")
}

async function findBestTransportIPv4(remoteHost) {
  if (process.platform !== 'win32') return null
  const target = escapePowerShellString(remoteHost || DEFAULT_HOST)
  try {
    const output = await runPowerShell(`
function Convert-IPv4($value) {
  $parts = ([string]$value).Split('.') | ForEach-Object { [uint64]$_ }
  if ($parts.Count -ne 4) { return $null }
  return ($parts[0] * 16777216) + ($parts[1] * 65536) + ($parts[2] * 256) + $parts[3]
}

$targetAddresses = @([System.Net.Dns]::GetHostAddresses('${target}') |
  Where-Object { $_.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork })
if ($targetAddresses.Count -eq 0) { exit 2 }
$targetNumber = Convert-IPv4 $targetAddresses[0].IPAddressToString

$routes = @(Get-WmiObject Win32_IP4RouteTable -ErrorAction Stop |
  Where-Object { $_.Destination -and $_.Mask -and $_.InterfaceIndex -gt 0 } |
  ForEach-Object {
    $destination = Convert-IPv4 $_.Destination
    $mask = Convert-IPv4 $_.Mask
    if ($null -eq $destination -or $null -eq $mask) { return }
    if (($targetNumber -band $mask) -ne ($destination -band $mask)) { return }
    $bits = $mask
    $prefix = 0
    while ($bits -gt 0) {
      $prefix += ($bits -band 1)
      $bits = $bits -shr 1
    }
    [PSCustomObject]@{
      InterfaceIndex = [int]$_.InterfaceIndex
      PrefixLength = $prefix
      Metric = [int]$_.Metric1
    }
  } | Sort-Object -Property @{Expression='PrefixLength';Descending=$true}, @{Expression='Metric';Ascending=$true})

foreach ($route in $routes) {
  $configs = @(Get-WmiObject Win32_NetworkAdapterConfiguration -ErrorAction SilentlyContinue |
    Where-Object { $_.InterfaceIndex -eq $route.InterfaceIndex -and $_.IPEnabled -and $_.DefaultIPGateway })
  foreach ($config in $configs) {
    $address = @($config.IPAddress | Where-Object { $_ -match '^\\d{1,3}(?:\\.\\d{1,3}){3}$' }) | Select-Object -First 1
    if ($address) { [Console]::Out.WriteLine($address); exit 0 }
  }
}
exit 3
`, 8000)
    return String(output || '').trim().split(/\r?\n/).find((line) => /^\d{1,3}(?:\.\d{1,3}){3}$/.test(line.trim())) || null
  } catch {
    return null
  }
}

function isRetryableConnectError(error) {
  return /连接超时：未获取虚拟 IP|TAP|adapter|网卡|CreateFile|DeviceIoControl/i.test(String(error?.message || error || ''))
}

async function stopStaleWelN2nProcesses() {
  if (process.platform !== 'win32') return
  try {
    await runPowerShell(`
$deadline = [DateTime]::UtcNow.AddSeconds(5)
do {
  $processes = @(Get-WmiObject Win32_Process -ErrorAction SilentlyContinue |
    Where-Object {
    ($_.Name -ieq 'edge.exe') -and
      ($_.CommandLine -like '*wel-room-*' -or $_.CommandLine -like '*WELPlatform*')
    })
  foreach ($process in $processes) { try { $process.Terminate() | Out-Null } catch {} }
  if ($processes.Count -eq 0) { exit 0 }
  Start-Sleep -Milliseconds 250
} while ([DateTime]::UtcNow -lt $deadline)
exit 2
`, 5000)
  } catch {
    // Best effort only. A normal connection attempt can still proceed.
  }
}

function n2nCommunity(roomID, community) {
  const value = String(community || '').trim()
  return value || `wel-room-${roomID}`
}

function buildEdgeArgs({ host, port, roomID, username, subnetCidr, virtualIP, community, transportKey, tapName, transportBindIP }) {
  if (!virtualIP) throw new Error('房间地址未分配，请重新进入房间')
  const args = [
    '-E',
    '-x', '1',
    '-c', n2nCommunity(roomID, community),
    '-l', `${host}:${port}`,
    '-a', `${virtualIP}/${prefixFromCidr(subnetCidr)}`,
    '-t', String(EDGE_MANAGEMENT_PORT),
  ]
  if (transportBindIP) args.push('-p', transportBindIP, '-e', transportBindIP)
  if (tapName) args.unshift('-d', tapName)
  if (transportKey) args.push('-k', transportKey)
  if (username) args.push('-I', username)
  return args
}

function classifyN2NPeers(rows) {
  const peers = Array.isArray(rows) ? rows.filter((row) => row && typeof row === 'object') : []
  const directPeers = peers.filter((peer) => String(peer.mode || '').toLowerCase() === 'p2p')
  const relayPeers = peers.filter((peer) => String(peer.mode || '').toLowerCase() === 'psp')
  if (directPeers.length > 0 && relayPeers.length > 0) return { path: 'mixed', peers: peers.length, directPeers: directPeers.length, relayPeers: relayPeers.length }
  if (directPeers.length > 0) return { path: 'direct', peers: peers.length, directPeers: directPeers.length, relayPeers: relayPeers.length }
  if (relayPeers.length > 0) return { path: 'relay', peers: peers.length, directPeers: 0, relayPeers: relayPeers.length }
  return { path: 'pending', peers: peers.length, directPeers: 0, relayPeers: 0 }
}

function queryEdgeManagement(command = 'edges', port = EDGE_MANAGEMENT_PORT, timeoutMs = EDGE_MANAGEMENT_TIMEOUT_MS) {
  return new Promise((resolve, reject) => {
    const socket = dgram.createSocket('udp4')
    const rows = []
    let settled = false
    let timer
    const finish = (error, value) => {
      if (settled) return
      settled = true
      if (timer) clearTimeout(timer)
      try { socket.close() } catch {}
      if (error) reject(error)
      else resolve(value)
    }
    socket.on('error', (error) => finish(error))
    socket.on('message', (message) => {
      for (const line of String(message || '').split(/\r?\n/).map((item) => item.trim()).filter(Boolean)) {
        let event
        try { event = JSON.parse(line) } catch { continue }
        if (event._type === 'error') return finish(new Error(String(event.error || 'n2n 管理接口返回错误')))
        if (event._type === 'row') rows.push(event)
        if (event._type === 'end') return finish(null, rows)
      }
    })
    timer = setTimeout(() => finish(new Error('n2n 管理接口响应超时')), timeoutMs)
    socket.bind(0, '127.0.0.1', () => {
      const payload = Buffer.from(`r 0 ${String(command || 'edges').trim()}\n`, 'utf8')
      socket.send(payload, 0, payload.length, Number(port) || EDGE_MANAGEMENT_PORT, '127.0.0.1', (error) => {
        if (error) finish(error)
      })
    })
  })
}

async function transportStatus() {
  if (!connection?.process || connection.process.exitCode !== null) {
    return { path: 'pending', peers: 0, directPeers: 0, relayPeers: 0, summary: '网络组件未连接' }
  }
  try {
    const result = classifyN2NPeers(await queryEdgeManagement('edges', connection.managementPort || EDGE_MANAGEMENT_PORT))
    const summary = result.path === 'direct'
      ? '直连'
      : result.path === 'relay'
        ? '中继'
        : result.path === 'mixed'
          ? '混合'
          : '连接中'
    return { ...result, summary }
  } catch {
    return { path: 'pending', peers: 0, directPeers: 0, relayPeers: 0, summary: '连接中' }
  }
}

async function connectAttempt({ executable, host, port, roomID, username, subnetCidr, virtualIP, community, transportKey, tapName, transportBindIP }) {
  await stopConnection()
  const files = buildConfig({
    host: host || DEFAULT_HOST,
    port: Number(port) || DEFAULT_PORT,
    username,
    roomID,
    subnetCidr,
    virtualIP,
    community,
    transportKey,
    tapName,
    transportBindIP,
  })
  const edgeArgs = buildEdgeArgs({
    host: host || DEFAULT_HOST,
    port: Number(port) || DEFAULT_PORT,
    roomID,
    username,
    subnetCidr,
    virtualIP,
    community,
    transportKey,
    tapName,
    transportBindIP,
  })
  const child = spawn(executable, edgeArgs, {
    windowsHide: true,
    env: { ...process.env, ...(transportKey ? { N2N_KEY: transportKey } : {}) },
  })
  const output = []
  let failed = ''
  let initialized = false
  child.stdout.on('data', (chunk) => output.push(chunk.toString()))
  child.stderr.on('data', (chunk) => output.push(chunk.toString()))
  child.once('error', (error) => { failed = error.message })
  child.once('close', (code) => {
    if (!initialized) failed = n2nExitReason(code)
    else if (connection?.process === child) {
      const current = connection
      connection = null
      removeFiles(current.temporaryFiles)
    }
  })
  connection = {
    process: child,
    managementPort: EDGE_MANAGEMENT_PORT,
    temporaryFiles: [files.configPath],
    logPath: files.logPath,
    network: null,
  }

  try {
    const startedAt = Date.now()
    while (Date.now() - startedAt < CONNECT_TIMEOUT_MS) {
      if (failed) break
      const liveOutput = recentOutput(output)
      if (liveOutput) {
        try { fs.appendFileSync(files.logPath, liveOutput + '\r\n', 'utf8') } catch {}
      }
      if (N2N_PROGRESS.test(liveOutput) || Date.now() - startedAt > 1000) {
        const network = await waitForVpnNetwork(subnetCidr, 3000)
        if (network.connected && (!virtualIP || network.actualIp === virtualIP)) {
          const inspectedNetwork = await inspectVpnNetwork(subnetCidr)
          if (failed || child.exitCode !== null) {
            throw new Error(failed || '网络组件在准备游戏网卡时退出')
          }
          if (connection?.process !== child) throw new Error('n2n 连接在准备游戏网络时已关闭')
          connection.network = inspectedNetwork
          initialized = true
          await ensureRoomRoute(subnetCidr, inspectedNetwork.actualIp, inspectedNetwork.interfaceIndex)
          return inspectedNetwork
        }
      }
      await wait(300)
    }
    const liveOutput = recentOutput(output)
    const fileOutput = readRecentLog(files.logPath)
    const reason = failed || '连接超时：未获取虚拟 IP'
    const detail = [reason, liveOutput || fileOutput].filter(Boolean).join('\n')
    throw new Error(`网络连接失败：${detail || '连接超时'}\n日志文件：${files.logPath}`)
  } catch (error) {
    await stopConnection()
    throw error
  }
}

async function connect({ host, port, roomID, username, subnetCidr, virtualIP, community, transportKey }) {
  const executable = locateEdge()
  if (!executable) throw new Error('未检测到网络组件，请重新运行完整安装包')
  if (!username || !roomID || !subnetCidr) throw new Error('房间凭据不完整')
  if (!virtualIP) throw new Error('房间地址未分配，请重新进入房间')

  await stopConnection()
  await stopStaleWelN2nProcesses()
  await wait(500)
  const failedTapGuids = new Set()
  const tapRepairState = { freshCreated: false }
  const prepared = await prepare(failedTapGuids, tapRepairState)
  let tapNode = prepared.tapNode
  const transportBindIP = await findBestTransportIPv4(host || DEFAULT_HOST)
  const firewall = await ensureRoomFirewall(executable, subnetCidr)

  let lastError = null
  for (let attempt = 1; attempt <= CONNECT_MAX_ATTEMPTS; attempt += 1) {
    try {
      // Pass the GUID to n2n. It is ASCII and is the authoritative Windows
      // adapter identity; the localized connection name is retained for
      // diagnostics and UI only.
      const network = await connectAttempt({ executable, host, port, roomID, username, subnetCidr, virtualIP, community, transportKey, tapName: tapNode, transportBindIP })
      const result = { ...network, warnings: firewall.warnings || [] }
      if (connection) connection.network = result
      return result
    } catch (error) {
      lastError = error
      if (attempt >= CONNECT_MAX_ATTEMPTS || !isRetryableConnectError(error)) throw error
      if (/Cannot find tap device/i.test(String(error?.message || error || ''))) {
        const failedGuid = parseTapGuid(tapNode)
        if (failedGuid) failedTapGuids.add(failedGuid)
        preparedTap = null
        const refreshed = await prepare(failedTapGuids, tapRepairState)
        tapNode = refreshed.tapNode
      }
      await wait(attempt * 1200)
    }
  }
  throw lastError
}

module.exports = {
  DEFAULT_HOST,
  DEFAULT_PORT,
  CONNECT_MAX_ATTEMPTS,
  CONNECT_TIMEOUT_MS,
  N2N_PROGRESS,
  TAP_NAME,
  activeNetwork,
  buildEdgeArgs,
  connect,
  ensureRoomRoute,
  removeRoomRoute,
  isWelTapAdapter,
  isRetryableConnectError,
  n2nCommunity,
  n2nExitReason,
  classifyN2NPeers,
  queryEdgeManagement,
  transportStatus,
  transportConfigPath,
  parseTapGuid,
  parsePingSummary,
  pingHost,
  parseTapctlList,
  parseRegistryConnectionNames,
  parseRegistryTapAdapters,
  parseWmiTapAdapters,
  prepare,
  readRecentLog,
  selectWelTapAdapter,
  status,
  stopConnection,
}
