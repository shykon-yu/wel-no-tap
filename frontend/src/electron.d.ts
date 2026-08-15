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
  relay: PingPathResult
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
      transportStatus: () => Promise<{ path: 'relay' | 'direct'; directState: string; summary: string }>
      chooseGame: () => Promise<string | null>
      launchGame: (options: { gamePath: string; relay: string; room: string; logicalIp: string; token: string; remoteIp?: string; remoteDescription?: string }) => Promise<{ started: boolean; detail: string; warnings?: string[] }>
      disconnect: () => Promise<{ stopped: boolean }>
      onBeforeQuit: (callback: () => void) => () => void
      completeQuit: () => Promise<void>
      pingHost: (host: string) => Promise<PingResult>
      prepareIce: (options: { stunHost: string; stunPort: number; relay: string; room: string; logicalIp: string; token: string }) => Promise<{ localDescription: string; directState: string; agentPort: number; hookPort: number }>
      configureIce: (remoteDescription: string) => Promise<boolean>
      pingIce: (remoteDescription: string) => Promise<number>
      pingRelay: () => Promise<number>
    }
  }
}

export {}
