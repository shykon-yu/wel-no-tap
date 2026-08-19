export type RuntimeConfig = {
  appVersion: string
  platformName: string
  platformShortName: string
  gameName: string
  apiBaseUrl: string
  apiLoginPath: string
  apiLogoutPath: string
  apiMePath: string
  apiRoomSessionPath: string
  apiRoomsPath: string
  apiRoomMembersPath: string
  apiRoomJoinPath: string
  apiRoomHeartbeatPath: string
  apiRoomLeavePath: string
  apiRoomIcePath: string
  apiRoomPeerProbesPath: string
  diagnosticLog?: string
  upnp?: string
  configFile?: string | null
}

const fallback: RuntimeConfig = {
  appVersion: '0.0.45',
  platformName: 'WEL对战平台',
  platformShortName: 'WEL',
  gameName: 'WE8',
  apiBaseUrl: 'http://8.155.145.132:8082/api/v1',
  apiLoginPath: '/auth/login',
  apiLogoutPath: '/auth/logout',
  apiMePath: '/me',
  apiRoomSessionPath: '/notap/me/room-session',
  apiRoomsPath: '/notap/rooms',
  apiRoomMembersPath: '/notap/rooms/{roomId}/members',
  apiRoomJoinPath: '/notap/rooms/{roomId}/join',
  apiRoomHeartbeatPath: '/notap/rooms/{roomId}/heartbeat',
  apiRoomLeavePath: '/notap/rooms/{roomId}/leave',
  apiRoomIcePath: '/notap/rooms/{roomId}/ice',
  apiRoomPeerProbesPath: '/notap/rooms/{roomId}/peer-probes',
  diagnosticLog: 'false',
  upnp: 'true',
}

export const runtimeConfig: RuntimeConfig = { ...fallback, ...(window.welNoTapConfig ?? {}) }

export function route(template: string, roomId: number) {
  return template.replace('{roomId}', String(roomId))
}
