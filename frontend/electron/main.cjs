const { app, BrowserWindow, Menu, Tray, nativeImage, dialog, ipcMain, shell } = require('electron')
const fs = require('node:fs')
const path = require('node:path')
const { pathToFileURL } = require('node:url')
const { version: appVersion } = require('../package.json')
const { publicConfig } = require('./config.cjs')
const notap = require('./notap.cjs')
const firewall = require('./firewall.cjs')

if (process.platform === 'win32') app.commandLine.appendSwitch('no-sandbox')

const runtime = { ...publicConfig(), appVersion }
const logDirectory = path.join(process.env.LOCALAPPDATA || app.getPath('userData'), 'WELPlatform', 'logs')
const logFile = path.join(logDirectory, 'main.log')
let mainWindow = null
let tray = null
let isQuitting = false
let quitTimer = null
const firewallConsentAsked = new Map()
const firewallBlockNoticeShown = new Map()
const firewallPromptCooldownMs = 10000

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

function finishQuit() {
  if (quitTimer) clearTimeout(quitTimer)
  quitTimer = null
  isQuitting = true
  app.quit()
}

function requestGracefulQuit() {
  if (isQuitting) return
  if (!mainWindow || mainWindow.isDestroyed()) return finishQuit()
  mainWindow.webContents.send('platform-before-quit')
  quitTimer = setTimeout(finishQuit, 5000)
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
    { label: '退出平台', click: requestGracefulQuit },
  ]))
  tray.on('double-click', showMainWindow)
}

