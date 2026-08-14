const { app, BrowserWindow, Menu, Tray, nativeImage, dialog, ipcMain } = require('electron')
const fs = require('node:fs')
const path = require('node:path')
const { pathToFileURL } = require('node:url')
const { version: appVersion } = require('../package.json')
const { publicConfig } = require('./config.cjs')
const notap = require('./notap.cjs')

if (process.platform === 'win32') app.commandLine.appendSwitch('no-sandbox')

const runtime = publicConfig()
const logDirectory = path.join(process.env.LOCALAPPDATA || app.getPath('userData'), 'WELPlatform', 'logs')
const logFile = path.join(logDirectory, 'main.log')
let mainWindow = null
let tray = null
let isQuitting = false

function writeLog(message, error) {
  try {
    fs.mkdirSync(logDirectory, { recursive: true })
    const detail = error instanceof Error ? error.stack || error.message : String(error || '')
    fs.appendFileSync(logFile, '[' + new Date().toISOString() + '] ' + message + (detail ? '\n' + detail : '') + '\n', 'utf8')
  } catch {}
}

function showFatalError(error) {
  writeLog('应用发生致命错误', error)
  dialog.showErrorBox(runtime.platformName + '启动失败', String(error instanceof Error ? error.message : error || '未知错误') + '\n\n错误日志：' + logFile)
}

function frontendEntryPath() {
  const packagedEntry = path.join(process.resourcesPath, 'frontend', 'index.html')
  return app.isPackaged && fs.existsSync(packagedEntry) ? packagedEntry : path.join(__dirname, '..', 'dist', 'index.html')
}

function showMainWindow() {
  if (!mainWindow || mainWindow.isDestroyed()) return
  if (mainWindow.isMinimized()) mainWindow.restore()
  mainWindow.show()
  mainWindow.focus()
}

function trayIconPath() {
  const candidates = app.isPackaged
    ? [path.join(process.resourcesPath, 'welhelper', 'wel.ico')]
    : [path.join(__dirname, '..', 'build', 'icon.ico')]
  return candidates.find((candidate) => fs.existsSync(candidate)) || null
}

function createTray() {
  if (process.platform !== 'win32' || tray) return
  const iconPath = trayIconPath()
  if (!iconPath) return
  const icon = nativeImage.createFromPath(iconPath)
  if (icon.isEmpty()) return
  tray = new Tray(icon)
  tray.setToolTip(runtime.platformName + ' v' + appVersion)
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: '打开主界面', click: showMainWindow },
    { type: 'separator' },
    { label: '退出平台', click: () => { isQuitting = true; app.quit() } },
  ]))
  tray.on('double-click', showMainWindow)
}

function createChineseMenu() {
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    { label: '文件', submenu: [{ role: 'reload', label: '重新载入' }, { type: 'separator' }, { role: 'quit', label: '退出' }] },
    { label: '编辑', submenu: [{ role: 'cut', label: '剪切' }, { role: 'copy', label: '复制' }, { role: 'paste', label: '粘贴' }, { role: 'selectAll', label: '全选' }] },
    { label: '查看', submenu: [{ role: 'resetZoom', label: '实际大小' }, { role: 'zoomIn', label: '放大' }, { role: 'zoomOut', label: '缩小' }, { type: 'separator' }, { role: 'togglefullscreen', label: '全屏' }] },
    { label: '帮助', submenu: [{ label: '关于', click: () => dialog.showMessageBox({ type: 'info', title: '关于', message: runtime.platformName + ' v' + appVersion }) }] },
  ]))
}

function chooseGame(event) {
  return dialog.showOpenDialog(BrowserWindow.fromWebContents(event.sender), {
    title: '选择 ' + runtime.gameName + ' 游戏程序',
    properties: ['openFile'],
    filters: [{ name: runtime.gameName + ' 游戏程序', extensions: ['exe'] }],
  }).then((result) => result.canceled ? null : result.filePaths[0] || null)
}

ipcMain.on('get-runtime-config', (event) => { event.returnValue = runtime })
ipcMain.handle('notap-status', () => notap.status())
ipcMain.handle('notap-disconnect', () => notap.disconnect())
ipcMain.handle('notap-ping', (_event, host) => notap.pingHost(host))
ipcMain.handle('notap-choose-game', chooseGame)
ipcMain.handle('notap-launch-game', (_event, options) => notap.launch(options))

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1180, height: 760, minWidth: 900, minHeight: 620,
    title: runtime.platformName + ' v' + appVersion,
    backgroundColor: '#f4f7f6',
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  })
  mainWindow.on('close', (event) => {
    if (isQuitting || process.platform !== 'win32') return
    event.preventDefault()
    mainWindow.hide()
  })
  mainWindow.on('closed', () => { mainWindow = null })
  mainWindow.webContents.on('render-process-gone', (_event, details) => writeLog('渲染进程退出：' + details.reason + '，代码 ' + details.exitCode))
  const entryUrl = pathToFileURL(frontendEntryPath()).toString()
  writeLog('正在加载前端页面：' + entryUrl)
  mainWindow.loadURL(entryUrl).catch(showFatalError)
}

process.on('uncaughtException', showFatalError)
process.on('unhandledRejection', showFatalError)

app.whenReady().then(() => {
  createChineseMenu()
  createWindow()
  createTray()
  writeLog(runtime.platformName + '已启动，配置文件：' + (runtime.configFile || '内置默认值'))
}).catch((error) => {
  showFatalError(error)
  app.quit()
})

app.on('before-quit', () => { isQuitting = true })
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit() })
