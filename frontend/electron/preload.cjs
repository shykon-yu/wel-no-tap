const { contextBridge, ipcRenderer } = require('electron')

const runtimeConfig = ipcRenderer.sendSync('get-runtime-config')

contextBridge.exposeInMainWorld('welNoTapConfig', runtimeConfig)
contextBridge.exposeInMainWorld('welNoTapDesktop', {
  desktopStatus: () => ipcRenderer.invoke('notap-status'),
  chooseGame: () => ipcRenderer.invoke('notap-choose-game'),
  launchGame: (options) => ipcRenderer.invoke('notap-launch-game', options),
  disconnect: () => ipcRenderer.invoke('notap-disconnect'),
  pingHost: (host) => ipcRenderer.invoke('notap-ping', host),
})
