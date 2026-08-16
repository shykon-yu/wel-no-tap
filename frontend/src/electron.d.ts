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
  direct: PingPathResult
}

export type TransportPingResult = {
  relay: PingResult
  direct: PingResult
}

declare global {
  interface Window {
    welNoTapConfig?: RuntimeConfig
    welNoTapDesktop?: {
      desktopStatus: () => Promise<DesktopLeaseStatus>
      ensureFirewall: (options: { gamePath?: string }) => Promise<{ state: string; warning?: string; missing?: Array<{ name: string }>; blockers?: Array<{ name: string }> }>
      transportStatus: () => Promise<{ path: 'pending' | 'relay' | 'direct'; directState: string; summary: string }>
      chooseGame: () => Promise<string | null>
      launchGame: (options: { gamePath: string; relay: string; room: string; logicalIp: string; token: string }) => Promise<{ started: boolean; detail: string; warnings?: string[] }>
      disconnect: () => Promise<{ stopped: boolean }>
      onBeforeQuit: (callback: () => void) => () => void
      completeQuit: () => Promise<void>
      pingHost: (host: string) => Promise<PingResult>
      prepareIce: (options: { stunHost: string; stunPort: number; relay: string; room: string; logicalIp: string; token: string }) => Promise<{ localDescription: string; directState: string; agentPort: number; hookPort: number }>
      configureIce: (options: { remoteDescription: string; remoteIp: string }) => Promise<boolean>
      createProbeIce: (options: { stunHost: string; stunPort: number }) => Promise<{ probeKey: string; localDescription: string }>
      configureProbeIce: (probeKey: string, remoteDescription: string) => Promise<boolean>
      pingProbeIce: (probeKey: string) => Promise<number>
      stopProbeIce: (probeKey: string) => Promise<{ stopped: boolean }>
      onGamePeer: (callback: (logicalIp: string) => void) => () => void
      pingRelay: () => Promise<number>
      pingRelayPeer: (remoteIp: string) => Promise<number>
    }
  }
}

export {}
