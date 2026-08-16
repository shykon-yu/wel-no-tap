<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { FolderOpen, Gamepad2, LogOut, Play, RefreshCw, Router, ShieldCheck, Users } from 'lucide-vue-next'
import { ApiError, authApi, clearToken, hasToken, roomApi, setToken, type Lease, type Room, type RoomMember, type User } from './api'
import type { DesktopLeaseStatus, PingResult } from './electron'
import { runtimeConfig } from './config'

const user = ref<User | null>(null)
const rooms = ref<Room[]>([])
const activeLease = ref<Lease | null>(null)
const roomMembers = ref<RoomMember[]>([])
const networkStatus = ref<DesktopLeaseStatus | null>(null)
const loading = ref(false)
const errorMessage = ref('')
const warningMessage = ref('')
const notice = ref('')
const gameTransportSummary = ref('尚未启动游戏')
const directCandidateStatus = ref<'waiting' | 'gathering' | 'ready' | 'relay-only'>('waiting')
const directCandidateMessage = ref('尚未收集直连候选')
const pingResults = ref<Record<number, PingResult | undefined>>({})
const selectedMemberId = ref<number | null>(null)
const form = ref({ username: '', password: '' })
const GAME_PATH_KEY = 'we8.game-path'
const LEGACY_GAME_PATH_KEY = 'pes8.game-path'
const gamePath = ref(localStorage.getItem(GAME_PATH_KEY) ?? localStorage.getItem(LEGACY_GAME_PATH_KEY) ?? '')
const totalOnline = computed(() => rooms.value.reduce((total, room) => total + room.members, 0))
const activeRoom = computed(() => activeLease.value ? rooms.value.find(room => room.id === activeLease.value?.room_id) ?? null : null)
const displayRoomName = (room: Room) => `房间 ${String(room.id).padStart(2, '0')}`
const activeRoomName = computed(() => activeRoom.value ? displayRoomName(activeRoom.value) : (activeLease.value ? `房间 ${String(activeLease.value.room_id).padStart(2, '0')}` : '未进入房间'))
const roomInfoTitle = computed(() => activeLease.value ? activeRoomName.value : '未进入房间')
const roomInfoSubtitle = computed(() => {
  if (!activeLease.value) return '请选择一个可用房间进入'
  if (directCandidateStatus.value === 'gathering') return `${activeRoomName.value} · 正在收集直连候选`
  if (directCandidateStatus.value === 'ready') return `${activeRoomName.value} · 中继已连接 · 直连候选已就绪`
  if (directCandidateStatus.value === 'relay-only') return `${activeRoomName.value} · 中继已连接 · 仅使用中继`
  return networkStatus.value?.connected ? `${activeRoomName.value} · 网络已连接` : `${activeRoomName.value} · 正在确认网络`
})
const virtualIpLabel = computed(() => {
  if (!activeLease.value) return '待分配'
  return networkStatus.value?.actualIp ? `${networkStatus.value.actualIp} / ${activeLease.value.subnet_cidr}` : '尚未获取'
})
const gamePathLabel = computed(() => gamePath.value.trim() || `未选择 ${runtimeConfig.gameName} 游戏程序`)
const desktop = () => window.welNoTapDesktop
const heartbeatIntervalMs = 5 * 60 * 1000
const sessionCheckIntervalMs = 30 * 1000
const roomMembersIntervalMs = 3 * 1000
const peerProbeIntervalMs = 1000
const maxIncomingProbeAgents = 4
let heartbeatTimer: number | undefined
let sessionCheckTimer: number | undefined
let peerProbeTimer: number | undefined
let roomMembersTimer: number | undefined
let transportStatusTimer: number | undefined
let signingOut = false
let leaseEpoch = 0
let platformExitInProgress = false
let removeBeforeQuitListener: (() => void) | undefined
let removeGamePeerListener: (() => void) | undefined
const incomingProbeIds = new Set<number>()
const activeProbeKeys = new Set<string>()
const gamePeerTasks = new Map<string, Promise<void>>()
const pingingMemberIds = ref<Set<number>>(new Set())
let peerProbePollInFlight = false
let activeIncomingProbeAgents = 0

function stopLeaseHeartbeat() {
  if (heartbeatTimer !== undefined) window.clearInterval(heartbeatTimer)
  heartbeatTimer = undefined
  leaseEpoch += 1
}

