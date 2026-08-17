const { contextBridge, ipcRenderer } = require('electron')

const runtimeConfig = ipcRenderer.sendSync('get-runtime-config')

contextBridge.exposeInMainWorld('welNoTapConfig', runtimeConfig)
contextBridge.exposeInMainWorld('welNoTapDesktop', {
  desktopStatus: () => ipcRenderer.invoke('notap-status'),
  ensureFirewall: (options) => ipcRenderer.invoke('notap-ensure-firewall', options),
  transportStatus: () => ipcRenderer.invoke('notap-transport-status'),
  chooseGame: () => ipcRenderer.invoke('notap-choose-game'),
  launchGame: (options) => ipcRenderer.invoke('notap-launch-game', options),
  disconnect: () => ipcRenderer.invoke('notap-disconnect'),
  onBeforeQuit: (callback) => {
    const listener = () => callback()
    ipcRenderer.on('platform-before-quit', listener)
    return () => ipcRenderer.removeListener('platform-before-quit', listener)
  },
  completeQuit: () => ipcRenderer.invoke('platform-complete-quit'),
  pingHost: (host) => ipcRenderer.invoke('notap-ping', host),
  prepareIce: (options) => ipcRenderer.invoke('notap-prepare-ice', options),
  prepareGameIce: () => ipcRenderer.invoke('notap-prepare-game-ice'),
  resetIce: () => ipcRenderer.invoke('notap-reset-ice'),
  prewarmIce: () => ipcRenderer.invoke('notap-prewarm-ice'),
  activateIce: () => ipcRenderer.invoke('notap-activate-ice'),
  configureIce: (options) => ipcRenderer.invoke('notap-configure-ice', options),
  onGamePeer: (callback) => {
    const listener = (_event, gamePeer) => callback(gamePeer)
    ipcRenderer.on('notap-game-peer', listener)
    return () => ipcRenderer.removeListener('notap-game-peer', listener)
  },
  pingRelay: () => ipcRenderer.invoke('notap-ping-relay'),
  pingRelayPeer: (remoteIp) => ipcRenderer.invoke('notap-ping-relay-peer', remoteIp),
})
