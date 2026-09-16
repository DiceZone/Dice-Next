# Dice!Next command and plugin compatibility report

Date: 2026-09-03

## 2026-09-16: Legacy Dice sample reply templates

- Compared with `Dice-Old/Dice-dev/Dice/DiceFormatter.cpp` (`MarkSampleNode`):
  uniformly choose a top-level option and expand only its selected subtree.
- Added `{sample:a|b}` to localized command/roll replies and persona templates,
  custom replies, and `.text`; retained existing custom-reply `{a|b}` syntax.
- Supports nested choices and existing placeholders; sampling precedes value
  interpolation, so player names and regex captures cannot inject sample macros.
- Uses an independent thread-local random source for cosmetic choices and keeps
  the existing cached Markdown-to-plain conversion. Recursion is bounded.
- Targeted regression: 8 cases / 68 assertions passed, including actual `.r 1d1`,
  nested variables, empty choices, deterministic option boundaries, malformed
  input, persona/fallback, and explicit Markdown/plain reply rendering.
- Full CTest: 405 core cases / 2078 assertions and 9 Lua cases / 71 assertions
  passed. Release server build and documentation-site build passed.
- Local implementation only; no release package, commit, or push performed.

## 2026-09-14: BDC cloud character cards

Protocol baseline: `ShiaNyaa/Better-Dice-Control` commit `b894848`, verified
against GitHub HEAD and the local `cloud_cards.py`, `oauth_device.py`, and
`cards/document.py` implementations. No BDC source or production player data
was modified during this adaptation.

Final verification:

- Release server and test executables build successfully.
- Full CTest: 381 core cases / 1863 assertions and 9 Lua cases / 71 assertions pass.
- Cloud-card filter: 17 registered cases (including the opt-in live probe),
  118 offline assertions pass. The separately enabled live probe passes 2/2
  assertions using the actual production HTTPS transport and public Discovery.
- Documentation-site `npm run docs:build` passes. Changes remain local/unpublished.

Coverage includes schema/type rejection, numeric/text/lock/unknown-field
conversion, least-privilege authorization, pending/slow-down/denied/expired
device states, adapter/player/native-identity isolation, key rotation/removal,
read-only scopes, revocation, restart without credential persistence, local
name collisions and ambiguous legacy rows, stable cloud IDs after rename,
offline bindings, server merge responses, 409 without retry or base revision
advancement, compare-and-swap protection against edits during HTTP requests,
and the bounded background queue.

The public BDC Discovery currently omits the device authorization endpoint.
After checking the official issuer, the client uses the verified same-origin
compatibility path `/api/oauth/device_authorization`; token endpoint discovery
still rejects other origins. API keys, access tokens and device codes never
enter ordinary reply logging or on-disk sync state.

Scope/remaining integration checks:

- This release provides explicit private-chat `.pc cloud` commands, not
  automatic background card synchronization or a WebUI conflict editor.
- BDC's current device grant returns no Refresh Token. Expiry/restart requires
  reauthorization; local cards and persistent ID mappings survive.
- Tests do not grant consent on a real player's behalf, access real private
  cloud cards, or prove authenticated production round trips. A user must
  complete the website consent flow with a verified adapter API key for that
  final end-to-end check.
- Separately stored local formula/weapon shortcuts are outside BDC schema v1;
  unknown fields in the card document are retained, not executed as commands.

Usage and compatibility boundaries: [cloud-cards.md](../../docs/cloud-cards.md).

## 2026-09-07: bot-off logging and operational command regression

Full local Release regression: 343 core cases / 1485 assertions and 9 Lua cases /
68 assertions passed (`ctest --test-dir server/build/tests -C Release --output-on-failure`).
These figures supersede the historical baseline below. No real adapters or log-site
uploads were used by the new tests; live platform delivery remains to be field-tested.

`test_log_bot_off.cpp` adds 19 cases / 130 assertions covering:

- Active incoming/final-reply transcripts continue during ordinary `.bot off`.
- Explicit `.log new/on` can start recording while off; pause/resume/end work
  independently, without implicitly waking dice commands or starting a transcript.
- Hard locks block management and delayed transcript writes; private messages do
  not enter group logs. Active-log pointers remain group/account-scoped and survive
  database reopen. Global silence/external-mode restrictions remain in place.
- The inbound blacklist gate still rejects events before command/recording dispatch.
- Literal `.reply` controls retain permissions; unrelated reply/plugin commands stay silent.
- The `.master` compatibility gateway reuses existing handlers, verifies the owner
  account (including official-platform native identity), and rejects unsupported
  operations instead of treating its arguments as arbitrary commands.
- Remote bot switches really persist on first write and do not affect other groups
  or adapter accounts. This regression exposed a null JSON `extra` in synthesized
  remote messages, now initialized before account-scoped settings are written.

This is not complete legacy `.master` parity: reset/delete/groupclr and other
unimplemented operations are explicitly rejected. Existing explicit-@ wake-up and
the separate `.dismiss` emergency behavior are unchanged.