function startLeaseHeartbeat() {
  stopLeaseHeartbeat()
  if (!activeLease.value) return
  const epoch = ++leaseEpoch
  heartbeatTimer = window.setInterval(() => { void renewLease(epoch) }, heartbeatIntervalMs)
}

function stopSessionMonitor() {
  if (sessionCheckTimer !== undefined) window.clearInterval(sessionCheckTimer)
  sessionCheckTimer = undefined
}

function startSessionMonitor() {
  stopSessionMonitor()
  if (!user.value) return
  sessionCheckTimer = window.setInterval(() => { void checkSession() }, sessionCheckIntervalMs)
}

function stopRoomMembersMonitor() {
  if (roomMembersTimer !== undefined) window.clearInterval(roomMembersTimer)
  if (peerProbeTimer !== undefined) window.clearInterval(peerProbeTimer)
  roomMembersTimer = undefined
  peerProbeTimer = undefined
  roomMembers.value = []
}

function startRoomMembersMonitor() {
  stopRoomMembersMonitor()
  if (!activeLease.value) return
  void loadRoomMembers()
  void answerIncomingPeerProbes(activeLease.value)
  roomMembersTimer = window.setInterval(() => { void loadRoomMembers() }, roomMembersIntervalMs)
  peerProbeTimer = window.setInterval(() => {
    if (activeLease.value) void answerIncomingPeerProbes(activeLease.value)
  }, peerProbeIntervalMs)
}

function clearRoomSessionState() {
  stopTransportStatusMonitor()
  activeLease.value = null
  networkStatus.value = null
  directCandidateStatus.value = 'waiting'
  directCandidateMessage.value = '尚未收集直连候选'
  pingResults.value = {}
  selectedMemberId.value = null
  warningMessage.value = ''
  gameTransportSummary.value = '尚未启动游戏'
  incomingProbeIds.clear()
  activeIncomingProbeAgents = 0
  peerProbePollInFlight = false
  pingingMemberIds.value = new Set()
  gamePeerTasks.clear()
}

function stopTransportStatusMonitor() {
  if (transportStatusTimer !== undefined) window.clearInterval(transportStatusTimer)
  transportStatusTimer = undefined
}

function startTransportStatusMonitor() {
  stopTransportStatusMonitor()
  if (!desktop()?.transportStatus) return
  const refresh = async () => {
    try { gameTransportSummary.value = (await desktop()!.transportStatus()).summary } catch { /* status is best effort */ }
  }
  void refresh()
  transportStatusTimer = window.setInterval(() => { void refresh() }, 2000)
}

async function loadRoomMembers() {
  const lease = activeLease.value
  if (!lease) return
  try {
    const result = await roomApi.members(lease.room_id)
    if (activeLease.value?.room_id === lease.room_id) {
      roomMembers.value = result.members
    }
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) await forceSignedOut(error.message)
  }
}

async function checkSession() {
  if (!user.value || signingOut) return
  try {
    await authApi.me()
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) await forceSignedOut(error.message)
  }
}

async function forceSignedOut(message: string) {
  if (signingOut) return
  signingOut = true
  stopLeaseHeartbeat()
  stopSessionMonitor()
  stopRoomMembersMonitor()
  const lease = activeLease.value
  if (lease) {
    try { await desktop()?.disconnect() } catch { /* the server has already revoked this session */ }
  }
  clearRoomSessionState()
  user.value = null
  rooms.value = []
  clearToken()
  notice.value = ''
  errorMessage.value = message
  signingOut = false
}

async function renewLease(epoch: number) {
  const lease = activeLease.value
  if (!lease) return
  try {
    const result = await roomApi.heartbeat(lease.room_id)
    if (activeLease.value?.room_id === lease.room_id) activeLease.value.expires_at = result.expires_at
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) {
      await forceSignedOut(error.message)
      return
    }
    if (error instanceof ApiError && (error.status === 404 || error.status === 409)) {
      // A response from a previous room must not clear a newer room session.
      if (epoch !== leaseEpoch) return
      stopLeaseHeartbeat()
      stopRoomMembersMonitor()
      try { await desktop()?.disconnect() } catch { /* local connection may already be gone */ }
      activeLease.value = null
      networkStatus.value = null
      await loadRooms()
      errorMessage.value = error.message
      return
    }
    errorMessage.value = `房间连接续期失败，将自动重试：${messageOf(error)}`
  }
}

