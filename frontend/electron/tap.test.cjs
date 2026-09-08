const test = require('node:test')
const assert = require('node:assert/strict')
const {
  classifyN2NPeers,
  parseTapctlList,
  parsePingSummary,
  parseWmiTapAdapters,
  selectWelTapAdapter,
} = require('./tap.cjs')

test('classifies an n2n peer-to-peer edge as direct', () => {
  assert.deepEqual(classifyN2NPeers([{ mode: 'p2p' }]), {
    path: 'direct', peers: 1, directPeers: 1, relayPeers: 0,
  })
})

test('classifies a supernode-forwarded edge as relay', () => {
  assert.deepEqual(classifyN2NPeers([{ mode: 'pSp' }]), {
    path: 'relay', peers: 1, directPeers: 0, relayPeers: 1,
  })
})

test('does not call an empty peer list relay', () => {
  assert.deepEqual(classifyN2NPeers([]), {
    path: 'pending', peers: 0, directPeers: 0, relayPeers: 0,
  })
})

test('reports mixed paths when n2n has direct and relayed peers', () => {
  assert.deepEqual(classifyN2NPeers([{ mode: 'pSp' }, { mode: 'p2p' }]), {
    path: 'mixed', peers: 2, directPeers: 1, relayPeers: 1,
  })
})

test('keeps the TAP description separate from the localized Windows connection name', () => {
  const guid = '12345678-1234-1234-1234-1234567890ab'
  const encode = (value) => Buffer.from(value, 'utf8').toString('base64')
  const adapters = parseWmiTapAdapters([
    [guid, '以太网 2', 'TAP-Windows Adapter V9'].map(encode).join('|'),
  ].join('\n'))

  assert.deepEqual(adapters[0], {
    guid: `{${guid}}`,
    name: '以太网 2',
    connectionName: '以太网 2',
    description: 'TAP-Windows Adapter V9',
    isTap: true,
  })
  assert.equal(selectWelTapAdapter(adapters).connectionName, '以太网 2')
})

test('tapctl GUID results remain trusted TAP devices even when the name is localized', () => {
  const guid = '12345678-1234-1234-1234-1234567890ac'
  const adapter = parseTapctlList(`{${guid}} Ethernet 2`)[0]
  assert.equal(adapter.isTap, true)
  assert.equal(selectWelTapAdapter([adapter]).guid, `{${guid}}`)
})

test('does not infer TAP from a generic connection alias alone', () => {
  const guid = '12345678-1234-1234-1234-1234567890ad'
  assert.equal(selectWelTapAdapter([{
    guid: `{${guid}}`,
    name: '以太网 2',
    connectionName: '以太网 2',
    description: null,
  }]), null)
})

test('parses Windows Ping output for a TAP peer', () => {
  assert.deepEqual(parsePingSummary('10.222.1.11', '来自 10.222.1.11 的回复: 字节=32 时间<1ms TTL=128\n    丢失 = 0 (0% 丢失),\n平均 = 0ms'), {
    host: '10.222.1.11',
    reachable: true,
    summary: '可达，平均 0ms，丢包 0%',
  })
  assert.deepEqual(parsePingSummary('10.222.1.11', 'Request timed out.\n    Lost = 4 (100% loss),'), {
    host: '10.222.1.11',
    reachable: false,
    summary: '不可达，丢包 100%',
  })
})
