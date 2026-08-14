import type { RuntimeConfig } from './config'

export type DesktopLeaseStatus = {
  ready: boolean
  connected: boolean
  message: string
  actualIp?: string | null
  warnings?: string[]
}

export type PingResult = {
  host: string
  reachable: boolean
  summary: string
}

declare global {
  interface Window {
    welNoTapConfig?: RuntimeConfig
    welNoTapDesktop?: {
      desktopStatus: () => Promise<DesktopLeaseStatus>
      chooseGame: () => Promise<string | null>
      launchGame: (options: { gamePath: string; relay: string; room: string; logicalIp: string; token: string }) => Promise<{ started: boolean; detail: string; warnings?: string[] }>
      disconnect: () => Promise<{ stopped: boolean }>
      pingHost: (host: string) => Promise<PingResult>
    }
  }
}

export {}