async function loadRooms() {
  loading.value = true
  errorMessage.value = ''
  try { rooms.value = (await roomApi.list()).rooms } catch (error) { errorMessage.value = messageOf(error) } finally { loading.value = false }
}

async function authenticate() {
  loading.value = true
  errorMessage.value = ''
  try {
    const session = await authApi.login({ username: form.value.username, password: form.value.password })
    setToken(session.token)
    user.value = session.user
    form.value.password = ''
    activeLease.value = (await authApi.roomSession()).lease
    startLeaseHeartbeat()
    startRoomMembersMonitor()
    await loadRooms()
    startSessionMonitor()
  } catch (error) { errorMessage.value = messageOf(error) } finally { loading.value = false }
}

async function restoreSession() {
  if (!hasToken()) return
  let previousLease: Lease | null = null
  try {
    user.value = (await authApi.me()).user
    previousLease = (await authApi.roomSession()).lease
  } catch (error) {
    clearToken()
    if (error instanceof ApiError && error.status === 401) errorMessage.value = error.message
    return
  }

  clearRoomSessionState()
  await loadRooms()
  startSessionMonitor()
  if (previousLease) {
    try { await desktop()?.disconnect() } catch { /* best effort cleanup */ }
    try { await roomApi.leave(previousLease.room_id) } catch { /* stale room state will expire server-side */ }
    notice.value = '已清理上次房间状态，请重新进入房间'
    await loadRooms()
  }
}

async function joinRoom(room: Room) {
  loading.value = true
  errorMessage.value = ''
  warningMessage.value = ''
  let lease: Lease | null = null
  let firewallWarning = ''
  let roomNotice = '已进入房间，当前仅使用中继'
  try {
    lease = (await roomApi.join(room.id)).lease
    activeLease.value = lease
    try {
      const firewall = await desktop()?.ensureFirewall({ gamePath: gamePath.value })
      firewallWarning = firewall?.warning || ''
      warningMessage.value = firewallWarning
    } catch (error) {
      firewallWarning = '无法确认 Windows 防火墙规则，直连可能不可用；当前仍可使用中继。'
      warningMessage.value = firewallWarning
    }
    directCandidateStatus.value = 'gathering'
    directCandidateMessage.value = '正在收集直连候选'
    notice.value = `正在连接直连服务 ${lease.ice_stun_host}:${lease.ice_stun_port} 并收集 candidate...`
    if (desktop()?.prepareIce) {
      try {
        const ice = await desktop()!.prepareIce({
          stunHost: lease.ice_stun_host, stunPort: lease.ice_stun_port,
          relay: `${lease.relay_host}:${lease.relay_port}`, room: lease.community,
          logicalIp: lease.logical_ip || lease.virtual_ip, token: lease.relay_token,
        })
        await roomApi.publishIce(room.id, ice.localDescription)
        directCandidateStatus.value = 'ready'
        directCandidateMessage.value = summarizeCandidates(ice.localDescription)
        roomNotice = `已进入房间，${directCandidateMessage.value}`
      } catch (error) {
        directCandidateStatus.value = 'relay-only'
        directCandidateMessage.value = '直连候选收集失败，当前仅使用中继'
        const message = messageOf(error)
        const candidateWarning = message.includes('ICE candidate 收集超时')
          ? '已尝试通过直连服务收集 candidate，但 12 秒内未完成；当前仅使用中继，稍后点击玩家 Ping 可再次尝试'
          : message.includes('ICE 辅助程序提前退出')
            ? `已尝试启动直连候选收集，但组件提前退出；当前仅使用中继：${message}`
            : `直连候选准备失败，当前仅使用中继：${message}`
        warningMessage.value = [firewallWarning, candidateWarning].filter(Boolean).join('\n')
      }
    }
    networkStatus.value = {
      ready: true,
      connected: true,
      message: '房间已准备，启动游戏时建立云中继连接',
      actualIp: lease.logical_ip || lease.virtual_ip,
    }
    startLeaseHeartbeat()
    startRoomMembersMonitor()
    notice.value = roomNotice
    await loadRooms()
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) {
      await forceSignedOut(error.message)
      return
    }
    stopLeaseHeartbeat()
    stopRoomMembersMonitor()
    if (lease) {
      try { await desktop()?.disconnect() } catch { /* connection setup may be incomplete */ }
    }
    if (lease) try { await roomApi.leave(room.id) } catch { /* the lease reaper will clean it up */ }
    activeLease.value = null
    networkStatus.value = null
    errorMessage.value = messageOf(error)
  } finally { loading.value = false }
}