## Outcome

The Release server builds successfully. The automated core suite passes all 286
test cases (1153/1153 assertions). The Lua compatibility suite passes all 9 test
cases (68/68 assertions without external files), including 123/123 assertions
when the user-provided real plugin corpus is mounted.

The real corpus includes:

- `ResourceSearchEngine.lua`, including cache reload and a detailed resource query.
- The supplied 求签 plugin directory.
- DailyNews load and scheduled `task_call news` execution, including its two
  asynchronous replies.

求签 and DailyNews remain ordinary third-party plugins. They are not treated as
built-in/systemized features and are not blocked.

## WebUI multi-instance session isolation

WebUI session cookies are now named from a stable identity derived from each
installation's session/config path. This keeps two Dice!Next installations on
the same host independent even though browsers do not isolate cookies by port,
while deliberately excluding the listening port so a port change or restart
does not discard a trusted-device session.

The legacy `dice_session` cookie remains accepted and is migrated through
`/api/auth/status`. Logout revokes and clears only the current instance's
session. Automated coverage verifies cookie-name stability, per-installation
separation, legacy migration, and scoped logout. A real HTTP flow also exercised
two instances on ports 18088 and 18089 with one cookie container: both stayed
authenticated, logging out of one did not affect the other, and the trusted
session survived its instance restart.

## Container update boundary

The updater now combines an explicit `DICENEXT_CONTAINER` image marker with
Kubernetes and Windows-container environment variables (including
`DOTNET_RUNNING_IN_CONTAINER`), the standard
`container` and systemd container markers, Docker/Podman marker files, cgroups,
and strongly scoped mountinfo signatures. False-like explicit values do not by
themselves classify a bare-metal process, and an ordinary host path containing
the word `docker` is not enough to trigger the mountinfo fallback.

When a container is detected, Release checks remain available and use the
container temporary directory so a read-only application root still works.
Manual download, manual installation, automatic download, and automatic
installation are rejected by the service itself. A persisted old automatic
download/install policy has an effective action of `notify`; it cannot bypass
the restriction through the API or a stale WebUI. The Windows distribution
manager independently checks container markers before applying an already
staged package; a launcher smoke test confirmed that the package remains
untouched.

Update HTTP subprocesses are cancellable and reaped on both Windows and Unix,
so stopping the service no longer waits for a 20-minute curl timeout. Manifest
source racing accepts the first valid response, cancels and joins slower probes,
and temporary updater files include a process-specific random token plus a
sequence number to isolate co-located instances. Automated coverage includes
container signals and API refusal paths as well as subprocess cancellation,
active-fetch shutdown, first-success racing, and temporary-name uniqueness.
The Docker publishing workflow now builds and starts an amd64 candidate before
pushing, then checks the runtime status and all self-update refusal paths.

## Fixed regressions

### Command routing versus plugin commands

Enabled JS and Lua plugins now get exact command-word ownership before legacy
compact prefix parsers. This prevents a plugin command such as `.ram` from being
consumed as core `.ra` with an attached argument.

The exception is an explicit list of real core command names and documented
aliases. Those continue to be handled by Dice!Next even if a plugin registers
the same name. The list now includes the previously missed legacy forms:

- `.h`, `.rsh`, `.rhs`, `.rah`, `.rch`, and `.drawh`.
- `.coc6`, `.coc7`, `.cocd`, `.coc6d`, `.coc7d`, and their historical trailing-`s` forms.
- `.mrrp` and `.zrrp`.
- `.boton`, `.botoff`, and the original black/white-list commands.
- Chinese aliases for long rest, death saves, and favor.
- Every audited original `.strXXX` key in the legacy message-key map.

The check is exact. It does not reserve broad prefixes such as `ra*`, `game*`,
or `str*`, so unrelated commands such as `.ram`, `.gameHelper`, and
`.strike` remain available to plugins. Per-group plugin enable/disable state is
also respected while probing ownership.

### Roll parsing and expression results

- Compact default-die reasons work again: `.rd测试` and `.rdtest` both roll the
  default die and retain the attached reason.
- A spaced DiceScript identifier remains distinct: `.r dtest` is evaluated as
  an expression rather than being silently rewritten as a reason.
- Multi-roll syntax `.r 2#d100` performs two independent rolls.
- DiceScript string, null, and array results no longer escape as `null` or an
  invalid roll. A roll command now requires an integer or floating-point result.
- Composite DiceScript details include the final value. For example,
  `[1,2,3].sum()+2d1` renders a complete trace ending in `=8`.
- `.dx5c10测试` and `.ww5测试` retain their attached reason without requiring a
  space.
- `.dx/.rdx` accept a trailing `+N` or `-N` final modifier. It is applied once
  after all Double Cross exploding rounds; malformed repeated modifiers are
  rejected instead of silently truncated. `.ww` parsing is unchanged.

