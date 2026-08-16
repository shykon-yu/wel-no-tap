const { spawn } = require('node:child_process')
const path = require('node:path')

const RULE_PREFIX = 'WEL No-TAP'
const RULES = [
  { suffix: 'ICE Inbound', direction: 'in' },
  { suffix: 'ICE Outbound', direction: 'out' },
  { suffix: 'WE8 Inbound', direction: 'in' },
  { suffix: 'WE8 Outbound', direction: 'out' },
]

function psLiteral(value) {
  return "'" + String(value).replace(/'/g, "''") + "'"
}

function encodedCommand(script) {
  return Buffer.from(String(script), 'utf16le').toString('base64')
}

function runPowerShell(script, { elevated = false, timeoutMs = 30000 } = {}) {
  const payload = encodedCommand(script)
  let command = ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', payload]
  if (elevated) {
    const argumentsLiteral = psLiteral(command.join(' '))
    const wrapper = `$p = Start-Process -FilePath 'powershell.exe' -ArgumentList ${argumentsLiteral} -Verb RunAs -WindowStyle Hidden -Wait -PassThru; if ($null -eq $p) { exit 1 }; exit $p.ExitCode`
    command = ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', wrapper]
  }
  return new Promise((resolve) => {
    const child = spawn('powershell.exe', command, { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] })
    const output = []
    const timer = setTimeout(() => {
      try { child.kill() } catch {}
      resolve({ code: 124, output: output.join('') })
    }, timeoutMs)
    child.stdout.on('data', (chunk) => output.push(chunk.toString('utf8')))
    child.stderr.on('data', (chunk) => output.push(chunk.toString('utf8')))
    child.once('error', (error) => {
      clearTimeout(timer)
      resolve({ code: 1, output: String(error.message || error) })
    })
    child.once('close', (code) => {
      clearTimeout(timer)
      resolve({ code: code ?? 1, output: output.join('') })
    })
  })
}

function normalizeProgram(value) {
  return path.normalize(String(value || '')).replace(/[\\/]+$/, '').toLowerCase()
}

function ruleName(rule) {
  return `${RULE_PREFIX} ${rule.suffix}`
}

function ruleSpecs({ icePath, gamePath }) {
  const paths = [
    { kind: 'ice', path: icePath },
    { kind: 'game', path: gamePath },
  ].filter((item) => item.path && String(item.path).trim())
  return paths.flatMap((item) => RULES
    .filter((rule) => rule.suffix.startsWith(item.kind === 'ice' ? 'ICE' : 'WE8'))
    .map((rule) => ({ ...rule, name: ruleName(rule), program: path.normalize(String(item.path)) })))
}

function queryScript() {
  return `
$policy = New-Object -ComObject HNetCfg.FwPolicy2
foreach ($rule in $policy.Rules) {
  $program = [string]$rule.ApplicationName
  $name = [string]$rule.Name
  $enabled = if ($rule.Enabled) { '1' } else { '0' }
  Write-Output ('WELFW|' + [int]$rule.Direction + '|' + [int]$rule.Action + '|' + $enabled + '|' + $program + '|' + $name)
}
`
}

async function queryRules() {
  if (process.platform !== 'win32') return []
  const result = await runPowerShell(queryScript(), { timeoutMs: 15000 })
  if (result.code !== 0) return []
  return result.output.split(/\r?\n/).filter((line) => line.startsWith('WELFW|')).map((line) => {
    const [, direction, action, enabled, program, ...nameParts] = line.split('|')
    return {
      direction: Number(direction),
      action: Number(action),
      enabled: enabled === '1',
      program: normalizeProgram(program),
      name: nameParts.join('|'),
    }
  })
}

async function inspectFirewall(options = {}) {
  if (process.platform !== 'win32') return { state: 'not-needed', missing: [], blockers: [], rules: [] }
  const specs = ruleSpecs(options)
  const rules = await queryRules()
  const wantedPrograms = new Set(specs.map((spec) => normalizeProgram(spec.program)))
  const blockers = rules.filter((rule) => rule.enabled && rule.direction === 1 && rule.action === 0 && wantedPrograms.has(rule.program))
  const missing = specs.filter((spec) => !rules.some((rule) => rule.enabled && rule.direction === (spec.direction === 'in' ? 1 : 2) && rule.action === 1 && rule.program === normalizeProgram(spec.program) && rule.name === spec.name))
  return { state: missing.length === 0 && blockers.length === 0 ? 'ready' : 'needs-fix', missing, blockers, rules }
}

function applyScript(specs) {
  const lines = specs.map((spec) => {
    const args = [
      'advfirewall', 'firewall', 'add', 'rule',
      `name=${spec.name}`, `dir=${spec.direction}`, 'action=allow',
      `program=${spec.program}`, 'enable=yes', 'profile=any', 'protocol=UDP',
    ]
    const literalArgs = args.map(psLiteral).join(' ')
    const deleteArgs = ['advfirewall', 'firewall', 'delete', 'rule', `name=${spec.name}`].map(psLiteral).join(' ')
    return `& $netsh ${deleteArgs} | Out-Null; & $netsh ${literalArgs}; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }`
  }).join('\n')
  return `$netsh = Join-Path $env:SystemRoot 'System32\\netsh.exe'\n${lines}`
}

async function trySilentFirewall(options = {}) {
  if (process.platform !== 'win32') return inspectFirewall(options)
  const specs = ruleSpecs(options)
  if (!specs.length) return inspectFirewall(options)
  const before = await inspectFirewall(options)
  if (before.state === 'ready' || before.blockers.length) return before
  await runPowerShell(applyScript(specs), { timeoutMs: 15000 })
  return inspectFirewall(options)
}

async function applyElevatedFirewall(options = {}) {
  if (process.platform !== 'win32') return inspectFirewall(options)
  const specs = ruleSpecs(options)
  if (!specs.length) return inspectFirewall(options)
  const result = await runPowerShell(applyScript(specs), { elevated: true, timeoutMs: 60000 })
  const checked = await inspectFirewall(options)
  if (checked.state === 'ready') return checked
  return { ...checked, elevatedCode: result.code, elevatedOutput: result.output.slice(-1000) }
}

module.exports = { inspectFirewall, trySilentFirewall, applyElevatedFirewall, ruleSpecs }