async function releaseActiveLease() {
  const lease = activeLease.value
  if (!lease) return
  stopLeaseHeartbeat()
  stopRoomMembersMonitor()
  let cleanupError: unknown
  try { await desktop()?.disconnect() } catch (error) { cleanupError = error }
  activeProbeKeys.clear()
  try {
    await roomApi.leave(lease.room_id)
  } catch (error) {
    if (!(error instanceof ApiError && error.status === 404)) cleanupError ??= error
  }
  clearRoomSessionState()
  if (cleanupError) throw cleanupError
}

async function handlePlatformExit() {
  if (platformExitInProgress) return
  platformExitInProgress = true
  try {
    if (activeLease.value) await releaseActiveLease()
    else await desktop()?.disconnect()
  } catch {
    // Closing must not be blocked by a disconnected API or local helper.
  } finally {
    try { await desktop()?.completeQuit() } catch { /* the main-process timeout is the fallback */ }
  }
}

async function leaveRoom() {
  if (!activeLease.value) return
  loading.value = true
  try {
    await releaseActiveLease()
    notice.value = '已退出房间'
    await loadRooms()
  } catch (error) { errorMessage.value = messageOf(error) } finally { loading.value = false }
}

function saveGamePath() {
  localStorage.setItem(GAME_PATH_KEY, gamePath.value)
  localStorage.removeItem(LEGACY_GAME_PATH_KEY)
  notice.value = '游戏路径已保存'
}
async function chooseGame() {
  errorMessage.value = ''
  if (!desktop()) {
    notice.value = '浏览器预览不会弹出本机文件选择器，请在 Windows 客户端测试'
    return
  }
  try {
    const selectedPath = await desktop()!.chooseGame()
    if (!selectedPath) return
    gamePath.value = selectedPath
    saveGamePath()
  } catch (error) {
    errorMessage.value = messageOf(error)
  }
}