One important intentional behavior was retained: in enhanced mode,
`[1,2,3]` is accepted by the preceding OneDice V1 engine and evaluates to
`3`, because OneDice defines a tuple's scalar value as its final element.
In DiceScript-only mode the same bare array is correctly rejected as
non-numeric. This is engine-specific behavior, not a null-result regression.

### Initiative and localization

Multi-entry initiative (`.ri N#name`) now has complete localized output and
localized lower/upper-bound errors. The real HTTP message chain was exercised
for `zh-Hans`, `zh-Hant`, `en`, and `ja`; no raw i18n keys or empty strings
were returned. A recursive bundle test also verifies that all four built-in
locale files expose the same leaf-key set.

### SealDice JS compatibility

- `seal.format(ctx, ...)` resolves standard `$t...` temporary variables from
  the current message context, including player, raw IDs, group, platform,
  game/rule system, date/time, privilege, and log state fields.
- `seal.getEndPoints()` returns endpoint snapshots rather than an empty
  placeholder array.
- Group name and active rule system are supplied by the host.
- `ctx.endPoint.userId` and `$t骰子帐号` use the account that actually received
  the message, rather than a process-wide fallback; this is covered with a
  multi-account-shaped message fixture.
- `$t个人骰子面数`, `$t群组骰子面数`, and `$t当前骰子面数` read the same
  personal `.set` override and group/rule default used by core rolls.
- JS command ownership checks honor per-group plugin state.

### Lua compatibility

- Exact Lua command-trigger discovery is side-effect free and honors group-only,
  trust, and per-group plugin gates.
- A Lua plugin command that uses an `@` mention as its target/argument is allowed
  through the same pre-router exception as a SealDice JS command; messages that
  explicitly address another registered dice bot remain ignored.
- Legacy sibling `loadLua`, load-time HTTP, `sleepTime`, `task_call`, mixed
  UTF-8/CP936 replies, invalid audit paths, and CP936 resource filenames under
  the Windows CRT UTF-8 locale are covered.
- Legacy single-file plugins now receive a fresh environment for each command
  and scheduled task, matching original Dice! behavior. Top-level HTTP data is
  therefore refreshed for DailyNews instead of being frozen at plugin load.
  Sibling `loadLua` scripts share that invocation environment without leaking
  globals into later invocations or other plugins.
- ResourceSearchEngine's old CP936 `QQBot/index` paths and stale absolute cache
  paths are remapped only below the active plugin directory. `.法术 reload`, a
  detailed spell query, and scheduled 求签/DailyNews callbacks pass with the
  real plugin files.

### Other corrections covered by this run

- The bundled JS deck example reads `seal.deck.draw()`'s result object correctly
  and no longer registers the same extension twice.
- NPC automatic recalculation has localized direct-change fallback text and
  includes HP/SAN/MP changes.
- Previously misplaced legacy i18n sections are restored to their top-level
  keys.

## Real service-chain checks

An isolated Release instance was started on loopback and exercised through
`/api/test/message`, which uses the same command router and plugin fallback
chain as live messages. Representative checks:

| Input | Observed behavior |
| --- | --- |
| `.rd测试`, `.rdtest` | Default D100 roll with the attached reason |
| `.r 2#d100` | Two independent results |
| `.r "abc"` in DiceScript-only mode | Localized non-numeric error |
| `.r [1,2,3].sum()+2d1` | Numeric result with full `=8` trace |
| `.dx5c10测试`, `.ww5测试` | Roll succeeds and reason is retained |
| `.dx5c10+3测试` | Final value is the Double Cross tally plus 3; reason retained |
| `.rdx7c7-10测试` | Legacy alias applies -10 once after all exploding rounds |
| `.ww5c8-3测试` | `-3测试` remains the reason; WW semantics are unchanged |
| `.ri 3#` | Three independently rolled, numbered initiative entries |
| `.ram` with a probe plugin | Plugin response wins |
| `.ra 100`, `.mrrp`, `.strRollDice show` with conflicting probe commands | Core response wins |
| `.user state`, `.cloud`, `.coc6d` | Non-empty compatible core response |

The isolated runtime and probe plugin were removed after the service shut down
cleanly.

## Verification commands

```powershell
cmake --build server/build --target dice-next-server --config Release -j 2
ctest --test-dir server/build -C Release --output-on-failure
server/build/tests/Release/dice-next-tests.exe  # detailed 286 / 1153 summary

$env:DICENEXT_LEGACY_RESOURCE_PLUGIN = '<ResourceSearchEngine.lua>'
$env:DICENEXT_LEGACY_FORTUNE_PLUGIN = '<求签 plugin directory>'
server/build/tests/Release/dice-next-lua-tests.exe
```

## Remaining boundary

This run verifies parsing, command routing, plugin execution, localization, and
the real HTTP message pipeline. It does not connect to a live OneBot, QQ
Official, Discord, or KOOK account, so platform network delivery and
platform-side rendering still require release-candidate smoke testing with real
bot credentials.
