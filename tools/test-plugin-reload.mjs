// POSIX live-core regression; isolated data, no bot accounts or external services.
// Usage: node tools/test-plugin-reload.mjs <core-binary> [lua-file]
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { fileURLToPath } from 'node:url';
import { randomUUID } from 'node:crypto';
import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';

if (process.platform === 'win32') throw new Error('This smoke driver tests POSIX graceful shutdown; run CTest on Windows.');
if (process.argv.length < 3 || process.argv.length > 4)
  throw new Error('Usage: node tools/test-plugin-reload.mjs <core-binary> [lua-file]');
const core = path.resolve(process.argv[2]);
const resources = fileURLToPath(new URL('../server/', import.meta.url));
const lua = process.argv[3] ? fs.readFileSync(process.argv[3]) : Buffer.from(`
msg_order = {}
function reload_probe(msg) return "reload-probe-ok" end
msg_order["reload-probe"] = "reload_probe"
`);
const filename = process.argv[3] ? path.basename(process.argv[3]) : 'reload-probe.lua';
assert.equal(path.extname(filename).toLowerCase(), '.lua');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dicenext-plugin-reload-'));
const apiKey = randomUUID();
let child, exited, logs = '';
function config(name, value) {
  fs.writeFileSync(path.join(root, 'config', `${name}.json`), JSON.stringify(value));
}
try {
  const probe = net.createServer();
  await new Promise((resolve, reject) => { probe.once('error', reject); probe.listen(0, '127.0.0.1', resolve); });
  const port = probe.address().port;
  await new Promise(resolve => probe.close(resolve));
  fs.mkdirSync(path.join(root, 'config'));
  fs.copyFileSync(core, path.join(root, 'dice-next-server'));
  fs.chmodSync(path.join(root, 'dice-next-server'), 0o755);
  for (const dir of ['i18n', 'rules', 'card-templates'])
    if (fs.existsSync(path.join(resources, dir))) fs.cpSync(path.join(resources, dir), path.join(root, dir), { recursive: true });
  config('server', { host: '127.0.0.1', port, api_key: apiKey, db_path: './data/dice.db' });
  config('adapters', []);
  // Split configuration is validated as a set; missing files would trigger
  // snapshot recovery/defaults and lose the isolated listener/feature flags.
  config('events', {});
  config('webui', {});
  config('i18n', { resource_dir: 'i18n', default_locale: 'zh-Hans' });
  config('dice', { heart_enabled: false, cloudban_enabled: false, console_start_hidden: false });
  config('update', { auto_check: false });
  config('backup', { auto_enabled: false });
  config('hot_reload', { enabled: false });
  child = spawn(path.join(root, 'dice-next-server'), [], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] });
  exited = new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', (code, signal) => resolve({ code, signal }));
  });
  for (const stream of [child.stdout, child.stderr]) stream.on('data', data => { logs = (logs + data).slice(-20000); });
  async function request(endpoint, body) {
    const response = await fetch(`http://127.0.0.1:${port}${endpoint}`, {
      method: body === undefined ? 'GET' : 'POST',
      headers: { 'Content-Type': 'application/json', 'X-API-Key': apiKey },
      body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(5000),
    });
    assert.equal(response.status, 200, endpoint);
    assert.ok(response.headers.get('X-Dice-Request-Id'), 'API response must carry a diagnostic request id');
    return response.json();
  }
  async function ok(endpoint, body) {
    const response = await request(endpoint, body);
    assert.equal(response.code, 0, `${endpoint}: ${response.message}`);
    return response;
  }
  const readyUntil = Date.now() + 30000;
  while (true) {
    try { await request('/api/auth/status'); break; } catch (error) {
      if (child.exitCode !== null || child.signalCode !== null || Date.now() > readyUntil) throw error;
      await delay(100);
    }
  }
  const upload = { filename, content: lua.toString('base64') };
  assert.notEqual((await request('/api/plugins/js/upload', { filename, content: lua.toString() })).code, 0);
  await ok('/api/mod/lua/upload', { ...upload, dry_run: true });
  const installed = await ok('/api/mod/lua/upload', upload);
  const name = installed.data.mods.find(mod => mod.name === path.basename(filename, '.lua'))?.name;
  assert.ok(name, 'Uploaded single-file Lua must appear in the mod list');
  async function healthy() {
    await ok('/api/mod/lua');
    await ok('/api/plugins/js');
    for (const text of ['.r 1', '.ra(1) 急救']) {
      const reply = await request('/api/test/message', { text, userId: '10001', groupId: '100001' });
      assert.ok(reply.matched && reply.reply && !reply.error, JSON.stringify(reply));
    }
  }
  await healthy();
  if (!process.argv[3])
    assert.equal((await request('/api/test/message', { text: 'reload-probe' })).reply, 'reload-probe-ok');
  for (const enabled of [false, true]) {
    await ok('/api/mod/lua/toggle', { name, enabled });
    await healthy();
    if (!process.argv[3])
      assert.equal((await request('/api/test/message', { text: 'reload-probe' })).matched, enabled);
  }
  await ok('/api/mod/lua/reload', {});
  await ok('/api/plugins/js/reload', {});
  await healthy();
  const deleted = await ok('/api/mod/lua/delete', { name });
  assert.ok(!deleted.data.some(mod => mod.name === name));
  await healthy();
  if (!process.argv[3])
    assert.equal((await request('/api/test/message', { text: 'reload-probe' })).matched, false);
  await ok('/api/mod/lua/upload', upload);
  await healthy();
  // Several simultaneous operations exercise the HTTP workers and rules readers.
  await Promise.all([ok('/api/mod/lua/reload', {}), ok('/api/plugins/js/reload', {}), healthy()]);

  // Deliberately fail plugin execution, then check the real file and dashboard
  // reader. The fixture uses no real identities, message content or credentials.
  await ok('/api/plugins/js/upload', { filename: 'diagnostics-probe.js', content: `
const ext = seal.ext.new('diagnostics-probe', 'test', '1');
seal.ext.register(ext);
const cmd = seal.ext.newCmdItemInfo();
cmd.name = 'diagjs';
cmd.solve = () => { function inner() { throw new Error('js-diagnostic-probe'); } inner(); };
ext.cmdMap.diagjs = cmd;
` });
  await request('/api/test/message', { text: '.diagjs' });
  const errorLua = Buffer.from(`
msg_order = {}
function diag_lua(msg) local function inner() error('lua-diagnostic-probe') end inner() end
msg_order["diag-lua"] = "diag_lua"
`);
  await ok('/api/mod/lua/upload', { filename: 'diagnostics-probe.lua', content: errorLua.toString('base64') });
  await request('/api/test/message', { text: 'diag-lua' });
  await healthy();
  const dashboard = await ok('/api/dashboard/stats');
  assert.ok(dashboard.data.recent_logs.some(line => line.level === 'error'), 'Dashboard must still parse error levels');
  child.kill('SIGTERM');
  const exit = await Promise.race([exited, delay(5000).then(() => { throw new Error('Graceful shutdown timed out'); })]);
  assert.equal(exit.code, 0, JSON.stringify(exit));
  const files = fs.readdirSync(path.join(root, 'data/logs/app')).filter(file => file.endsWith('.log'));
  const fileLog = files.map(file => fs.readFileSync(path.join(root, 'data/logs/app', file), 'utf8')).join('\n');
  for (const marker of ['[tz=', 'thread=', 'elapsed_ms=', 'phase=begin', 'phase=end', 'request=',
    'filename must end with .js', 'action=load_plugin',
    'js-diagnostic-probe', 'lua-diagnostic-probe', 'stack traceback:', 'diagnostics-probe.js:'])
    assert.ok(fileLog.includes(marker), `Missing diagnostic marker: ${marker}`);
  assert.ok(!fileLog.includes(apiKey), 'Configured API key must never appear in file logs');
  console.log('PASS: Lua/JS reload regression, concurrent reads, rolls, exception traces, HTTP diagnostics, dashboard parsing, redaction and graceful exit');
} catch (error) {
  console.error(logs);
  throw error;
} finally {
  if (child && child.exitCode === null && child.signalCode === null) { child.kill('SIGKILL'); await exited; }
  fs.rmSync(root, { recursive: true, force: true }); // Only the mkdtemp fixture created above.
}
