import { route, runtimeConfig } from './config'

const apiBase = runtimeConfig.apiBaseUrl
const ACCESS_TOKEN_KEY = `wel.no-tap.access-token:${apiBase}`
const LEGACY_ACCESS_TOKEN_KEY = 'pes8.access-token'

export type User = { id: number; username: string; nickname: string }
export type Room = { id: number; code: string; name: string; region: string; subnet_cidr: string; capacity: number; members: number; status: 'open' | 'maintenance' | 'closed' }
export type RoomMember = { user_id: number; username: string; nickname: string; virtual_ip: string; real_ip?: string; is_self: boolean; ice_description?: string; ice_state: 'waiting' | 'ready' }
export type Lease = { room_id: number; virtual_ip: string; logical_ip?: string; username: string; expires_at: string; subnet_cidr: string; community: string; relay_host: string; relay_port: number; relay_token: string; ice_stun_host: string; ice_stun_port: number }
export type PeerProbe = { id: number; requester_user_id: number; target_user_id: number; requester_description?: string; target_description?: string; expires_at: string }

let token = localStorage.getItem(ACCESS_TOKEN_KEY) ?? ''

export class ApiError extends Error {
  constructor(message: string, readonly status: number, readonly code?: string) {
    super(message)
    this.name = 'ApiError'
  }
}

export function setToken(value: string) {
  token = value
  localStorage.setItem(ACCESS_TOKEN_KEY, value)
  localStorage.removeItem(LEGACY_ACCESS_TOKEN_KEY)
}

export function clearToken() {
  token = ''
  localStorage.removeItem(ACCESS_TOKEN_KEY)
  localStorage.removeItem(LEGACY_ACCESS_TOKEN_KEY)
}

export function hasToken() { return token !== '' }
export function getAccessToken() { return token }

async function request<T>(path: string, options: RequestInit = {}): Promise<T> {
  const headers = new Headers(options.headers)
  headers.set('Content-Type', 'application/json')
  if (token) headers.set('Authorization', `Bearer ${token}`)
  const response = await fetch(`${apiBase}${path}`, { ...options, headers })
  const body = await response.json().catch(() => ({}))
  if (!response.ok) throw new ApiError(body.error ?? '请求失败，请稍后重试', response.status, body.code)
  return body as T
}

export const authApi = {
  login: (payload: { username: string; password: string }) => request<{ token: string; user: User }>(runtimeConfig.apiLoginPath, { method: 'POST', body: JSON.stringify(payload) }),
  logout: () => request<{ ok: boolean }>(runtimeConfig.apiLogoutPath, { method: 'POST' }),
  me: () => request<{ user: User }>(runtimeConfig.apiMePath),
  roomSession: () => request<{ lease: Lease | null }>(runtimeConfig.apiRoomSessionPath),
}

export const roomApi = {
  list: () => request<{ rooms: Room[] }>(runtimeConfig.apiRoomsPath),
  members: (roomID: number) => request<{ members: RoomMember[] }>(route(runtimeConfig.apiRoomMembersPath, roomID)),
  join: (roomID: number) => request<{ lease: Lease }>(route(runtimeConfig.apiRoomJoinPath, roomID), { method: 'POST', body: '{}' }),
  heartbeat: (roomID: number) => request<{ expires_at: string }>(route(runtimeConfig.apiRoomHeartbeatPath, roomID), { method: 'POST', body: '{}' }),
  leave: (roomID: number) => request<{ ok: boolean }>(route(runtimeConfig.apiRoomLeavePath, roomID), { method: 'POST', body: '{}' }),
  publishIce: (roomID: number, localDescription: string) => request<{ state: string }>(route(runtimeConfig.apiRoomIcePath, roomID), { method: 'POST', body: JSON.stringify({ local_description: localDescription }) }),
  createPeerProbe: (roomID: number, targetUserID: number, localDescription: string) => request<{ probe: PeerProbe }>(route(runtimeConfig.apiRoomPeerProbesPath, roomID), { method: 'POST', body: JSON.stringify({ target_user_id: targetUserID, local_description: localDescription }) }),
  incomingPeerProbes: (roomID: number) => request<{ probes: PeerProbe[] }>(`${route(runtimeConfig.apiRoomPeerProbesPath, roomID)}/incoming`),
  peerProbe: (roomID: number, probeID: number) => request<{ probe: PeerProbe }>(`${route(runtimeConfig.apiRoomPeerProbesPath, roomID)}/${probeID}`),
  answerPeerProbe: (roomID: number, probeID: number, localDescription: string) => request<{ state: string }>(`${route(runtimeConfig.apiRoomPeerProbesPath, roomID)}/${probeID}/answer`, { method: 'POST', body: JSON.stringify({ local_description: localDescription }) }),
}