async function launchGame() {
  errorMessage.value = ''
  if (!activeLease.value) { errorMessage.value = '请先进入一个房间'; return }
  if (!gamePath.value.trim()) { errorMessage.value = `请先选择 ${runtimeConfig.gameName} 游戏程序路径`; return }
  if (!desktop()) { notice.value = '浏览器预览不会启动本机程序，请在 Windows 客户端测试'; return }
  loading.value = true
  try {
    const firewall = await desktop()!.ensureFirewall({ gamePath: gamePath.value })
    if (firewall.warning) warningMessage.value = firewall.warning
    const result = await desktop()!.launchGame({
      gamePath: gamePath.value,
      relay: `${activeLease.value.relay_host}:${activeLease.value.relay_port}`,
      room: activeLease.value.community,
      logicalIp: activeLease.value.logical_ip || activeLease.value.virtual_ip,
      token: activeLease.value.relay_token,
    })
    const warnings = [...(result.warnings || [])]
    warningMessage.value = [...new Set(warnings)].join('\n')
    notice.value = result.detail.includes('injection=apc') ? '已启动 WE8（APC 兼容模式）' : '已启动 WE8'
    gameTransportSummary.value = '游戏已启动，正在确认联机线路'
    startTransportStatusMonitor()
  } catch (error) {
    errorMessage.value = messageOf(error)
  } finally {
    loading.value = false
  }
}
async function pingMember(member: RoomMember) {
  if (!desktop()) return
  if (pingingMemberIds.value.has(member.user_id)) return
  pingingMemberIds.value = new Set(pingingMemberIds.value).add(member.user_id)
  pingResults.value = {
    ...pingResults.value,
    [member.user_id]: {
      host: member.virtual_ip,
      reachable: false,
      summary: '正在探测中继服务器、中继玩家与直连...',
      relayServer: { reachable: false, summary: '探测中...' },
      relayPeer: { reachable: false, summary: '探测中...' },
      direct: { reachable: false, summary: '正在建立玩家专属直连探测...' },
    },
  }
  try {
    const relayServerPromise = desktop()!.pingRelay()
      .then(milliseconds => ({ reachable: true, summary: `中继服务器 ${milliseconds} ms` }))
      .catch(() => ({ reachable: false, summary: '中继服务器不可用' }))
    const relayPeerPromise = desktop()?.pingRelayPeer && member.virtual_ip
      ? desktop()!.pingRelayPeer(member.virtual_ip)
        .then(milliseconds => ({ reachable: true, summary: `中继玩家 ${milliseconds} ms` }))
        .catch(() => ({ reachable: false, summary: '中继玩家探测超时' }))
      : Promise.resolve({ reachable: false, summary: member.virtual_ip ? '中继玩家探测不可用' : '对方逻辑 IP 未知' })
    const directPromise = (async () => {
      const lease = activeLease.value
      if (!lease || member.is_self || !desktop()?.createProbeIce) return { reachable: false, summary: member.is_self ? '不能探测自己' : '直连探测不可用' }
      let probeKey = ''
      try {
        const local = await desktop()!.createProbeIce({ stunHost: lease.ice_stun_host, stunPort: lease.ice_stun_port })
        probeKey = local.probeKey
        activeProbeKeys.add(probeKey)
        const created = await roomApi.createPeerProbe(lease.room_id, member.user_id, local.localDescription)
        const answered = await waitForPeerProbeAnswer(lease.room_id, created.probe.id)
        if (answered.requester_user_id !== user.value?.id || answered.target_user_id !== member.user_id) {
          throw new Error('直连探测目标校验失败')
        }
        if (!answered.target_description) throw new Error('目标玩家未返回直连 candidate')
        if (!await desktop()!.configureProbeIce(probeKey, answered.target_description)) throw new Error('临时 ICE 配置失败')
        const milliseconds = await desktop()!.pingProbeIce(probeKey)
        return { reachable: true, summary: `直连 ${milliseconds} ms` }
      } catch (error) {
        const message = messageOf(error)
        return {
          reachable: false,
          summary: message.includes('直连探测应答超时')
            ? '对方客户端未响应直连探测'
            : message.includes('PING_UNAVAILABLE')
              ? 'ICE 尚未建立，直连 Ping 未发送'
              : message.includes('远端 candidate 无效') || message.includes('未确认远端 candidate')
                ? '对方 candidate 无效或未确认'
            : message.includes('ICE 直连检查超时')
              ? 'ICE 直连检查超时'
              : message.includes('直连 Ping 超时')
                ? '直连 Ping 超时'
                : '直连不可用',
        }
      } finally {
        if (probeKey) {
          try { await desktop()!.stopProbeIce(probeKey) } catch {}
          activeProbeKeys.delete(probeKey)
        }
      }
    })()
    const [relayServerResult, relayPeerResult, directResult] = await Promise.all([relayServerPromise, relayPeerPromise, directPromise])
    pingResults.value = {
      ...pingResults.value,
      [member.user_id]: {
        host: member.virtual_ip,
        reachable: relayServerResult.reachable || relayPeerResult.reachable || directResult.reachable,
        summary: `${relayServerResult.summary}；${relayPeerResult.summary}；${directResult.summary}`,
        relayServer: relayServerResult,
        relayPeer: relayPeerResult,
        direct: directResult,
      },
    }
  } catch (error) {
    pingResults.value = {
      ...pingResults.value,
      [member.user_id]: {
        host: member.virtual_ip,
        reachable: false,
        summary: messageOf(error),
        relayServer: { reachable: false, summary: '中继服务器不可用' },
        relayPeer: { reachable: false, summary: '中继玩家不可用' },
        direct: { reachable: false, summary: '直连不可用' },
      },
    }
  } finally {
    const next = new Set(pingingMemberIds.value)
    next.delete(member.user_id)
    pingingMemberIds.value = next
  }
}

async function waitForPeerProbeAnswer(roomID: number, probeID: number) {
  const deadline = Date.now() + 18000
  while (Date.now() < deadline) {
    const result = await roomApi.peerProbe(roomID, probeID)
    if (result.probe.target_description) return result.probe
    await new Promise(resolve => window.setTimeout(resolve, 400))
  }
  throw new Error('直连探测应答超时')
}

