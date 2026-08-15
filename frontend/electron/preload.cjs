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
  configureIce: (remoteDescription) => ipcRenderer.invoke('notap-configure-ice', remoteDescription),
  pingIce: (remoteDescription) => ipcRenderer.invoke('notap-ping-ice', remoteDescription),
  pingRelay: () => ipcRenderer.invoke('notap-ping-relay'),
})
