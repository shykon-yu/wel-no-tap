const fs = require('node:fs')
const path = require('node:path')

const defaults = {
  WEL_PLATFORM_NAME: 'WEL对战平台',
  WEL_PLATFORM_SHORT_NAME: 'WEL',
  WEL_API_BASE_URL: 'http://8.155.145.132:8082/api/v1',
  WEL_API_LOGIN_PATH: '/auth/login',
  WEL_API_LOGOUT_PATH: '/auth/logout',
  WEL_API_ME_PATH: '/me',
  WEL_API_ROOM_SESSION_PATH: '/notap/me/room-session',
  WEL_API_ROOMS_PATH: '/notap/rooms',
  WEL_API_ROOM_MEMBERS_PATH: '/notap/rooms/{roomId}/members',
  WEL_API_ROOM_JOIN_PATH: '/notap/rooms/{roomId}/join',
  WEL_API_ROOM_HEARTBEAT_PATH: '/notap/rooms/{roomId}/heartbeat',
  WEL_API_ROOM_LEAVE_PATH: '/notap/rooms/{roomId}/leave',
  WEL_API_ROOM_ICE_PATH: '/notap/rooms/{roomId}/ice',
  WEL_API_ROOM_PEER_PROBES_PATH: '/notap/rooms/{roomId}/peer-probes',
  WEL_GAME_NAME: 'WE8',
}

function parseEnv(contents) {
  const values = {}
  for (const rawLine of String(contents || '').replace(/^\uFEFF/, '').split(/\r?\n/)) {
    const line = rawLine.trim()
    if (!line || line.startsWith('#')) continue
    const separator = line.indexOf('=')
    if (separator < 1) continue
    const key = line.slice(0, separator).trim()
    let value = line.slice(separator + 1).trim()
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
      value = value.slice(1, -1)
    }
    values[key] = value
  }
  return values
}

function configCandidates() {
  const executableDirectory = path.dirname(process.execPath)
  return [
    path.join(executableDirectory, 'wel-no-tap.env'),
    path.join(executableDirectory, '.env'),
    path.join(process.resourcesPath || '', 'wel-no-tap.env'),
    path.join(process.resourcesPath || '', '.env'),
    path.join(__dirname, '..', 'wel-no-tap.env'),
    path.join(process.cwd(), 'wel-no-tap.env'),
    path.join(process.cwd(), '.env'),
  ].filter(Boolean)
}

function loadConfig() {
  const filePath = configCandidates().find((candidate) => fs.existsSync(candidate)) || null
  let values = {}
  if (filePath) {
    try { values = parseEnv(fs.readFileSync(filePath, 'utf8')) } catch { values = {} }
  }
  const merged = { ...defaults, ...values }
  merged.WEL_API_BASE_URL = merged.WEL_API_BASE_URL.replace(/\/+$/, '')
  return { values: merged, filePath }
}

function publicConfig() {
  const { values, filePath } = loadConfig()
  return {
    platformName: values.WEL_PLATFORM_NAME,
    platformShortName: values.WEL_PLATFORM_SHORT_NAME,
    gameName: values.WEL_GAME_NAME,
    apiBaseUrl: values.WEL_API_BASE_URL,
    apiLoginPath: values.WEL_API_LOGIN_PATH,
    apiLogoutPath: values.WEL_API_LOGOUT_PATH,
    apiMePath: values.WEL_API_ME_PATH,
    apiRoomSessionPath: values.WEL_API_ROOM_SESSION_PATH,
    apiRoomsPath: values.WEL_API_ROOMS_PATH,
    apiRoomMembersPath: values.WEL_API_ROOM_MEMBERS_PATH,
    apiRoomJoinPath: values.WEL_API_ROOM_JOIN_PATH,
    apiRoomHeartbeatPath: values.WEL_API_ROOM_HEARTBEAT_PATH,
    apiRoomLeavePath: values.WEL_API_ROOM_LEAVE_PATH,
    apiRoomIcePath: values.WEL_API_ROOM_ICE_PATH,
    apiRoomPeerProbesPath: values.WEL_API_ROOM_PEER_PROBES_PATH,
    configFile: filePath,
  }
}

module.exports = { loadConfig, parseEnv, publicConfig }
