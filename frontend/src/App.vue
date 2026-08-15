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
const pingResults = ref<Record<number, PingResult | undefined>>({})
const selectedMemberId = ref<number | null>(null)
const form = ref({ username: '', password: '' })
const GAME_PATH_KEY = 'we8.game-path'
const LEGACY_GAME_PATH_KEY = 'pes8.game-path'
const gamePath = ref(localStorage.getItem(GAME_PATH_KEY) ?? localStorage.getItem(LEGACY_GAME_PATH_KEY) ?? '')
const totalOnline = computed(() => rooms.value.reduce((total, room) => total + room.members, 0))
const activeRoom = computed(() => activeLease.value ? rooms.value.find(room => room.id === activeLease.value?.room_id) ?? null : null)
const activeRoomName = computed(() => activeRoom.value?.name ?? (activeLease.value ? `房间 ${activeLease.value.room_id}` : '未进入房间'))
const roomInfoTitle = computed(() => activeLease.value ? activeRoomName.value : '未进入房间')
const roomInfoSubtitle = computed(() => {
  if (!activeLease.value) return '请选择一个可用房间进入'
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
const roomMembersIntervalMs = 15 * 1000
let heartbeatTimer: number | undefined
let sessionCheckTimer: number | undefined
let roomMembersTimer: number | undefined
let transportStatusTimer: number | undefined
let signingOut = false
let leaseEpoch = 0

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
  roomMembersTimer = undefined
  roomMembers.value = []
}

function startRoomMembersMonitor() {
  stopRoomMembersMonitor()
  if (!activeLease.value) return
  void loadRoomMembers()
  roomMembersTimer = window.setInterval(() => { void loadRoomMembers() }, roomMembersIntervalMs)
}

function clearRoomSessionState() {
  stopTransportStatusMonitor()
  activeLease.value = null
  networkStatus.value = null
  pingResults.value = {}
  selectedMemberId.value = null
  warningMessage.value = ''
}

function stopTransportStatusMonitor() {
  if (transportStatusTimer !== undefined) window.clearInterval(transportStatusTimer)
  transportStatusTimer = undefined
}

function startTransportStatusMonitor() {
  stopTransportStatusMonitor()
  if (!desktop()?.transportStatus) return
  transportStatusTimer = window.setInterval(async () => {
    try { notice.value = (await desktop()!.transportStatus()).summary } catch { /* diagnostic status is best effort */ }
  }, 2000)
}

async function loadRoomMembers() {
  const lease = activeLease.value
  if (!lease) return
  try {
    const result = await roomApi.members(lease.room_id)
    if (activeLease.value?.room_id === lease.room_id) {
      roomMembers.value = result.members
      const peer = result.members.find(member => !member.is_self && member.ice_description)
      if (peer?.ice_description && desktop()?.configureIce) {
        try { await desktop()!.configureIce(peer.ice_description) } catch { /* relay remains available */ }
      }
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
  try {
    lease = (await roomApi.join(room.id)).lease
    activeLease.value = lease
    if (desktop()?.prepareIce) {
      try {
        const ice = await desktop()!.prepareIce({
          stunHost: lease.ice_stun_host, stunPort: lease.ice_stun_port,
          relay: `${lease.relay_host}:${lease.relay_port}`, room: lease.community,
          logicalIp: lease.logical_ip || lease.virtual_ip, token: lease.relay_token,
        })
        await roomApi.publishIce(room.id, ice.localDescription)
        notice.value = '已进入房间，正在探测直连'
      } catch (error) {
        warningMessage.value = `直连候选准备失败，将继续使用中继：${messageOf(error)}`
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
    notice.value = '已进入房间'
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
  try {
    await roomApi.leave(lease.room_id)
  } catch (error) {
    if (!(error instanceof ApiError && error.status === 404)) cleanupError ??= error
  }
  clearRoomSessionState()
  if (cleanupError) throw cleanupError
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
    const result = await desktop()!.launchGame({
      gamePath: gamePath.value,
      relay: `${activeLease.value.relay_host}:${activeLease.value.relay_port}`,
      room: activeLease.value.community,
      logicalIp: activeLease.value.logical_ip || activeLease.value.virtual_ip,
      token: activeLease.value.relay_token,
      remoteIp: roomMembers.value.find(member => !member.is_self && member.ice_description)?.virtual_ip,
      remoteDescription: roomMembers.value.find(member => !member.is_self && member.ice_description)?.ice_description,
    })
    const warnings = [...(result.warnings || [])]
    warningMessage.value = [...new Set(warnings)].join('\n')
    notice.value = '已启动 WE8'
    startTransportStatusMonitor()
  } catch (error) {
    errorMessage.value = messageOf(error)
  } finally {
    loading.value = false
  }
}
async function pingMember(member: RoomMember) {
  if (!desktop()) return
  pingResults.value = { ...pingResults.value, [member.user_id]: { host: member.virtual_ip, reachable: false, summary: '正在探测中继与直连...' } }
  try {
    let relaySummary = '中继不可用'
    try { relaySummary = `中继 ${await desktop()!.pingRelay()} ms` } catch (error) { relaySummary = messageOf(error) }
    let directSummary = member.ice_description ? '直连探测中，请稍后再试' : '对方尚未完成 candidate'
    if (member.ice_description && desktop()?.pingIce) {
      try { directSummary = `直连 ${await desktop()!.pingIce(member.ice_description)} ms` } catch (error) { directSummary = messageOf(error) }
    }
    pingResults.value = { ...pingResults.value, [member.user_id]: { host: member.virtual_ip, reachable: relaySummary.startsWith('中继 '), summary: `${relaySummary}；${directSummary}` } }
  } catch (error) {
    pingResults.value = {
      ...pingResults.value,
      [member.user_id]: { host: member.virtual_ip, reachable: false, summary: messageOf(error) },
    }
  }
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

onMounted(restoreSession)
onBeforeUnmount(() => {
  stopLeaseHeartbeat()
  stopSessionMonitor()
  stopRoomMembersMonitor()
  stopTransportStatusMonitor()
})
</script>

<template>
  <main v-if="!user" class="auth-shell">
    <section class="auth-panel">
      <div class="brand-mark"><Gamepad2 :size="28" /></div>
      <p class="eyebrow">{{ runtimeConfig.platformShortName }} ONLINE ARENA</p>
      <h1>{{ runtimeConfig.platformName }}</h1>
      <form @submit.prevent="authenticate">
        <label>账号<input v-model.trim="form.username" autocomplete="username" placeholder="3 至 32 位账号" required /></label>
        <label>密码<input v-model="form.password" type="password" autocomplete="current-password" placeholder="至少 6 位" minlength="6" required /></label>
        <p v-if="errorMessage" class="form-error">{{ errorMessage }}</p>
        <button class="primary-button" :disabled="loading">{{ loading ? '处理中...' : '登录平台' }}</button>
      </form>
    </section>
  </main>

  <main v-else class="app-shell">
    <aside class="sidebar">
      <div class="sidebar-brand"><span class="brand-mark"><Gamepad2 :size="22" /></span><span>{{ runtimeConfig.platformName }}</span></div>
      <div class="user-row"><span class="avatar">{{ user.nickname.slice(0, 1) }}</span><span><strong>{{ user.nickname }}</strong><small>@{{ user.username }}</small></span></div>
      <nav><a class="active"><Users :size="18" /> 对战房间</a></nav>
      <div class="sidebar-actions"><button class="logout" @click="logout"><LogOut :size="17" /> 退出登录</button></div>
    </aside>

    <section class="content">
      <header class="topbar"><div><p class="eyebrow">游戏大厅</p><h2>选择一个对战房间</h2></div><div class="topbar-actions"><div class="online"><span></span>{{ totalOnline }} 人在线</div></div></header>
      <section class="game-path-panel"><div><p class="eyebrow">当前游戏路径</p><span :class="['game-path', { empty: !gamePath.trim() }]">{{ gamePathLabel }}</span></div><button class="secondary-button" @click="chooseGame"><FolderOpen :size="17" /> 选择游戏</button></section>
      <p v-if="errorMessage" class="banner error">{{ errorMessage }}</p><p v-if="warningMessage" class="banner warning">{{ warningMessage }}</p><p v-if="notice" class="banner notice">{{ notice }}</p>

      <section class="connection-strip room-status-strip" :class="{ connected: activeLease }">
        <div><p class="eyebrow">房间信息</p><h3>{{ roomInfoTitle }}</h3><span><Router :size="15" /> {{ roomInfoSubtitle }}</span></div>
        <div class="connection-actions"><span class="secure"><ShieldCheck :size="17" /> 逻辑 IP：{{ virtualIpLabel }}</span><button class="primary-button launch" @click="launchGame" :disabled="!activeLease || !networkStatus?.connected"><Play :size="17" /> 启动 {{ runtimeConfig.gameName }}</button><button v-if="activeLease" class="secondary-button" @click="leaveRoom" :disabled="loading">退出房间</button></div>
      </section>

      <div class="room-workspace">
        <section class="room-section"><div class="section-heading"><h3>可用房间</h3><button class="icon-button" title="刷新房间" @click="loadRooms" :disabled="loading"><RefreshCw :size="18" :class="{ spinning: loading }" /></button></div>
          <div class="room-grid"><article v-for="room in rooms" :key="room.id" class="room-card" :class="{ unavailable: room.status !== 'open' }"><div class="room-card-top"><span class="region">{{ room.region }}</span><span :class="['room-state', room.status]">{{ room.status === 'open' ? '可进入' : '维护中' }}</span></div><h3>{{ room.name }}</h3><p>{{ room.subnet_cidr }}</p><div class="room-card-footer"><span><Users :size="16" /> {{ room.members }} / {{ room.capacity }}</span><button class="join-button" :disabled="loading || room.status !== 'open' || Boolean(activeLease)" @click="joinRoom(room)">进入</button></div></article></div>
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
                <strong :class="['ping-result', { ok: selectedMemberPing?.reachable }]">{{ selectedMemberPing?.summary || '未检测' }}</strong>
              </div>
            </div>
            <footer class="member-modal-footer">
              <button class="secondary-button" :disabled="!selectedMember.virtual_ip || !desktop()" @click="pingMember(selectedMember)">Ping</button>
              <button class="primary-button" @click="closeMemberDetail">关闭</button>
            </footer>
          </section>
        </div>
      </teleport>

    </section>
  </main>
</template>
