import assert from 'node:assert/strict';
import { test } from 'node:test';
import { supportsNode } from './check-node-version.mjs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

test('Node security floor permits later patch and minor releases, not another major', () => {
  for (const version of ['26.12.1', '26.12.2', '26.13.0', 'v26.12.1']) {
    assert.equal(supportsNode(version, '26.12.1'), true, version);
  }
  for (const version of ['26.12.0', '26.11.99', '24.22.1', '27.0.0',
    '26.12.1-rc.1', '26.x', 'not-a-version']) {
    assert.equal(supportsNode(version, '26.12.1'), false, version);
  }
  assert.equal(supportsNode('26.12.1', 'bad-minimum'), false);
});

test('the real CLI validates the runtime and reports its exact version', () => {
  const result = spawnSync(process.execPath,
    [fileURLToPath(new URL('./check-node-version.mjs', import.meta.url))],
    { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  assert.ok(result.stdout.includes(process.version));
  assert.ok(result.stdout.includes('minimum secure version 26.12.1'));
});
