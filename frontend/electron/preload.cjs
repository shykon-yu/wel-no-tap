const { contextBridge, ipcRenderer } = require('electron')

const runtimeConfig = ipcRenderer.sendSync('get-runtime-config')

contextBridge.exposeInMainWorld('welNoTapConfig', runtimeConfig)
contextBridge.exposeInMainWorld('welNoTapDesktop', {
  desktopStatus: () => ipcRenderer.invoke('notap-status'),
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
  configureIce: (options) => ipcRenderer.invoke('notap-configure-ice', options),
  createProbeIce: (options) => ipcRenderer.invoke('notap-create-probe-ice', options),
  configureProbeIce: (probeKey, remoteDescription) => ipcRenderer.invoke('notap-configure-probe-ice', probeKey, remoteDescription),
  pingProbeIce: (probeKey) => ipcRenderer.invoke('notap-ping-probe-ice', probeKey),
  stopProbeIce: (probeKey) => ipcRenderer.invoke('notap-stop-probe-ice', probeKey),
  onGamePeer: (callback) => {
    const listener = (_event, logicalIp) => callback(logicalIp)
    ipcRenderer.on('notap-game-peer', listener)
    return () => ipcRenderer.removeListener('notap-game-peer', listener)
  },
  pingRelay: () => ipcRenderer.invoke('notap-ping-relay'),
  pingRelayPeer: (remoteIp) => ipcRenderer.invoke('notap-ping-relay-peer', remoteIp),
})
