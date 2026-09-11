const test = require('node:test')
const assert = require('node:assert/strict')
const wireguard = require('./wireguard.cjs')

test('reports the WireGuard runtime as unavailable when no bundled binaries exist', () => {
  const result = wireguard.status()
  assert.equal(result.available, false)
  assert.equal(result.adapterReady, false)
})

test('falls back to the existing relay path without peer configuration', async () => {
  const result = await wireguard.connectPeer({ logicalIp: '10.222.7.11' })
  assert.equal(result.path, 'relay')
  assert.equal(wireguard.transportStatus().path, 'relay')
})

test('derives the rendezvous address for both WireGuard room subnets', () => {
  assert.equal(wireguard.serverVirtualIp('10.222.7.0/24'), '10.222.7.1')
  assert.equal(wireguard.serverVirtualIp('10.222.8.0/24'), '10.222.8.1')
})

test('parses a bridge game-peer event into the shared renderer event format', () => {
  assert.deepEqual(wireguard.parseGamePeerLine('GAME_PEER 10.222.7.12|57320|57321|4'), {
    logicalIp: '10.222.7.12',
    transactionKey: '10.222.7.12|57320|57321|4',
  })
})

test('rejects malformed bridge game-peer events', () => {
  assert.equal(wireguard.parseGamePeerLine('GAME_PEER 10.222.7.999|57320|57321|4'), null)
  assert.equal(wireguard.parseGamePeerLine('GAME_PEER 10.222.7.12|0|57321|4'), null)
  assert.equal(wireguard.parseGamePeerLine('GAME_PEER 10.222.7.12|57320|57321|0'), null)
  assert.equal(wireguard.parseGamePeerLine('STATE connected'), null)
})

test('keeps the WireGuard runtime lookup independent from the bridge directory', () => {
  // The packaged bridge lives under welhelper while official WireGuard tools
  // are normally installed under Program Files\WireGuard. The lookup is
  // intentionally exposed through status() and must not require one folder
  // to contain all three executables.
  const result = wireguard.status()
  assert.ok(Object.hasOwn(result, 'bridge'))
  assert.ok(Object.hasOwn(result, 'wireguard'))
  assert.ok(Object.hasOwn(result, 'wg'))
})
