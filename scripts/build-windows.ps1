$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'native'
$output = Join-Path $root 'build\x86'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  throw '请在 x86 Native Tools Command Prompt for Visual Studio 中运行此脚本。'
}

New-Item -ItemType Directory -Force -Path $output | Out-Null

& cl.exe /nologo /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 `
  /LD (Join-Path $source 'welnpt_trace.c') /Fe:(Join-Path $output 'welnpttrace.dll') `
  /link Ws2_32.lib Psapi.lib
if ($LASTEXITCODE -ne 0) { throw 'welnpttrace.dll 编译失败。' }

& cl.exe /nologo /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 `
  /LD (Join-Path $source 'welnpt_hook.c') /Fe:(Join-Path $output 'welnpt.dll') `
  /link Ws2_32.lib Psapi.lib Bcrypt.lib
if ($LASTEXITCODE -ne 0) { throw 'welnpt.dll 编译失败。' }

& cl.exe /nologo /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 `
  (Join-Path $source 'welnpt_relay.c') /Fe:(Join-Path $output 'welnptrelay.exe') `
  /link Ws2_32.lib Bcrypt.lib
if ($LASTEXITCODE -ne 0) { throw 'welnptrelay.exe 编译失败。' }

& cl.exe /nologo /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 `
  (Join-Path $source 'welnpt_launcher.c') /Fe:(Join-Path $output 'welnptgame.exe')
if ($LASTEXITCODE -ne 0) { throw 'welnptgame.exe 编译失败。' }

& cl.exe /nologo /W4 /O2 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 `
  (Join-Path $source 'welnpt_gui.c') /Fe:(Join-Path $output 'WEL无网卡观测工具.exe') `
  /link User32.lib Gdi32.lib Comdlg32.lib Shell32.lib /SUBSYSTEM:WINDOWS
if ($LASTEXITCODE -ne 0) { throw 'WEL无网卡观测工具.exe 编译失败。' }

& cl.exe /nologo /W4 /O2 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 `
  (Join-Path $source 'welnpt_connect_gui.c') /Fe:(Join-Path $output 'WEL无网卡联机测试.exe') `
  /link User32.lib Gdi32.lib Comdlg32.lib Shell32.lib /SUBSYSTEM:WINDOWS
if ($LASTEXITCODE -ne 0) { throw 'WEL无网卡联机测试.exe 编译失败。' }

Write-Host "构建完成：$output"
