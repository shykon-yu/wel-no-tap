import type { RuntimeConfig } from './config'

export type DesktopLeaseStatus = {
  ready: boolean
  connected: boolean
  message: string
  actualIp?: string | null
  warnings?: string[]
}

export type PingPathResult = {
  reachable: boolean
  summary: string
}

export type PingResult = {
  host: string
  reachable: boolean
  summary: string
  relayServer: PingPathResult
  relayPeer: PingPathResult
}

export type TransportPingResult = {
  relay: PingResult
}

declare global {
  interface Window {
    welNoTapConfig?: RuntimeConfig
    welNoTapDesktop?: {
      desktopStatus: () => Promise<DesktopLeaseStatus>
      ensureFirewall: (options: { gamePath?: string }) => Promise<{ state: string; warning?: string; missing?: Array<{ name: string }>; blockers?: Array<{ name: string }> }>
      transportStatus: () => Promise<{ path: 'pending' | 'relay' | 'direct'; directState: string; summary: string }>
      chooseGame: () => Promise<string | null>
      launchGame: (options: { gamePath: string; relay: string; room: string; logicalIp: string; token: string; direct: boolean }) => Promise<{ started: boolean; detail: string; warnings?: string[] }>
      disconnect: () => Promise<{ stopped: boolean }>
      onBeforeQuit: (callback: () => void) => () => void
      completeQuit: () => Promise<void>
      pingHost: (host: string) => Promise<PingResult>
      prepareIce: (options: { stunHost: string; stunPort: number; relay: string; room: string; logicalIp: string; token: string }) => Promise<{ localDescription: string; directState: string; agentPort: number; hookPort: number }>
      prepareGameIce: () => Promise<{ localDescription: string; directState: string; agentPort: number; hookPort: number }>
      resetIce: () => Promise<{ localDescription: string; directState: string; agentPort: number; hookPort: number }>
      prewarmIce: () => Promise<{ ready: boolean; state: string; localDescription?: string; error?: string }>
      activateIce: () => Promise<{ localDescription: string; directState: string; agentPort: number; hookPort: number } | null>
      configureIce: (options: { remoteDescription: string; remoteIp: string }) => Promise<boolean>
      onGamePeer: (callback: (event: { logicalIp: string; transactionKey: string }) => void) => () => void
      pingRelay: () => Promise<number>
      pingRelayPeer: (remoteIp: string) => Promise<number>
    }
  }
}

export {}