function createChineseMenu() {
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    { label: '文件', submenu: [{ role: 'reload', label: '重新载入' }, { type: 'separator' }, { label: '退出', click: requestGracefulQuit }] },
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

function isHookInjectionFailure(error) {
  const message = String(error instanceof Error ? error.message : error || '')
  return message.includes('Hook module injection failed') ||
    message.includes('Hook module did not initialize') ||
    message.includes('QueueUserAPC') ||
    message.includes('CreateRemoteThread')
}

function isElevationRequired(error) {
  return String(error instanceof Error ? error.message : error || '').includes('Windows error 740')
}

async function openWindowsSecurity() {
  try {
    await shell.openExternal('windowsdefender://threat/')
    return
  } catch {}
  try {
    const child = require('node:child_process').spawn('control.exe', ['/name', 'Microsoft.ActionCenter'], {
      detached: true,
      stdio: 'ignore',
      windowsHide: false,
    })
    child.unref()
  } catch {}
}

function openFirewallSettings() {
  try {
    const child = require('node:child_process').spawn('control.exe', ['firewall.cpl'], {
      detached: true,
      stdio: 'ignore',
      windowsHide: false,
    })
    child.unref()
  } catch {}
}

function firewallResultMessage(result) {
  const missing = (result.missing || []).map((rule) => rule.name).join('、')
  const blockers = (result.blockers || []).map((rule) => rule.name + (rule.program ? ` (${rule.program})` : '')).join('\n')
  if (blockers) return `发现 Windows 防火墙存在阻止规则，请删除后重新进入房间：\n${blockers}`
  if (missing) return `以下 UDP 放行规则尚未生效：${missing}`
  return 'Windows 防火墙规则状态无法确认，请检查系统防火墙设置。'
}

async function ensureWindowsFirewall(event, options = {}) {
  if (process.platform !== 'win32') return { state: 'not-needed', warning: '' }
  const status = notap.status()
  const paths = { icePath: status.icePath }
  let result = await firewall.trySilentFirewall(paths)
  if (result.state === 'ready' || result.state === 'not-needed') return { ...result, warning: '' }

  const owner = BrowserWindow.fromWebContents(event.sender)
  const key = [paths.icePath, paths.gamePath].filter(Boolean).map((value) => String(value).toLowerCase()).join('|')
  if (result.blockers?.length) {
    const lastBlockNotice = firewallBlockNoticeShown.get(key) || 0
    if (Date.now() - lastBlockNotice >= firewallPromptCooldownMs) {
      firewallBlockNoticeShown.set(key, Date.now())
      const prompt = await dialog.showMessageBox(owner, {
        type: 'warning',
        title: '发现防火墙阻止规则',
        message: 'Windows 防火墙中存在会阻止 WEL 直连或游戏入站数据的规则。',
        detail: firewallResultMessage(result) + '\n\n请在防火墙高级设置的“入站规则”中删除这些阻止规则，或确认规则已禁用。当前仍可进入房间，但直连可能不可用。',
        buttons: ['打开防火墙设置', '继续进入'],
        defaultId: 0,
        cancelId: 1,
        noLink: true,
      })
      if (prompt.response === 0) openFirewallSettings()
    }
    return { ...result, warning: firewallResultMessage(result) }
  }

  const lastConsentPrompt = firewallConsentAsked.get(key) || 0
  if (Date.now() - lastConsentPrompt >= firewallPromptCooldownMs) {
    firewallConsentAsked.set(key, Date.now())
    const prompt = await dialog.showMessageBox(owner, {
      type: 'warning',
      title: '需要允许 WEL 网络规则',
      message: 'WEL 需要为直连 ICE 组件写入 Windows 防火墙 UDP 放行规则。',
      detail: '平台已先尝试静默写入，但当前系统没有确认规则生效。点击“允许并修复”后，Windows 可能显示管理员授权提示。拒绝后仍可进入房间，但直连可能不可用。',
      buttons: ['允许并修复', '继续进入'],
      defaultId: 0,
      cancelId: 1,
      noLink: true,
    })
    if (prompt.response === 0) result = await firewall.applyElevatedFirewall(paths)
  }
  if (result.state === 'ready') return { ...result, warning: '' }
  return { ...result, warning: firewallResultMessage(result) + '\n当前仍可进入房间，直连失败时将继续使用中继。' }
}

async function launchGameWithRecovery(event, options) {
  try {
    return await notap.launch(options)
  } catch (error) {
    if (isElevationRequired(error)) {
      const owner = BrowserWindow.fromWebContents(event.sender)
      const result = await dialog.showMessageBox(owner, {
        type: 'info',
        title: '游戏需要管理员权限',
        message: '该 WE8.exe 被 Windows 设置为必须使用管理员权限运行。',
        detail: '点击“允许并启动”后会出现 Windows 用户账户控制提示。平台本身无需重新启动。',
        buttons: ['允许并启动', '取消'],
        defaultId: 0,
        cancelId: 1,
        noLink: true,
      })
      if (result.response === 0) return notap.launchElevated(options)
      throw new Error('已取消管理员授权，游戏未启动')
    }
    if (!isHookInjectionFailure(error)) throw error
    const owner = BrowserWindow.fromWebContents(event.sender)
    const exactError = String(error instanceof Error ? error.message : error || '未知错误')
    writeLog('常规与 APC Hook 注入均未成功', error)
    const result = await dialog.showMessageBox(owner, {
      type: 'warning',
      title: '游戏 Hook 被系统拦截',
      message: '平台已自动尝试常规注入和 APC 兼容模式，但 Windows 或安全软件仍然拒绝加载 Hook。',
      detail: '可以先重试一次；若仍失败，请打开安全中心的“保护历史记录”，允许 welnptgame.exe、welnpt.dll 和 WE8.exe 后重新启动游戏。\n\n准确错误：' + exactError,
      buttons: ['重新尝试', '打开 Windows 安全中心', '取消'],
      defaultId: 0,
      cancelId: 2,
      noLink: true,
    })
    if (result.response === 0) return notap.launch(options)
    if (result.response === 1) {
      await openWindowsSecurity()
      throw new Error('已打开 Windows 安全中心。请在保护历史记录中允许相关程序，然后重新点击启动游戏。\n原始错误：' + exactError)
    }
    throw error
  }
}

ipcMain.on('get-runtime-config', (event) => { event.returnValue = runtime })
ipcMain.handle('notap-status', () => notap.status())
ipcMain.handle('notap-ensure-firewall', (event, options) => ensureWindowsFirewall(event, options))
ipcMain.handle('notap-transport-status', () => notap.transportStatus())
ipcMain.handle('notap-disconnect', () => notap.disconnect())
ipcMain.handle('notap-ping', (_event, host) => notap.pingHost(host))
ipcMain.handle('notap-prepare-ice', (_event, options) => notap.prepareIce(options))
ipcMain.handle('notap-configure-ice', (_event, options) => notap.configureIce(options?.remoteDescription, options?.remoteIp))
ipcMain.handle('notap-create-probe-ice', (_event, options) => notap.createProbeIce(options))
ipcMain.handle('notap-configure-probe-ice', (_event, probeKey, remoteDescription) => notap.configureProbeIce(probeKey, remoteDescription))
ipcMain.handle('notap-ping-probe-ice', (_event, probeKey) => notap.pingProbeIce(probeKey))
ipcMain.handle('notap-stop-probe-ice', (_event, probeKey) => notap.stopProbeIce(probeKey))
ipcMain.handle('notap-ping-relay', () => notap.pingRelay())
ipcMain.handle('notap-ping-relay-peer', (_event, remoteIp) => notap.pingRelayPeer(remoteIp))
ipcMain.handle('notap-choose-game', chooseGame)
ipcMain.handle('notap-launch-game', launchGameWithRecovery)
ipcMain.handle('platform-complete-quit', finishQuit)

notap.onGamePeer((logicalIp) => {
  if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send('notap-game-peer', logicalIp)
})

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
