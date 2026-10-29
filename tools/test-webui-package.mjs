// Test the actual archived core and UI, not a replacement HTTP server.
// Only a new isolated copy is started, with loopback binding, no bot accounts,
// no automatic updates/backups/cloud heartbeat and no production credentials.
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { createServer } from 'node:net';
import { mkdtemp, readdir, readFile, writeFile, mkdir, stat, rename, rm } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

assert.ok(process.argv[2], 'Usage: node tools/test-webui-package.mjs <archive-or-release-directory>');
if (process.platform === 'win32' && !process.env.CI && !process.argv.includes('--allow-windows-listener')) {
  throw new Error('This isolated core opens a loopback HTTP listener. On Windows, explicitly pass --allow-windows-listener (a firewall prompt is possible).');
}
let archive = path.resolve(process.argv[2]);
if ((await stat(archive)).isDirectory()) {
  const archives = (await readdir(archive)).filter(name => /^DiceNext-.*\.(zip|tar\.gz)$/.test(name));
  assert.equal(archives.length, 1, 'Expected exactly one release archive; pass an exact filename otherwise');
  archive = path.join(archive, archives[0]);
}
// A sibling of the archive, not the Windows OS Temp directory (startup guard).
const sandbox = await mkdtemp(path.join(path.dirname(archive), '.webui-package-test-'));
assert.match(path.relative(path.dirname(archive), sandbox), /^\.webui-package-test-[A-Za-z0-9_-]+$/);
let child, exited, origin, root, apiKey, output = '', phase = 'extract archive', success = false;
async function stop() {
  if (!child) return;
  if (child.exitCode === null && child.signalCode === null) child.kill('SIGTERM');
  if (!await Promise.race([exited.then(() => true), delay(5000, null, { ref: false }).then(() => false)])) {
    child.kill('SIGKILL'); await exited;
  }
  child = null;
}
async function request(route, options = {}) {
  return fetch(origin + route, { redirect: 'manual', signal: AbortSignal.timeout(10000), ...options });
}
async function start() {
  const reservation = createServer();
  reservation.listen(0, '127.0.0.1'); await once(reservation, 'listening');
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const configFile = path.join(root, 'config/server.json');
  const server = JSON.parse(await readFile(configFile, 'utf8'));
  await writeFile(configFile, JSON.stringify({ ...server, host: '127.0.0.1', port }));
  const executable = path.join(root, process.platform === 'win32' ? 'app/dice-next-core.exe' : 'dice-next-server');
  const env = { ...process.env };
  // Never inherit a host's managed-instance bootstrap mode or restart markers.
  for (const key of Object.keys(env)) if (/^(MODE|DICEFLATS_.*|DICENEXT_MANAGED|DICENEXT_MANAGER_.*)$/i.test(key)) delete env[key];
  if (process.platform === 'linux') env.LD_LIBRARY_PATH = path.join(root, 'lib');
  if (process.platform === 'darwin') env.DYLD_LIBRARY_PATH = path.join(root, 'lib');
  output = '';
  child = spawn(executable, [], { cwd: root, env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let launchError;
  exited = new Promise(resolve => {
    child.once('error', error => { launchError = error; resolve(); });
    child.once('exit', resolve);
  });
  for (const stream of [child.stdout, child.stderr]) stream.on('data', data => { output = (output + data).slice(-12000); });
  origin = `http://127.0.0.1:${port}`;
  for (let attempt = 0; attempt < 100; attempt++) {
    if (launchError) throw launchError;
    assert.equal(child.exitCode, null, 'Packaged core exited before readiness');
    try {
      const response = await request('/api/auth/status', { signal: AbortSignal.timeout(500) });
      if (response.status === 200) return;
    } catch { /* The core has not bound the listener yet. */ }
    await delay(100);
  }
  throw new Error('Packaged core startup timed out');
}
async function state() {
  const response = await request('/api/system/webui-version', { headers: { 'X-API-Key': apiKey } });
  assert.equal(response.status, 200);
  return (await response.json()).data;
}
async function select(version, status = 200) {
  const response = await request('/api/system/webui-version', {
    method: 'PUT', headers: { 'X-API-Key': apiKey, 'Content-Type': 'application/json' },
    body: JSON.stringify({ version }),
  });
  assert.equal(response.status, status);
}
async function entries(version) {
  for (const entry of ['/', '/index.html']) {
    for (const method of ['GET', 'HEAD']) {
      const response = await request(`${entry}?sso=bound&test=upgrade`, { method });
      assert.equal(response.status, 302, `${method} ${entry} must redirect, not return 404`);
      assert.equal(response.headers.get('location'), `./${version}/?sso=bound&test=upgrade`);
      assert.equal(response.headers.get('cache-control'), 'no-store');
      if (method === 'HEAD') assert.equal(await response.text(), '');
    }
  }
  const followed = await request('/', { redirect: 'follow' });
  assert.equal(followed.status, 200);
  assert.equal(new URL(followed.url).pathname, `/${version}/`);
}
async function assets(version) {
  const response = await request(`/${version}/`);
  assert.equal(response.status, 200);
  assert.match(response.headers.get('content-type'), /text\/html/);
  const html = await response.text();
  assert.match(html, /id="root"/);
  assert.equal((await request(`/${version}/index.html`)).status, 200);
  const references = [...html.matchAll(/(?:src|href)=["']([^"']*assets\/[^"']+)["']/g)].map(match => match[1]);
  assert.ok(references.some(file => file.endsWith('.js')), `${version}: missing JavaScript entry`);
  assert.ok(references.some(file => file.endsWith('.css')), `${version}: missing stylesheet`);
  for (const reference of references) {
    const url = new URL(reference, `${origin}/${version}/`);
    assert.equal(url.origin, origin, 'Packaged assets must not depend on a remote origin');
    assert.ok(url.pathname.startsWith(`/${version}/assets/`), 'Versions must keep separate assets');
    const asset = await request(url.pathname);
    assert.equal(asset.status, 200, url.pathname);
    assert.match(asset.headers.get('content-type'), reference.endsWith('.js') ? /javascript/ : /css/);
    assert.ok((await asset.arrayBuffer()).byteLength > 0);
  }
}
try {
  if (process.platform === 'win32') {
    // Windows tar can reject UTF-8 ZIP names under the CI runner's locale.
    // .NET reads ZIP names directly, without a locale-dependent console roundtrip.
    assert.equal(path.extname(archive).toLowerCase(), '.zip');
    execFileSync('pwsh.exe', ['-NoProfile', '-NonInteractive', '-File',
      fileURLToPath(new URL('./expand-release-zip.ps1', import.meta.url)),
      '-ArchivePath', archive, '-DestinationPath', sandbox], { encoding: 'utf8', windowsHide: true });
  } else {
    // Refuse path traversal before extracting the locally built CI artifact.
    const names = execFileSync('tar', ['-tf', archive], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 }).trim().split(/\r?\n/);
    for (const name of names) assert.ok(!name.startsWith('/') && !name.includes('\\') && !name.split('/').includes('..') && !/^[A-Za-z]:/.test(name), `Unsafe archive member: ${name}`);
    // POSIX packages legitimately contain shared-library symlinks. The archives
    // here are locally built release artifacts, never an upload from an API caller.
    execFileSync('tar', ['-xf', archive, '-C', sandbox]);
  }
  const directories = (await readdir(sandbox, { withFileTypes: true })).filter(entry => entry.isDirectory());
  assert.equal(directories.length, 1, 'Expected one package root');
  root = path.join(sandbox, directories[0].name);
  for (const file of ['web/dist/index.html', 'web/dist/v1/index.html', 'web/dist/v2/index.html', 'web/dist/webui-versions.json']) {
    assert.ok((await stat(path.join(root, file))).isFile(), `Missing ${file}`);
  }
  // Model an existing installation: no ui_version field; the API credential
  // and unrelated preferences must survive the first version switch.
  apiKey = randomUUID();
  await mkdir(path.join(root, 'config'), { recursive: true });
  const sections = {
    server: { host: '127.0.0.1', port: 18088, api_key: apiKey, db_path: './data/webui-smoke.db', log_level: 'error' },
    adapters: [], events: {}, webui: { password: '', fixture_existing_preference: 'preserve-me' },
    i18n: { resource_dir: 'i18n', default_locale: 'zh-Hans' },
    dice: { heart_enabled: false, cloudban_enabled: false, console_start_hidden: true },
    update: { auto_check: false, auto_action: 'notify' }, backup: { auto_enabled: false }, hot_reload: { enabled: false },
  };
  for (const [name, value] of Object.entries(sections)) await writeFile(path.join(root, 'config', name + '.json'), JSON.stringify(value));
  phase = 'old configuration -> default V1';
  await start(); await entries('v1'); await assets('v1'); await assets('v2');
  assert.deepEqual((await state()).available, { v1: true, v2: true });
  phase = 'authenticated version switch';
  assert.equal((await request('/api/system/webui-version')).status, 401);
  assert.equal((await request('/api/system/webui-version', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: '{"version":"v2"}' })).status, 401);
  await select('v2'); await entries('v2'); await assets('v1');
  await select('v1'); await entries('v1');
  await select('v2');
  const saved = JSON.parse(await readFile(path.join(root, 'config/webui.json'), 'utf8'));
  assert.equal(saved.ui_version, 'v2'); assert.equal(saved.password, '');
  assert.equal(saved.fixture_existing_preference, 'preserve-me');
  phase = 'restart retains selected V2';
  await stop(); await start(); await entries('v2');
  phase = 'missing V2 falls back to V1 without erasing selection';
  await stop();
  await rename(path.join(root, 'web/dist/v2'), path.join(root, 'fixture-v2-held'));
  await start(); await entries('v1');
  assert.equal((await state()).selected, 'v2');
  await select('v2', 400);
  phase = 'legacy single-bundle entry remains accessible';
  await stop();
  await rename(path.join(root, 'web/dist'), path.join(root, 'fixture-dual-held'));
  await mkdir(path.join(root, 'web/dist'));
  await writeFile(path.join(root, 'web/dist/index.html'), '<!doctype html><html><body>legacy-ui-fixture</body></html>');
  await start();
  for (const route of ['/', '/index.html']) {
    const response = await request(route);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('cache-control'), 'no-store');
    assert.match(await response.text(), /legacy-ui-fixture/);
    assert.equal((await request(route, { method: 'HEAD' })).status, 200);
  }
  const legacy = await state();
  assert.equal(legacy.legacy, true); assert.equal(legacy.effective, 'v1');
  assert.equal(legacy.selected, 'v2');
  success = true;
  console.log('PASS packaged WebUI HTTP: root/index GET+HEAD, V1/V2 assets, old settings, auth, switching, restart, fallback and legacy bundle');
} catch (error) {
  throw new Error(`${phase}: ${error.message}\nIsolated package retained at ${sandbox}\n${output.replaceAll(apiKey || 'never-a-key', '<redacted>')}`, { cause: error });
} finally {
  await stop();
  // Only this invocation's mkdtemp directory; preserve evidence on failure.
  if (success) await rm(sandbox, { recursive: true, force: true });
}
