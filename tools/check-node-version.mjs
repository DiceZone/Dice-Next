// CI follows the latest Node 26 release, but must never accept an old cached
// runtime below the October 2026 security release. Keep this dependency-free.
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

export function supportsNode(version, minimum) {
  const parse = (value) => /^(?:v)?(\d+)\.(\d+)\.(\d+)$/.exec(value)?.slice(1).map(Number);
  const actual = parse(version);
  const floor = parse(minimum);
  if (!actual || !floor || actual[0] !== floor[0]) return false;
  return actual[1] > floor[1] || (actual[1] === floor[1] && actual[2] >= floor[2]);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const minimum = readFileSync(new URL('../.node-version', import.meta.url), 'utf8').trim();
  if (!supportsNode(process.versions.node, minimum)) {
    throw new Error(`Node.js ${process.version} is unsupported: require >=${minimum} within the same major release line.`);
  }
  console.log(`Node.js ${process.version}; minimum secure version ${minimum}`);
}
