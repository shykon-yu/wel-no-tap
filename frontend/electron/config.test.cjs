const test = require('node:test')
const assert = require('node:assert/strict')
const { parseEnv } = require('./config.cjs')

test('parseEnv reads comments, whitespace, and quoted values', () => {
  assert.deepEqual(parseEnv('\n# comment\nWEL_PLATFORM_NAME = "WEL 测试"\nWEL_API_BASE_URL=http://127.0.0.1:8082/\n'), {
    WEL_PLATFORM_NAME: 'WEL 测试',
    WEL_API_BASE_URL: 'http://127.0.0.1:8082/',
  })
})
