// Uses only Node built-ins. No production bot, credentials or extra SDK/runtime.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createServer } from 'node:net';
import { once } from 'node:events';

const executable = process.argv[2];
if (!executable) throw new Error('Usage: node tools/test-mcp-http.mjs path/to/dice-next-mcp-probe');
const root = await mkdtemp(join(tmpdir(), 'dicenext-mcp-http-'));
const reservation = createServer();
reservation.listen(0, '127.0.0.1'); await once(reservation, 'listening');
const port = reservation.address().port;
await new Promise((done) => reservation.close(done));
const child = spawn(resolve(executable), [root, String(port)], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
child.stdin.on('error', () => {}); // A failed child may already have closed stdin.
const exit = once(child, 'exit');
let stderr = '';
let phase = 'startup';
child.stderr.on('data', (data) => { stderr = (stderr + data).slice(-3000); });
try {
  const grant = await new Promise((accept, reject) => {
    let output = '';
    const timer = setTimeout(() => reject(new Error('MCP fixture startup timed out')), 20000);
    child.once('error', reject);
    child.once('exit', () => { clearTimeout(timer); reject(new Error('MCP fixture exited before readiness')); });
    child.stdout.on('data', (data) => {
      output += data;
      const match = output.match(/MCP_FIXTURE_READY ([^\r\n]+)/);
      if (match) { clearTimeout(timer); accept(JSON.parse(match[1])); output = ''; }
    });
  });
  const url = `http://127.0.0.1:${port}`;
  const headers = { Authorization: `Bearer ${grant.token}`, 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream', 'MCP-Protocol-Version': '2025-11-25' };
  const rpc = async (method, params = {}, extra = {}) => {
    phase = `${method} ${params.name || ''}`;
    const response = await fetch(`${url}/mcp`, { method: 'POST', headers: { ...headers, ...extra }, body: JSON.stringify({ jsonrpc: '2.0', id: 17, method, params }), signal: AbortSignal.timeout(10000) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  };
  const init = await rpc('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'HTTP fixture', version: '1' } });
  assert.equal(init.body.result.protocolVersion, '2025-11-25');
  assert.equal(init.body.result.serverInfo.version, 'transport-fixture');
  assert.equal(init.headers.get('mcp-session-id'), null);
  const listed = await rpc('tools/list'); assert.equal(listed.status, 200); assert.equal(listed.body.result.tools.length, 4);
  const roll = await rpc('tools/call', { name: 'roll_dice', arguments: { expression: '2d6+3' } });
  assert.equal(roll.body.result.isError, false); assert.ok(roll.body.result.structuredContent.result >= 5 && roll.body.result.structuredContent.result <= 15);
  const card = await rpc('tools/call', { name: 'get_character_card', arguments: { card_id: 7 } }); assert.equal(card.body.result.structuredContent.attributes.理智, 70);
  assert.equal((await rpc('tools/call', { name: 'get_character_card', arguments: { card_id: 8 } })).status, 403);
  assert.equal((await rpc('tools/call', { name: 'run_command', arguments: { command: '.bot on' } })).status, 400);
  const modernMeta = { 'io.modelcontextprotocol/protocolVersion': '2026-07-28', 'io.modelcontextprotocol/clientCapabilities': {} };
  const modernHeaders = { 'MCP-Protocol-Version': '2026-07-28', 'Mcp-Method': 'server/discover' };
  const discover = await rpc('server/discover', { _meta: modernMeta }, modernHeaders);
  assert.equal(discover.status, 200); assert.equal(discover.body.result.resultType, 'complete');
  assert.equal(discover.body.result.cacheScope, 'private'); assert.equal(discover.body.result.ttlMs, 0);
  assert.equal(discover.body.result._meta['io.modelcontextprotocol/serverInfo'].name, 'Dice!Next');
  assert.equal(discover.body.result._meta['io.modelcontextprotocol/serverInfo'].version, 'transport-fixture');
  const modernList = await rpc('tools/list', { _meta: modernMeta }, { ...modernHeaders, 'Mcp-Method': 'tools/list' });
  assert.equal(modernList.body.result.cacheScope, 'private'); assert.equal(modernList.body.result.ttlMs, 0);
  const modernCall = await rpc('tools/call', { _meta: modernMeta, name: 'get_status', arguments: {} }, { ...modernHeaders, 'Mcp-Method': 'tools/call', 'Mcp-Name': '=?base64?Z2V0X3N0YXR1cw==?=' });
  assert.equal(modernCall.status, 200); assert.equal(modernCall.body.result.structuredContent.status, 'running');
  const spoof = await rpc('tools/call', { _meta: modernMeta, name: 'get_status', arguments: {} }, { ...modernHeaders, 'Mcp-Method': 'tools/call', 'Mcp-Name': 'roll_dice' });
  assert.equal(spoof.body.error.code, -32020);
  assert.equal((await rpc('get_status', {}, { Authorization: 'Bearer invalid' })).status, 401);
  assert.equal((await rpc('get_status', {}, { Origin: 'https://evil.example' })).status, 403);
  phase = 'browser preflight';
  const cors = await fetch(`${url}/mcp`, { method: 'OPTIONS', headers: { Origin: 'https://fixture.example', 'Access-Control-Request-Method': 'POST', 'Access-Control-Request-Headers': 'authorization,content-type' }, signal: AbortSignal.timeout(10000) });
  assert.equal(cors.status, 204); assert.equal(cors.headers.get('access-control-allow-origin'), 'https://fixture.example'); assert.equal(cors.headers.get('access-control-allow-credentials'), null);
  const browser = await rpc('tools/call', { name: 'get_status' }, { Origin: 'https://fixture.example' });
  assert.equal(browser.headers.get('access-control-allow-origin'), 'https://fixture.example');
  const management = `${url}/api/system/mcp`;
  phase = 'management path authentication';
  for (const path of ['/API/system/mcp', '/api/System/MCP', '/%61pi/system/mcp', '/a%70i/SYSTEM/Mcp', '/%41pi/system/mcp', '/api%2Fsystem/mcp']) {
    const response = await fetch(`${url}${path}`, { signal: AbortSignal.timeout(10000) });
    assert.ok([401, 403, 404].includes(response.status), `${path}: unexpected HTTP ${response.status}`);
  }
  assert.equal((await fetch(management, { headers: { 'X-Fixture-Skip-Auth': '1' }, signal: AbortSignal.timeout(10000) })).status, 401);
  phase = 'MCP token must not authenticate management';
  assert.equal((await fetch(management, { headers: { Authorization: `Bearer ${grant.token}` }, signal: AbortSignal.timeout(10000) })).status, 401);
  const ownerHeaders = { 'X-API-Key': 'transport-fixture-owner' };
  phase = 'restricted administration';
  assert.equal((await fetch(management, { headers: { ...ownerHeaders, 'X-Fixture-Role': 'admin' }, signal: AbortSignal.timeout(10000) })).status, 403);
  phase = 'management settings';
  const publicSettings = await (await fetch(management, { headers: ownerHeaders, signal: AbortSignal.timeout(10000) })).json();
  assert.equal(publicSettings.code, 0); assert.ok(!JSON.stringify(publicSettings).includes(grant.token)); assert.ok(!JSON.stringify(publicSettings).includes('token_hash'));
  const mutate = (path, method, body, extra = {}) => fetch(`${management}${path}`, { method, headers: { ...ownerHeaders, 'Content-Type': 'application/json', ...extra }, body, signal: AbortSignal.timeout(10000) });
  phase = 'owner-only local self-check and card summaries';
  assert.ok(publicSettings.data.server_time > 0); assert.equal(publicSettings.data.usage_available, true);
  assert.ok(publicSettings.data.grants[0].last_used_at > 0);
  for (const [path, body] of [['/self-check', {}], ['/card-details', { card_ids: [7], grant_id: grant.id }]]) {
    const json = JSON.stringify(body);
    assert.equal((await fetch(`${management}${path}`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: json })).status, 401);
    assert.equal((await mutate(path, 'POST', json, { 'X-Fixture-Role': 'admin' })).status, 403);
    assert.equal((await mutate(path, 'POST', json, { Origin: 'https://evil.example' })).status, 403);
  }
  const check = await (await mutate('/self-check', 'POST', JSON.stringify({ token: grant.token, tool: 'get_status' }))).json();
  assert.equal(check.data.scope, 'local_protocol'); assert.ok(check.data.checks.every((row) => row.state === 'pass'));
  assert.ok(!JSON.stringify(check).includes(grant.token));
  const summaries = await (await mutate('/card-details', 'POST', JSON.stringify({ card_ids: [7], grant_id: grant.id }))).json();
  assert.equal(summaries.data[0].state, 'available'); assert.equal(summaries.data[0].user_id, 'fixture-user');
  assert.equal('attributes' in summaries.data[0], false);
  assert.equal((await mutate('/self-check', 'POST', '{"url":"http://internal/"}')).status, 400);
  assert.equal((await mutate('/card-details', 'POST', '{"card_ids":[0]}')).status, 400);
  phase = 'management browser CSRF boundary';
  assert.equal((await mutate('', 'PUT', '{"enabled":true}', { Origin: 'https://evil.example' })).status, 403);
  assert.equal((await mutate('', 'PUT', '{"enabled":true}', { Origin: 'null', 'Sec-Fetch-Site': 'same-origin', 'X-Dice-MCP-Request': 'webui' })).status, 403);
  assert.equal((await mutate('', 'PUT', '{"enabled":true}', { Origin: url, 'Sec-Fetch-Site': 'same-site', 'X-Dice-MCP-Request': 'webui' })).status, 403);
  const cookieWrite = (extra = {}) => fetch(management, { method: 'PUT', headers: { Cookie: 'fixture-session=owner', 'Content-Type': 'application/json', ...extra }, body: '{"enabled":true,"allowed_origins":["https://fixture.example"]}', signal: AbortSignal.timeout(10000) });
  assert.equal((await cookieWrite()).status, 403);
  assert.equal((await cookieWrite({ 'Content-Type': 'text/plain', Origin: url })).status, 415);
  assert.equal((await cookieWrite({ Origin: url, 'X-Dice-MCP-Request': 'webui' })).status, 200);
  assert.equal((await cookieWrite({ Origin: 'https://reverse-proxy.example', 'Sec-Fetch-Site': 'same-origin', 'X-Dice-MCP-Request': 'webui' })).status, 200);
  const preflight = await fetch(management, { method: 'OPTIONS', headers: { Origin: 'https://evil.example', 'Access-Control-Request-Method': 'PUT', 'Access-Control-Request-Headers': 'content-type,x-dice-mcp-request' }, signal: AbortSignal.timeout(10000) });
  assert.equal(preflight.status, 405); assert.equal(preflight.headers.get('access-control-allow-origin'), null);
  assert.equal((await fetch(management, { method: 'HEAD', headers: ownerHeaders, signal: AbortSignal.timeout(10000) })).status, 200);
  phase = 'uncertain write recovery without retaining secrets';
  const operation = 'b'.repeat(32), grantInput = JSON.stringify({ name: 'operation fixture', tools: ['get_status'] });
  const created = await (await mutate('/grants', 'POST', grantInput, { 'X-Dice-MCP-Operation-ID': operation })).json();
  assert.equal(created.code, 0); assert.ok(created.data.token);
  const operationState = await (await fetch(`${management}/operations/${operation}`, { headers: ownerHeaders, signal: AbortSignal.timeout(10000) })).json();
  assert.equal(operationState.data.state, 'succeeded'); assert.equal(operationState.data.grant_id, created.data.id);
  assert.ok(!JSON.stringify(operationState).includes(created.data.token)); assert.ok(!JSON.stringify(operationState).includes('token_hash'));
  assert.equal((await mutate('/grants', 'POST', grantInput, { 'X-Dice-MCP-Operation-ID': operation })).status, 409);
  const unknownState = await (await fetch(`${management}/operations/${'e'.repeat(32)}`, { headers: ownerHeaders, signal: AbortSignal.timeout(10000) })).json();
  assert.equal(unknownState.data.state, 'unknown');
  assert.equal((await mutate('/grants', 'POST', grantInput, { 'X-Dice-MCP-Operation-ID': 'invalid' })).status, 400);
  await mutate(`/grants/${created.data.id}`, 'DELETE');
  phase = 'client disconnect after accepted creation';
  const lostId = 'd'.repeat(32), abort = new AbortController();
  const disconnected = fetch(`${management}/grants`, { method: 'POST', headers: { ...ownerHeaders, 'Content-Type': 'application/json', 'X-Dice-MCP-Operation-ID': lostId },
    body: JSON.stringify({ name: 'lost reply fixture', tools: ['get_character_card'], card_ids: [42] }), signal: abort.signal }).catch((error) => error);
  const stateOf = async () => (await (await fetch(`${management}/operations/${lostId}`, { headers: ownerHeaders, signal: AbortSignal.timeout(1000) })).json()).data;
  let lostState;
  for (let i = 0; i < 60; ++i) {
    lostState = await stateOf();
    if (lostState.state === 'running') break;
    await new Promise((done) => setTimeout(done, 20));
  }
  assert.equal(lostState.state, 'running'); abort.abort(); await disconnected;
  for (let i = 0; i < 100; ++i) {
    lostState = await stateOf();
    if (lostState.state === 'succeeded') break;
    await new Promise((done) => setTimeout(done, 20));
  }
  assert.equal(lostState.state, 'succeeded'); assert.ok(lostState.grant_id); assert.equal('token' in lostState, false);
  const afterDisconnect = await (await fetch(management, { headers: ownerHeaders, signal: AbortSignal.timeout(1000) })).json();
  assert.equal(afterDisconnect.data.grants.filter((item) => item.name === 'lost reply fixture').length, 1);
  await mutate(`/grants/${lostState.grant_id}`, 'DELETE');
  phase = 'invalid management JSON';
  assert.equal((await mutate('', 'PUT', '{')).status, 400);
  assert.equal((await mutate('', 'PUT', '{"x":' + '['.repeat(40) + '0' + ']'.repeat(40) + '}')).status, 400);
  phase = 'management rotation permissions';
  assert.equal((await fetch(`${management}/grants/${grant.id}/rotate`, { method: 'POST', headers: { Authorization: `Bearer ${grant.token}` }, signal: AbortSignal.timeout(10000) })).status, 401);
  assert.equal((await mutate(`/grants/${grant.id}/rotate`, 'POST', '{}', { 'X-Fixture-Role': 'admin' })).status, 403);
  assert.equal((await mutate(`/grants/${grant.id}/rotate`, 'POST', '{"expires_days":365}')).status, 400);
  phase = 'editing preserves expiry';
  const edited = await (await mutate(`/grants/${grant.id}`, 'PUT', JSON.stringify({ name: 'renamed fixture', tools: grant.tools, card_ids: grant.card_ids }))).json();
  assert.equal(edited.code, 0); assert.equal(edited.data.expires_at, grant.expires_at);
  phase = 'credential rotation';
  const regenerated = await (await mutate(`/grants/${grant.id}/rotate`, 'POST', '{}')).json();
  assert.equal(regenerated.code, 0); assert.notEqual(regenerated.data.token, grant.token);
  assert.equal(regenerated.data.expires_at, grant.expires_at); assert.deepEqual(regenerated.data.tools, grant.tools);
  headers.Authorization = `Bearer ${regenerated.data.token}`;
  assert.equal((await rpc('ping', {}, { Authorization: `Bearer ${grant.token}` })).status, 401);
  assert.equal((await rpc('ping')).status, 200);
  phase = 'server-side audit filtering';
  const audit = await (await fetch(`${management}/audit?client_id=${grant.id}&outcome=denied&pageSize=1&page=99`, { headers: ownerHeaders, signal: AbortSignal.timeout(10000) })).json();
  assert.equal(audit.code, 0); assert.ok(audit.data.total >= 2); assert.equal(audit.data.items.length, 1);
  assert.ok(audit.data.items.every((row) => row.client_id === grant.id && row.outcome === 'denied'));
  for (const query of ['page=1oops', 'pageSize=0', 'outcome=unexpected', 'client_id=invalid']) {
    assert.equal((await fetch(`${management}/audit?${query}`, { headers: ownerHeaders, signal: AbortSignal.timeout(10000) })).status, 400);
  }
  phase = 'bounded slow work must not block WebUI I/O or operation status';
  const clients = [];
  for (let i = 0; i < 12; ++i) {
    const created = await (await mutate('/grants', 'POST', JSON.stringify({ name: `fairness fixture ${i}`, tools: ['search_help', 'get_status'] }))).json();
    assert.equal(created.code, 0); clients.push(created.data);
  }
  const slow = clients.map((client) => rpc('tools/call', { name: 'search_help', arguments: { query: 'fixture-slow' } }, { Authorization: `Bearer ${client.token}` }));
  await new Promise((done) => setTimeout(done, 100));
  const health = await fetch(`${url}/fixture-health`, { signal: AbortSignal.timeout(800) }); assert.equal(await health.text(), 'ok');
  const quickState = await fetch(`${management}/operations/${operation}`, { headers: ownerHeaders, signal: AbortSignal.timeout(800) }); assert.equal(quickState.status, 200);
  const managementStillResponds = await fetch(management, { headers: ownerHeaders, signal: AbortSignal.timeout(800) }); assert.equal(managementStillResponds.status, 200);
  const outcomes = await Promise.all(slow); assert.ok(outcomes.some((item) => item.status === 503)); assert.ok(outcomes.some((item) => item.status === 200));
  assert.ok(outcomes.every((item) => item.status === 200 || item.status === 503));
  phase = 'one credential cannot consume both tool workers';
  const oneSlow = rpc('tools/call', { name: 'search_help', arguments: { query: 'fixture-slow' } });
  for (let i = 0; i < 50; ++i) {
    const state = await fetch(`${url}/fixture-health`, { signal: AbortSignal.timeout(1000) });
    if (Number(state.headers.get('x-fixture-active')) > 0) break;
    await new Promise((done) => setTimeout(done, 20));
  }
  const sameCredential = await rpc('tools/call', { name: 'get_status' }); assert.equal(sameCredential.status, 429);
  const independent = await rpc('tools/call', { name: 'get_status' }, { Authorization: `Bearer ${clients[0].token}` }); assert.equal(independent.status, 200);
  assert.equal((await oneSlow).status, 200);
  assert.equal((await rpc('tools/call', { name: 'get_status' })).status, 200);
  phase = 'credential revocation';
  const deleted = await mutate(`/grants/${grant.id}`, 'DELETE'); assert.equal(deleted.status, 200);
  assert.equal((await rpc('ping')).status, 401);
  phase = 'shutdown cooperatively interrupts an active query';
  headers.Authorization = `Bearer ${clients[0].token}`;
  const cancelling = rpc('tools/call', { name: 'search_help', arguments: { query: 'fixture-cancel' } }).catch(() => null);
  let active = false;
  for (let i = 0; i < 50; ++i) {
    const state = await fetch(`${url}/fixture-health`, { signal: AbortSignal.timeout(1000) });
    if (Number(state.headers.get('x-fixture-active')) > 0) { active = true; break; }
    await new Promise((done) => setTimeout(done, 20));
  }
  assert.equal(active, true); child.stdin.end('quit\n');
  const exited = await Promise.race([exit.then(() => true), new Promise((done) => setTimeout(() => done(false), 2000).unref())]);
  assert.equal(exited, true); await cancelling;
  console.log('PASS MCP HTTP: legacy/modern, scoped tools, CSRF/CORS, auth/HEAD, safe write recovery, bounded workers, rotation, audit and revocation');
} catch (error) {
  throw new Error(`${phase}: ${error.message}; fixture exit=${child.exitCode}, stderr=${stderr.replace(/dnmcp_[0-9a-f]+/g, '<redacted>')}`, { cause: error });
} finally {
  if (child.exitCode === null && !child.stdin.destroyed) child.stdin.end('quit\n');
  const stopped = await Promise.race([exit.then(() => true), new Promise((done) => setTimeout(() => done(false), 5000).unref())]);
  if (!stopped) { child.kill(); await exit; }
  // Only the directory created by mkdtemp above; never a user-selected path.
  await rm(root, { recursive: true, force: true });
}
