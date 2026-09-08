const test = require('node:test')
const assert = require('node:assert/strict')
const { classifyN2NPeers } = require('./tap.cjs')

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
