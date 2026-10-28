// Official clients against an isolated C++ fixture and a real TLS-terminating
// reverse proxy. All credentials/certificates below are disposable test data.
import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdtemp, readFile, writeFile, rm, access } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import { createServer as netServer } from 'node:net';
import { createServer as tlsServer, get as httpsGet } from 'node:https';
import { request as httpRequest } from 'node:http';
import { getCACertificates } from 'node:tls';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';

const run = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));
const safe = (text) => String(text).replace(/dnmcp_[0-9a-f]+/g, '<redacted>');
if (process.argv[2] === '--sdk') {
  // Trust the temporary test CA using NODE_EXTRA_CA_CERTS on this child only.
  let input = ''; for await (const chunk of process.stdin) input += chunk;
  const { url, token } = JSON.parse(input);
  const client = new Client({ name: 'DiceNext official SDK test', version: '1' });
  const transport = new StreamableHTTPClientTransport(new URL(url), { requestInit: { headers: { Authorization: `Bearer ${token}` } } });
  try {
    // Fail here, with a precise phase, if the disposable CA was not loaded.
    assert.ok(getCACertificates('extra').length > 0, 'Temporary CA was not loaded');
    const expectedCA = (await readFile(process.env.NODE_EXTRA_CA_CERTS, 'utf8')).trim();
    assert.ok(getCACertificates('extra').some((value) => value.trim() === expectedCA), 'Loaded CA differs from the temporary test CA');
    await new Promise((done, reject) => {
      const request = httpsGet(url, { ca: getCACertificates('extra'), timeout: 10000 }, (response) => { response.resume(); response.on('end', done); });
      request.on('error', (error) => reject(new Error(`HTTPS sanity: ${error.message}`))); request.on('timeout', () => request.destroy());
    });
    await client.connect(transport, { timeout: 10000 });
    assert.equal((await client.listTools()).tools.length, 4);
    const roll = await client.callTool({ name: 'roll_dice', arguments: { expression: '2d6+3' } });
    assert.equal(roll.isError, false); assert.ok(roll.structuredContent.result >= 5 && roll.structuredContent.result <= 15);
    const card = await client.callTool({ name: 'get_character_card', arguments: { card_id: 7 } });
    assert.equal(card.structuredContent.attributes.理智, 70);
    assert.equal(transport.sessionId, undefined);
    console.log('SDK_OK');
  } finally { await client.close(); }
  process.exit(0);
}