async function answerIncomingPeerProbes(lease: Lease) {
  if (!desktop()?.createProbeIce || activeLease.value?.room_id !== lease.room_id || peerProbePollInFlight) return
  peerProbePollInFlight = true
  try {
    let probes
    try { probes = (await roomApi.incomingPeerProbes(lease.room_id)).probes } catch { return }
    for (const probe of probes) {
      if (activeIncomingProbeAgents >= maxIncomingProbeAgents) break
      if (incomingProbeIds.has(probe.id) || !probe.requester_description) continue
      incomingProbeIds.add(probe.id)
      activeIncomingProbeAgents += 1
      void (async () => {
        let probeKey = ''
        let retained = false
        try {
          const local = await desktop()!.createProbeIce({ stunHost: lease.ice_stun_host, stunPort: lease.ice_stun_port })
          probeKey = local.probeKey
          activeProbeKeys.add(probeKey)
          if (!await desktop()!.configureProbeIce(probeKey, probe.requester_description!)) throw new Error('临时 ICE 配置失败')
          await roomApi.answerPeerProbe(lease.room_id, probe.id, local.localDescription)
          retained = true
          window.setTimeout(() => {
            void desktop()?.stopProbeIce(probeKey)
            activeProbeKeys.delete(probeKey)
            activeIncomingProbeAgents = Math.max(0, activeIncomingProbeAgents - 1)
          }, 25000)
        } catch {
          if (probeKey) {
            try { await desktop()!.stopProbeIce(probeKey) } catch {}
            activeProbeKeys.delete(probeKey)
          }
          incomingProbeIds.delete(probe.id)
        } finally {
          if (!retained) activeIncomingProbeAgents = Math.max(0, activeIncomingProbeAgents - 1)
        }
      })()
    }
  } finally {
    peerProbePollInFlight = false
  }
}

async function configureGamePeerOnce(logicalIp: string) {
  const lease = activeLease.value
  if (!lease || !desktop()?.configureIce) return
  for (let attempt = 0; attempt < 20; attempt += 1) {
    try {
      const result = await roomApi.members(lease.room_id)
      if (activeLease.value?.room_id !== lease.room_id) return
      roomMembers.value = result.members
      const member = result.members.find(item => !item.is_self && item.virtual_ip === logicalIp)
      if (member?.ice_description) {
        const configured = await desktop()!.configureIce({ remoteIp: member.virtual_ip, remoteDescription: member.ice_description })
        if (!configured) {
          warningMessage.value = '比赛直连通道已锁定其他玩家，本场继续使用云中继'
          return
        }
        gameTransportSummary.value = `已锁定对手 ${member.nickname}，正在建立 P2P 直连`
        return
      }
      gameTransportSummary.value = member ? `已识别对手 ${member.nickname}，等待其直连候选` : '正在确认比赛对手，当前使用云中继'
    } catch {
      gameTransportSummary.value = '正在确认比赛对手，当前使用云中继'
    }
    await new Promise(resolve => window.setTimeout(resolve, 500))
  }
  gameTransportSummary.value = '对手直连候选未及时到达，当前使用云中继'
}

function configureGamePeer(logicalIp: string) {
  const normalizedIp = String(logicalIp || '').trim()
  if (!normalizedIp || gamePeerTasks.has(normalizedIp)) return
  const task = configureGamePeerOnce(normalizedIp)
  gamePeerTasks.set(normalizedIp, task)
  void task.then(() => {
    if (gamePeerTasks.get(normalizedIp) === task) gamePeerTasks.delete(normalizedIp)
  }, () => {
    if (gamePeerTasks.get(normalizedIp) === task) gamePeerTasks.delete(normalizedIp)
  })
}

function openMemberDetail(member: RoomMember) {
  selectedMemberId.value = member.user_id
}

function closeMemberDetail() {
  selectedMemberId.value = null
}

const selectedMember = computed(() => roomMembers.value.find(member => member.user_id === selectedMemberId.value) ?? null)
const selectedMemberPing = computed(() => selectedMember.value ? pingResults.value[selectedMember.value.user_id] ?? null : null)
async function logout() {
  loading.value = true
  errorMessage.value = ''
  try {
    await releaseActiveLease()
  } catch {
    errorMessage.value = '房间清理未完全成功，服务器将在超时后自动回收'
  }
  try {
    await authApi.logout()
  } catch (error) {
    if (!(error instanceof ApiError && error.status === 401)) {
      errorMessage.value ||= '服务器登录状态未完全清理，将在有效期结束后自动失效'
    }
  } finally {
    stopLeaseHeartbeat()
    stopSessionMonitor()
    stopRoomMembersMonitor()
    user.value = null
    networkStatus.value = null
    clearToken()
    rooms.value = []
    loading.value = false
  }
}
function messageOf(error: unknown) {
  if (typeof error === 'string') return error
  if (error instanceof Error) return error.message
  return '发生未知错误'
}

