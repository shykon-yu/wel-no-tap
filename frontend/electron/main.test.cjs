const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')

const client = fs.readFileSync(path.join(__dirname, 'main.cjs'), 'utf8')

test('configures the TAP game firewall before starting WE8', () => {
  assert.match(client, /const \{ ensureWe8Firewall \} = require\('\.\/tap-firewall\.cjs'\)/)
  assert.match(client, /const firewallResult = await ensureWe8Firewall\(options\.gamePath\)/)
  assert.match(client, /ensureWe8Firewall\(options\.gamePath\)[\s\S]*tapGame\.launchGameBound\(options\.gamePath, tap\.activeNetwork\(\)\)/)
})
