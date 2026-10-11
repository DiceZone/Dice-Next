// Real foreground process and filesystem smoke tests. All writes are confined
// to mkdtemp fixtures; no production bot/data is opened.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
if (process.argv.length !== 4) throw new Error('Usage: node tools/test-posix-manager.mjs <manager> <fixture-core>');
const [manager, core] = process.argv.slice(2).map(value => path.resolve(value));
const startupTimeout = 15000; // Hosted macOS cold-start checks can take more than 3 seconds.
const fixtures = [];
const file = (target, text) => { fs.mkdirSync(path.dirname(target), { recursive: true }); fs.writeFileSync(target, text); };
const metadata = { schema: 1, tag: 'v99.0.0-beta.900', version: '99.0.0', build: 900 };
function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dice-posix-manager-'));
  fixtures.push(root);
  fs.copyFileSync(manager, path.join(root, 'dice-next'));
  fs.copyFileSync(core, path.join(root, 'dice-next-server'));
  fs.chmodSync(path.join(root, 'dice-next'), 0o755);
  fs.chmodSync(path.join(root, 'dice-next-server'), 0o755);
  file(path.join(root, 'web/dist/index.html'), 'old');
  file(path.join(root, 'config/private.json'), 'private config');
  file(path.join(root, 'data/dice.db'), 'private database');
  file(path.join(root, 'data/plugins/custom.js'), 'private plugin');
  file(path.join(root, 'data/helpdoc/custom.json'), 'private help');
  return root;
}
function stage(root) {
  const pending = path.join(root, 'updates/pending');
  fs.mkdirSync(pending, { recursive: true });
  for (const [name, binary] of [['dice-next', manager], ['dice-next-server', core]]) {
    fs.copyFileSync(binary, path.join(pending, name)); fs.chmodSync(path.join(pending, name), 0o755);
  }
  file(path.join(pending, 'start.sh'), '#!/bin/sh\nexec "$(dirname "$0")/dice-next" "$@"\n');
  fs.chmodSync(path.join(pending, 'start.sh'), 0o755);
  for (const name of ['web/dist/index.html', 'i18n/en.json', 'docs/roadmap.md', 'data/plugins/example.js', 'data/helpdoc/example.json']) file(path.join(pending, name), 'new');
  file(path.join(pending, 'update.json'), JSON.stringify(metadata));
  return pending;
}
function run(root, args = [], env = {}) {
  const result = spawnSync(path.join(root, 'dice-next'), args, {
    cwd: '/', encoding: 'utf8', timeout: startupTimeout,
    env: { ...process.env, DICENEXT_CONTAINER: '', DOTNET_RUNNING_IN_CONTAINER: '', container: '', KUBERNETES_SERVICE_HOST: '', ...env },
  });
  assert.equal(result.status, 0, result.stderr || String(result.error || result.signal));
  return JSON.parse(fs.readFileSync(path.join(root, 'last-run.json'), 'utf8'));
}
const result = root => JSON.parse(fs.readFileSync(path.join(root, 'updates/last-result.json'), 'utf8'));
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function waitFor(callback) {
  for (let i = 0; i < 1000; ++i) { if (callback()) return; await pause(10); }
  assert.fail('process fixture did not become ready');
}
try {
  {
    const root = fixture();
    const direct = spawnSync(path.join(root, 'dice-next-server'), ['config', 'direct'], { cwd: root, encoding: 'utf8', timeout: startupTimeout });
    assert.equal(direct.status, 0, direct.stderr || String(direct.error || direct.signal));
    const info = JSON.parse(fs.readFileSync(path.join(root, 'last-run.json'), 'utf8'));
    assert.equal(info.pid, Number(fs.readFileSync(path.join(root, 'direct-first'), 'utf8')));
    console.log('PASS: direct exec restart closes the previous instance lock');
  }
  {
    const root = fixture(); const pending = stage(root);
    file(path.join(pending, 'install-held'), 'download only');
    assert.equal(run(root).version, 'old');
    assert.ok(fs.existsSync(pending));
    file(path.join(pending, 'install-on-restart'), 'authorized');
    assert.equal(run(root, ['config with spaces', 'done']).version, 'new');
    assert.equal(result(root).success, true);
    for (const [name, content] of [['config/private.json', 'private config'], ['data/dice.db', 'private database'], ['data/plugins/custom.js', 'private plugin'], ['data/helpdoc/custom.json', 'private help']])
      assert.equal(fs.readFileSync(path.join(root, name), 'utf8'), content);
    console.log('PASS: held packages, explicit restart permission, replacement and private-data preservation');
  }
  {
    const root = fixture(); const pending = stage(root);
    file(path.join(pending, 'install-held'), 'held');
    const info = run(root, ['配置目录 with spaces', 'restart']);
    const firstPid = Number(fs.readFileSync(path.join(root, 'first-run'), 'utf8'));
    assert.notEqual(info.pid, firstPid);
    assert.equal(info.version, 'new');
    assert.deepEqual(info.args, ['配置目录 with spaces', 'restart']);
    assert.equal(info.cwd, fs.realpathSync(root));
    assert.equal(result(root).success, true);
    console.log('PASS: core-exit handshake, new process, CWD and complete argument preservation');
  }
  {
    for (const executable of ['dice-next-server', 'dice-next']) {
      const root = fixture(); const pending = stage(root);
      file(path.join(pending, executable), 'invalid executable');
      fs.chmodSync(path.join(pending, executable), 0o755);
      assert.equal(run(root).version, 'old');
      assert.equal(result(root).success, false);
      assert.ok(!fs.existsSync(pending));
    }
    console.log('PASS: invalid core or manager is rejected before replacement');
  }
  {
    const root = fixture(); stage(root);
    assert.equal(run(root, [], { DICENEXT_CONTAINER: '1' }).version, 'old');
    assert.ok(fs.existsSync(path.join(root, 'updates/pending')));
    console.log('PASS: container install guard');
  }
  {
    const root = fixture();
    const child = spawn(path.join(root, 'dice-next'), ['config', 'hold'], { stdio: 'ignore' });
    let corePid;
    try {
      await waitFor(() => fs.existsSync(path.join(root, 'last-run.json')));
      corePid = JSON.parse(fs.readFileSync(path.join(root, 'last-run.json'), 'utf8')).pid;
      const duplicate = spawnSync(path.join(root, 'dice-next'), { encoding: 'utf8', timeout: startupTimeout });
      assert.equal(duplicate.status, 1, duplicate.stderr || String(duplicate.error || duplicate.signal));
      child.kill('SIGKILL');
      await waitFor(() => child.signalCode !== null);
      const orphan = spawnSync(path.join(root, 'dice-next'), { encoding: 'utf8', timeout: startupTimeout });
      assert.equal(orphan.status, 1, orphan.stderr || String(orphan.error || orphan.signal));
      console.log('PASS: duplicate manager and orphaned live core retain the update lock');
    } finally {
      if (corePid) { try { process.kill(corePid, 'SIGTERM'); } catch {} }
      try { child.kill('SIGTERM'); } catch {}
    }
  }
} finally {
  for (const root of fixtures) fs.rmSync(root, { recursive: true, force: true });
}