function summarizeCandidates(description: string) {
  const candidates = String(description || '').split(/\r?\n/).filter(line => line.startsWith('a=candidate:'))
  const host = candidates.filter(line => / typ host(?: |$)/.test(line)).length
  const serverReflexive = candidates.filter(line => / typ srflx(?: |$)/.test(line)).length
  const relay = candidates.filter(line => / typ relay(?: |$)/.test(line)).length
  const details = [`${candidates.length} 个候选`]
  if (host) details.push(`${host} 个本机地址`)
  if (serverReflexive) details.push(`${serverReflexive} 个公网地址`)
  if (relay) details.push(`${relay} 个中继地址`)
  return `直连候选已就绪（${details.join('，')}）`
}

onMounted(() => {
  removeBeforeQuitListener = desktop()?.onBeforeQuit?.(() => { void handlePlatformExit() })
  removeGamePeerListener = desktop()?.onGamePeer?.((logicalIp) => { void configureGamePeer(logicalIp) })
  void restoreSession()
})
onBeforeUnmount(() => {
  stopLeaseHeartbeat()
  stopSessionMonitor()
  stopRoomMembersMonitor()
  stopTransportStatusMonitor()
  removeBeforeQuitListener?.()
  removeGamePeerListener?.()
})
</script>

<template>
  <main v-if="!user" class="auth-shell">
    <section class="auth-panel">
      <div class="brand-mark"><Gamepad2 :size="28" /></div>
      <p class="eyebrow">{{ runtimeConfig.platformShortName }} ONLINE ARENA</p>
      <h1>{{ runtimeConfig.platformName }} <span class="app-version">v{{ runtimeConfig.appVersion }}</span></h1>
      <form @submit.prevent="authenticate">
        <label>账号<input v-model.trim="form.username" autocomplete="username" placeholder="3 至 32 位账号" required /></label>
        <label>密码<input v-model="form.password" type="password" autocomplete="current-password" placeholder="至少 6 位" minlength="6" required /></label>
        <p v-if="errorMessage" class="form-error">{{ errorMessage }}</p>
        <button class="primary-button" :disabled="loading">{{ loading ? '处理中...' : '登录平台' }}</button>
      </form>
      <footer class="auth-copyright">© 2026 {{ runtimeConfig.platformName }} · 版权所有 · 【WRH】比安</footer>
    </section>
  </main>

  <main v-else class="app-shell">
    <aside class="sidebar">
      <div class="sidebar-brand"><span class="brand-mark"><Gamepad2 :size="22" /></span><span>{{ runtimeConfig.platformName }}</span><span class="app-version">v{{ runtimeConfig.appVersion }}</span></div>
      <div class="user-row"><span class="avatar">{{ user.nickname.slice(0, 1) }}</span><span><strong>{{ user.nickname }}</strong><small>@{{ user.username }}</small></span></div>
      <nav><a class="active"><Users :size="18" /> 对战房间</a></nav>
      <div class="sidebar-actions"><button class="logout" @click="logout"><LogOut :size="17" /> 退出登录</button></div>
      <footer class="sidebar-copyright">© 2026 【WRH】比安</footer>
    </aside>

    <section class="content">
      <header class="topbar"><div><p class="eyebrow">游戏大厅</p><h2>选择一个对战房间</h2></div><div class="topbar-actions"><div class="online"><span></span>{{ totalOnline }} 人在线</div></div></header>
      <section class="game-path-panel"><div><p class="eyebrow">当前游戏路径</p><span :class="['game-path', { empty: !gamePath.trim() }]">{{ gamePathLabel }}</span></div><button class="secondary-button" @click="chooseGame"><FolderOpen :size="17" /> 选择游戏</button></section>
      <p v-if="errorMessage" class="banner error">{{ errorMessage }}</p><p v-if="warningMessage" class="banner warning">{{ warningMessage }}</p><p v-if="notice" class="banner notice">{{ notice }}</p>

      <section class="connection-strip room-status-strip" :class="{ connected: activeLease }">
        <div><p class="eyebrow">房间信息</p><h3>{{ roomInfoTitle }}</h3><span><Router :size="15" /> {{ roomInfoSubtitle }}</span></div>
        <div class="connection-actions"><span class="secure"><ShieldCheck :size="17" /> 逻辑 IP：{{ virtualIpLabel }}</span><span class="transport-status"><Router :size="17" /> {{ gameTransportSummary }}</span><button class="primary-button launch" @click="launchGame" :disabled="!activeLease || !networkStatus?.connected"><Play :size="17" /> 启动 {{ runtimeConfig.gameName }}</button><button v-if="activeLease" class="secondary-button" @click="leaveRoom" :disabled="loading">退出房间</button></div>
      </section>

      <div class="room-workspace">
        <section class="room-section"><div class="section-heading"><h3>可用房间</h3><button class="icon-button" title="刷新房间" @click="loadRooms" :disabled="loading"><RefreshCw :size="18" :class="{ spinning: loading }" /></button></div>
          <div class="room-grid"><article v-for="room in rooms" :key="room.id" class="room-card" :class="{ unavailable: room.status !== 'open' }"><div class="room-card-top"><span class="region">云中继</span><span :class="['room-state', room.status]">{{ room.status === 'open' ? '可进入' : '维护中' }}</span></div><h3>{{ displayRoomName(room) }}</h3><p>{{ room.subnet_cidr }}</p><div class="room-card-footer"><span><Users :size="16" /> {{ room.members }} / {{ room.capacity }}</span><button class="join-button" :disabled="loading || room.status !== 'open' || Boolean(activeLease)" @click="joinRoom(room)">进入</button></div></article></div>
        </section>
        <aside v-if="activeLease" class="room-members-panel"><div class="section-heading"><div><p class="eyebrow">{{ roomInfoTitle }}</p><h3>房间成员</h3></div><span class="member-count">{{ roomMembers.length }} 人</span></div><div v-if="roomMembers.length" class="member-list"><div v-for="member in roomMembers" :key="member.user_id" class="member-row"><span class="member-avatar">{{ member.nickname.slice(0, 1) }}</span><span><strong>{{ member.nickname }}</strong><small>@{{ member.username }}</small></span><button class="mini-button" @click="openMemberDetail(member)">详情</button><em v-if="member.is_self">我</em></div></div><p v-else class="member-empty">正在读取房间成员...</p></aside>
      </div>

      <teleport to="body">
        <div v-if="selectedMember" class="modal-backdrop" @click.self="closeMemberDetail">
          <section class="member-modal" role="dialog" aria-modal="true" :aria-label="`${selectedMember.nickname} 详情`">
            <header class="member-modal-header">
              <div>
                <p class="eyebrow">房间成员详情</p>
                <h3>{{ selectedMember.nickname }}</h3>
              </div>
              <button class="icon-button modal-close" title="关闭" @click="closeMemberDetail">×</button>
            </header>
            <div class="member-modal-body">
              <div class="detail-row"><span>账号</span><strong>@{{ selectedMember.username }}</strong></div>
              <div class="detail-row"><span>身份</span><strong>{{ selectedMember.is_self ? '当前用户' : '房间成员' }}</strong></div>
              <div class="detail-row"><span>逻辑 IP</span><strong>{{ selectedMember.virtual_ip || '未分配' }}</strong></div>
              <div class="detail-row"><span>真实 IP</span><strong>{{ selectedMember.real_ip || '未知' }}</strong></div>
              <div class="detail-row">
                <span>Ping</span>
                <div class="ping-results">
                  <div><span>中继服务器</span><strong :class="['ping-result', { ok: selectedMemberPing?.relayServer.reachable }]">{{ selectedMemberPing?.relayServer.summary || '未检测' }}</strong></div>
                  <div><span>中继玩家</span><strong :class="['ping-result', { ok: selectedMemberPing?.relayPeer.reachable }]">{{ selectedMemberPing?.relayPeer.summary || '未检测' }}</strong></div>
                  <div><span>直连</span><strong :class="['ping-result', { ok: selectedMemberPing?.direct.reachable }]">{{ selectedMemberPing?.direct.summary || '未检测' }}</strong></div>
                </div>
              </div>
            </div>
            <footer class="member-modal-footer">
              <button class="secondary-button" :disabled="selectedMember.is_self || !selectedMember.virtual_ip || !desktop() || pingingMemberIds.has(selectedMember.user_id)" @click="pingMember(selectedMember)">{{ pingingMemberIds.has(selectedMember.user_id) ? '检测中...' : 'Ping' }}</button>
              <button class="primary-button" @click="closeMemberDetail">关闭</button>
            </footer>
          </section>
        </div>
      </teleport>

    </section>
  </main>
</template>