const executable = process.argv[2];
if (!executable) throw new Error('Pass the path to dice-next-mcp-probe');
const root = await mkdtemp(join(tmpdir(), 'dicenext-mcp-interop-'));
const reservation = netServer(); reservation.listen(0, '127.0.0.1'); await once(reservation, 'listening');
const port = reservation.address().port; await new Promise((done) => reservation.close(done));
const child = spawn(resolve(executable), [root, String(port)], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
child.stdin.on('error', () => {});
const exit = once(child, 'exit');
let stderr = '', phase = 'startup', proxy;
child.stderr.on('data', (chunk) => { stderr = (stderr + chunk).slice(-2000); });
try {
  const grant = await new Promise((accept, reject) => {
    let output = '';
    const timer = setTimeout(() => reject(new Error('Fixture startup timed out')), 20000);
    child.once('error', reject);
    child.once('exit', () => { clearTimeout(timer); reject(new Error('Fixture exited before readiness')); });
    child.stdout.on('data', (chunk) => {
      output += chunk; const match = output.match(/MCP_FIXTURE_READY ([^\r\n]+)/);
      if (match) { clearTimeout(timer); accept(JSON.parse(match[1])); output = ''; }
    });
  });
  phase = 'temporary TLS certificate';
  let openssl = process.env.DICENEXT_TEST_OPENSSL || 'openssl';
  if (process.platform === 'win32' && !process.env.DICENEXT_TEST_OPENSSL) {
    const gitOpenSSL = 'C:/Program Files/Git/usr/bin/openssl.exe';
    try { await access(gitOpenSSL); openssl = gitOpenSSL; } catch { /* Try PATH. */ }
  }
  const key = join(root, 'test-key.pem'), cert = join(root, 'test-cert.pem'), ca = join(root, 'test-ca.pem');
  const caKey = join(root, 'test-ca-key.pem'), csr = join(root, 'test.csr'), extensions = join(root, 'test.ext');
  const sslConfig = join(root, 'openssl.cnf');
  // Do not inherit a machine's openssl.cnf: some Git/OpenSSL versions append
  // -addext to v3_ca and produce duplicate (invalid) Basic Constraints.
  await writeFile(sslConfig, '[req]\ndistinguished_name=dn\n[dn]\n[ca_extensions]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n');
  const ssl = (args) => run(openssl, args, { windowsHide: true, timeout: 15000 });
  await ssl(['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', caKey, '-out', ca, '-days', '1',
    '-subj', '/CN=DiceNext disposable test CA', '-config', sslConfig, '-extensions', 'ca_extensions']);
  await ssl(['req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', csr, '-subj', '/CN=localhost', '-config', sslConfig]);
  await writeFile(extensions, 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n');
  await ssl(['x509', '-req', '-in', csr, '-CA', ca, '-CAkey', caKey, '-CAcreateserial', '-out', cert, '-days', '1', '-extfile', extensions]);
  await ssl(['verify', '-CAfile', ca, cert]);
  proxy = tlsServer({ key: await readFile(key), cert: await readFile(cert) }, (request, response) => {
    const upstream = httpRequest({ hostname: '127.0.0.1', port, path: request.url.replace(/^\/dice/, ''), method: request.method,
      headers: { ...request.headers, host: `127.0.0.1:${port}`, 'x-forwarded-for': '127.0.0.1' } }, (result) => {
      response.writeHead(result.statusCode, result.headers); result.pipe(response);
    });
    upstream.on('error', () => { response.writeHead(502); response.end(); }); request.pipe(upstream);
  });
  proxy.listen(0, '127.0.0.1'); await once(proxy, 'listening');
  const publicOrigin = `https://127.0.0.1:${proxy.address().port}`;
  const url = `${publicOrigin}/dice/mcp`;
  const env = { ...process.env, NODE_EXTRA_CA_CERTS: ca, MCP_CLIENT_CONFIG_PATH: join(root, 'inspector-client.json') };
  delete env.MCP_CATALOG_PATH; // --config is read-only and mutually exclusive.
  const invoke = async (args) => {
    try { return await run(process.execPath, args, { env, timeout: 20000, maxBuffer: 1024 * 1024, windowsHide: true }); }
    catch (error) { throw new Error(safe(`${error.message}; ${error.stderr || ''}`)); }
  };
  const sdk = () => new Promise((accept, reject) => {
    const runner = spawn(process.execPath, [fileURLToPath(import.meta.url), '--sdk'], { env, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
    let output = ''; const timer = setTimeout(() => runner.kill(), 20000);
    runner.on('error', reject); runner.stdout.on('data', (chunk) => { output += chunk; });
    runner.stderr.on('data', (chunk) => { output += chunk; });
    runner.on('exit', (code) => { clearTimeout(timer); code === 0 ? accept(output) : reject(new Error(safe(output))); });
    runner.stdin.end(JSON.stringify({ url, token: grant.token }));
  });
  phase = 'proxy denied until explicitly enabled';
  // The first SDK attempt must be denied: localhost forwarding is not a bypass.
  await assert.rejects(sdk());
  const settings = await fetch(`http://127.0.0.1:${port}/api/system/mcp`, { method: 'PUT',
    headers: { 'X-API-Key': 'transport-fixture-owner', 'Content-Type': 'application/json' },
    body: JSON.stringify({ enabled: true, allow_remote: true, allowed_hosts: [`127.0.0.1:${port}`] }), signal: AbortSignal.timeout(10000) });
  assert.equal(settings.status, 200);
  phase = 'official SDK through HTTPS prefix proxy'; assert.match(await sdk(), /SDK_OK/);
  const config = join(root, 'inspector.json');
  await writeFile(config, JSON.stringify({ mcpServers: { fixture: { type: 'http', url, headers: { Authorization: `Bearer ${grant.token}` }, connectionTimeout: 10000, requestTimeout: 10000 } } }));
  const launcher = join(here, 'node_modules/@modelcontextprotocol/inspector/clients/launcher/build/index.js');
  for (const era of ['legacy', 'modern']) {
    phase = `official Inspector ${era}`;
    const base = [launcher, '--cli', '--config', config, '--server', 'fixture', '--protocol-era', era, '--format', 'json', '--quiet', '--stored-auth-only'];
    const list = JSON.parse((await invoke([...base, '--method', 'tools/list'])).stdout);
    const result = list.result || list; assert.equal(result.tools.length, 4);
    const called = JSON.parse((await invoke([...base, '--method', 'tools/call', '--tool-name', 'roll_dice', '--tool-args-json', '{"expression":"d1+2"}'])).stdout);
    assert.equal((called.result || called).structuredContent.result, 3);
  }
  console.log('PASS MCP interop: official SDK + Inspector legacy/modern; HTTPS CA validation, prefix proxy and local-only rejection');
} catch (error) {
  throw new Error(safe(`${phase}: ${error.message}; fixture=${stderr}`));
} finally {
  if (proxy) { proxy.closeAllConnections(); await new Promise((done) => proxy.close(done)); }
  if (child.exitCode === null && !child.stdin.destroyed) child.stdin.end('quit\n');
  const stopped = await Promise.race([exit.then(() => true), new Promise((done) => setTimeout(() => done(false), 5000).unref())]);
  if (!stopped) { child.kill(); await exit; }
  // Only our own mkdtemp directory, never user-selected runtime data.
  await rm(root, { recursive: true, force: true });
}
