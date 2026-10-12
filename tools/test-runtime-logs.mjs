// Node 20+ live-core logging smoke. Only a local fake OneBot peer is connected;
// no real bot account, external API, update, mail or production data is used.
// Usage: node tools/test-runtime-logs.mjs <core-binary>
// Windows requires explicit --allow-windows-listener consent: the temporary
// copied executable may trigger a Windows Defender Firewall permission prompt.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { randomUUID, randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

if (process.platform === 'win32' && !process.argv.includes('--allow-windows-listener')) {
  throw new Error('This smoke test starts a copied core with local HTTP/OneBot listeners and may trigger Windows Defender Firewall prompts. Run only with explicit consent using --allow-windows-listener; no firewall change is required.');
}
const core = path.resolve(process.argv[2] || '');
assert.ok(fs.existsSync(core) && fs.statSync(core).isFile(), 'Pass the built core binary');
const parent = path.dirname(core);
// Windows rejects execution from the OS Temp directory. Use an exact fixture
// beside the build binary, never a live installation or its data directory.
const root = fs.mkdtempSync(path.join(parent, 'dice-runtime-log-smoke-'));
const resources = fileURLToPath(new URL('../server/', import.meta.url));
const apiKey = randomUUID();
let child, exited, peer, output = '';
async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  const port = server.address().port; await new Promise(resolve => server.close(resolve)); return port;
}
async function until(check, timeout = 15000) {
  const end = Date.now() + timeout;
  for (;;) { const value = await check(); if (value) return value; if (Date.now() > end) throw new Error('Smoke condition timed out'); await delay(50); }
}
function localPeer(port) {
  const socket = net.connect(port, '127.0.0.1');
  let buffer = Buffer.alloc(0), upgraded = false;
  const listeners = [];
  const peer = {
    readyState: 0,
    addEventListener(type, callback) { if (type === 'message') listeners.push(callback); },
    send(text) {
      const data = Buffer.from(text), mask = randomBytes(4);
      const header = Buffer.alloc(data.length < 126 ? 2 : 4); header[0] = 0x81;
      if (data.length < 126) header[1] = 0x80 | data.length;
      else { header[1] = 0x80 | 126; header.writeUInt16BE(data.length, 2); }
      const payload = Buffer.from(data); for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
      socket.write(Buffer.concat([header, mask, payload]));
    },
    close() { socket.destroy(); },
  };
  socket.on('connect', () => socket.write('GET / HTTP/1.1\r\nHost: 127.0.0.1:' + port
    + '\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: '
    + randomBytes(16).toString('base64') + '\r\n\r\n'));
  socket.on('error', () => { peer.readyState = 3; });
  socket.on('data', chunk => {
    buffer = Buffer.concat([buffer, chunk]);
    if (!upgraded) {
      const end = buffer.indexOf('\r\n\r\n'); if (end < 0) return;
      assert.ok(buffer.subarray(0, end).toString().startsWith('HTTP/1.1 101'));
      buffer = buffer.subarray(end + 4); upgraded = true; peer.readyState = 1;
    }
    while (buffer.length >= 2) {
      const op = buffer[0] & 15; let length = buffer[1] & 127, offset = 2;
      if (length === 126) { if (buffer.length < 4) return; length = buffer.readUInt16BE(2); offset = 4; }
      if (length === 127) { if (buffer.length < 10) return; length = Number(buffer.readBigUInt64BE(2)); offset = 10; }
      if (buffer.length < offset + length) return;
      const data = buffer.subarray(offset, offset + length).toString(); buffer = buffer.subarray(offset + length);
      if (op === 1) for (const callback of listeners) callback({ data });
    }
  });
  return peer;
}
try {
  const httpPort = await freePort(), wsPort = await freePort();
  const binary = path.join(root, path.basename(core)); fs.copyFileSync(core, binary);
  for (const file of fs.readdirSync(parent)) if (file.endsWith('.dll')) fs.copyFileSync(path.join(parent, file), path.join(root, file));
  if (process.platform !== 'win32') fs.chmodSync(binary, 0o755);
  for (const dir of ['i18n', 'rules', 'card-templates'])
    if (fs.existsSync(path.join(resources, dir))) fs.cpSync(path.join(resources, dir), path.join(root, dir), { recursive: true });
  fs.mkdirSync(path.join(root, 'config'));
  const config = (name, value) => fs.writeFileSync(path.join(root, 'config', name + '.json'), JSON.stringify(value));
  config('server', { host: '127.0.0.1', port: httpPort, api_key: apiKey, db_path: './data/dice.db', log_level: 'info', timezone_minutes: 480 });
  config('adapters', [{ name: '本地日志测试', type: 'onebot_v11', enabled: true, endpoint: String(wsPort), connection_mode: 'reverse_ws' }]);
  for (const name of ['events', 'webui']) config(name, {});
  config('i18n', { resource_dir: 'i18n', default_locale: 'zh-Hans' });
  config('dice', { heart_enabled: false, cloudban_enabled: false, console_start_hidden: true, plugin_timeout_ms: 5000 });
  config('update', { auto_check: false }); config('backup', { auto_enabled: false }); config('hot_reload', { enabled: false });
  fs.mkdirSync(path.join(root, 'data/plugins/js'), { recursive: true });
  fs.mkdirSync(path.join(root, 'data/plugin'), { recursive: true });
  fs.writeFileSync(path.join(root, 'data/plugins/js/origin.js'), `
// ==UserScript==
// @name 日报测试
// ==/UserScript==
const ext = seal.ext.new('logs', 'test', '1');
const cmd = seal.ext.newCmdItemInfo();
cmd.solve = (ctx, msg) => {
  console.warn('plugin-own-warning'); console.debug('hidden-plugin-debug');
  seal.replyToSender(ctx, msg, 'news-reply-marker');
  setTimeout(() => seal.replyToSender(ctx, msg, 'timer-reply-marker'), 10);
  return seal.ext.newCmdExecuteResult(true);
}; ext.cmdMap.news = cmd;
const fail = seal.ext.newCmdItemInfo();
fail.solve = (ctx, msg) => { seal.replyToSender(ctx, msg, 'send-fail-marker'); return seal.ext.newCmdExecuteResult(true); };
ext.cmdMap.failmsg = fail; seal.ext.register(ext);`);
  fs.writeFileSync(path.join(root, 'data/plugin/lua-origin.lua'), `
msg_order = { [".luaorigin"] = "reply" }
function reply(msg)
  print('lua-own-log-marker')
  sendMsg('lua-direct-marker', msg.fromGroup, msg.fromUser)
  return 'lua-return-marker'
end`);
  child = spawn(binary, [], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  exited = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', (code, signal) => resolve({ code, signal })); });
  for (const stream of [child.stdout, child.stderr]) stream.on('data', data => { output = (output + data.toString()).slice(-50000); });
  async function api(endpoint, method = 'GET', body) {
    const response = await fetch('http://127.0.0.1:' + httpPort + endpoint, {
      method, headers: { 'X-API-Key': apiKey, 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(5000),
    });
    return response.json();
  }
  await until(async () => { try { return (await api('/api/auth/status')).code === 0; } catch { return false; } });
  const received = [];
  const respond = event => {
    const request = JSON.parse(event.data);
    if (!request.action) return;
    let data = {};
    if (request.action === 'get_login_info') data = { user_id: 10000, nickname: '测试骰娘' };
    else if (request.action === 'get_group_list') data = [{ group_id: 20000, group_name: '隔离测试群' }];
    else if (request.action === 'get_friend_list') data = [];
    else if (request.action === 'get_group_member_info') data = { user_id: request.params.user_id, role: 'member' };
    const send = request.action.startsWith('send_');
    if (send) { received.push(request); data = { message_id: received.length + 1000 }; }
    const denied = send && JSON.stringify(request.params).includes('send-fail-marker');
    peer.send(JSON.stringify({ echo: request.echo, status: denied ? 'failed' : 'ok', retcode: denied ? 100 : 0, message: denied ? 'fake-platform-rejection' : '', data }));
  };
  const connectPeer = () => { if (peer) peer.close(); peer = localPeer(wsPort); peer.addEventListener('message', respond); };
  connectPeer();
  // The HTTP listener can be ready before the deferred adapter startup. Retry
  // connection refusals without declaring the fake protocol ready prematurely.
  await until(() => { if (peer.readyState === 3) connectPeer(); return peer.readyState === 1; });
  await delay(150);
  const command = (content, id) => peer.send(JSON.stringify({ post_type: 'message', message_type: 'group',
    self_id: 10000, user_id: 30000, group_id: 20000, message_id: id, time: Math.floor(Date.now() / 1000),
    message: content, raw_message: content, sender: { user_id: 30000, nickname: '测试用户', role: 'member' } }));
  const records = async () => (await api('/api/dashboard/stats')).data.recent_logs;
  command('.r 1d1', 1); await until(() => received.length >= 1);
  command('.news', 2); await until(() => received.some(value => JSON.stringify(value).includes('timer-reply-marker')));
  command('.luaorigin', 3); await until(() => received.some(value => JSON.stringify(value).includes('lua-return-marker')));
  command('.failmsg', 4); await until(async () => (await records()).some(record => record.level === 'error' && record.message.includes('fake-platform-rejection')));
  let all = await records();
  assert.equal(all.filter(record => record.module === '消息' && record.message.startsWith('收↩')).length, 4, 'Exactly one INFO receipt per inbound');
  assert.ok(all.some(record => record.module === '插件' && record.level === 'warn' && record.message.includes('plugin-own-warning')));
  assert.ok(!all.some(record => record.message.includes('hidden-plugin-debug')));
  for (const marker of ['news-reply-marker', 'timer-reply-marker', 'lua-direct-marker', 'lua-return-marker']) {
    const sent = all.find(record => record.module === '消息' && record.level === 'info' && record.message.includes(marker));
    assert.ok(sent?.msg_id, marker + ' must have an internal msgID');
    const plan = all.find(record => record.module === '处理' && record.msg_id === sent.msg_id);
    assert.ok(plan?.message.includes(marker), marker + ' must match its planned body');
    assert.ok(plan.details.processing.sources[0].kind.includes('插件'), marker + ' must name its plugin source');
    assert.ok(!sent.message.includes('来源：'), 'Actual send should not repeat source text');
  }
  const failed = all.find(record => record.level === 'error' && record.message.includes('fake-platform-rejection'));
  assert.ok(all.some(record => record.module === '处理' && record.msg_id === failed.msg_id));
  assert.ok(failed.details.processing.sources[0].file.includes('origin.js'));
  const heartbeat = () => peer.send(JSON.stringify({ post_type: 'meta_event', meta_event_type: 'heartbeat', status: { online: true, good: true }, interval: 30000 }));
  heartbeat(); await delay(60); all = await records(); assert.ok(!all.some(record => record.message.includes('收到协议心跳')));
  assert.equal((await api('/api/system/log-mode', 'PUT', { level: 'trace', raw: true })).code, 0);
  heartbeat(); await until(async () => (await records()).some(record => record.level === 'trace' && record.message.includes('收到协议心跳')));
  assert.notEqual((await api('/api/system/log-mode', 'PUT', { level: 'error', raw: 'invalid' })).code, 0);
  assert.equal((await api('/api/system/log-mode')).data.level, 'trace', 'Invalid mixed settings must not partially mutate level');
  assert.equal((await api('/api/system/log-mode', 'PUT', { level: 'error' })).code, 0);
  command('.failmsg', 5);
  await until(async () => (await records()).filter(record => record.level === 'error' && record.message.includes('fake-platform-rejection')).length === 2);
  all = await records();
  const errorOnly = all.filter(record => record.level === 'error' && record.message.includes('fake-platform-rejection')).at(-1);
  assert.ok(errorOnly.details.processing.sources[0].file.includes('origin.js'));
  assert.ok(!all.some(record => record.module === '处理' && record.msg_id === errorOnly.msg_id), 'ERROR threshold hides plan but preserves source on the failure');
  assert.ok(!JSON.stringify(all).includes(apiKey));
  assert.ok(!output.includes('[tz=') && !output.includes('thread='));
  console.log('PASS: isolated OneBot receipts, core/Lua/JS/timer origins, planned and actual bodies, msgID failures, six-level API, heartbeat suppression and redaction');
} catch (error) {
  console.error(output); throw error;
} finally {
  if (peer) peer.close();
  if (child && child.exitCode === null && child.signalCode === null) { child.kill(); await exited; }
  // Validate the absolute recursive-delete target: only this exact generated
  // fixture directly under the explicitly chosen build binary directory.
  assert.equal(path.dirname(path.resolve(root)), path.resolve(parent));
  assert.ok(path.basename(root).startsWith('dice-runtime-log-smoke-'));
  fs.rmSync(root, { recursive: true, force: true });
}
