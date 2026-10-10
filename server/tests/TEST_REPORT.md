# Dice!Next command and plugin compatibility report

Historical baseline date: 2026-09-03. Later entries below record their own scope;
targeted checks must not be presented as a fresh full-suite or live-platform run.

## 2026-10-10: SealDice-style command shortcuts

- Windows Release backend, tests and the credential-free preview driver built
  successfully. CTest passed the full core suite: 566 cases / 5569 assertions;
  the existing Lua compatibility suite passed 17 cases / 165 assertions.
- The shortcut-specific scope passed 20 cases / 396 assertions, covering group
  and personal CRUD, scope precedence and per-account isolation, appended
  arguments, caller permissions, bot-off/lock/blacklist/command/feature gates,
  recursion protection, rule-pack target rewriting, persistence and verified
  identity migration. Legacy account permission links remain intact under
  `.admin account-alias`; shortcut storage never replaces `dice/aliases`.
- A real Seal-compatible JS extension exercised solve/command hooks, expanded
  message text and segments, caller ID and privilege, and plugin disablement.
  Mock bridges also checked deliberate silence and ordinary plugin-generated
  messages. Markdown/plain composition, persona overrides, nested capture and
  exception restoration were tested; no live Lua alias delivery is claimed.
- WebUI type check, production build and all 205 tests passed, including the
  real backend preview renderer for localized shortcut variables/nested sample.
  The four-locale documentation catalog linked all 891 editable texts per locale,
  and the documentation production build passed.
- These are isolated local checks, not a new Release/CI result, browser visual
  acceptance or real bot/platform delivery. Optional external Lua corpora were
  not supplied for this run.

## 2026-10-05: Scheduled update installation and installation on any restart

- Windows Release builds of the backend, tests and launcher succeeded. The full
  core suite passed 527 cases / 3565 assertions; the new UpdateSchedule scope
  passed 17 cases / 151 assertions. No live bot account or real process restart
  was used: restart callbacks recorded intent in isolated package fixtures.
- Coverage includes opt-in defaults (04:00), strict HH:MM validation, next-day
  and fractional timezone calculations, persisted deadlines, changed settings,
  cancellation, interrupted preparation, stale metadata, same-tag download
  deduplication, overdue plans and failure without a repeated restart loop.
- Real fixture ZIPs went through download, size/SHA-256 verification and Windows
  staging for scheduled, immediate and download-only policies. Incoming restart
  authorization was discarded; only the core generated a valid permission.
  The launcher's shared gate permits any startup to apply an authorized package
  before the proactive deadline, and revokes that permission for notify/download.
- WebUI: 137 tests and production build passed; styled time controls, four
  locales, server-time display, capability guards and setting search are covered.
  The isolated preview API also passed defaults, scheduled download, cancellation
  and explicit restart checks, without network downloads or restarting a bot.
- Documentation catalog validation (810 backend text keys per locale) and the
  documentation production build passed. These are local results, not a new
  Release/CI run or live-platform upgrade validation.

## 2026-10-05: Weighted command replies and global persona pool

Release scope: build 926. Backend, WebUI and docs were fast-forwarded before
this work, then merged with startup-limits; no live bot account was connected.

- The failed release on 2026-10-04 (UTC+8), run 37156422056 at 56a1466,
  hit the same JSON-to-optional conversion ambiguity in cloud_card_diff.h on
  all five platform builds and both backend-test jobs. Commit 1a7fe1f already
  fixes that conversion. The targeted rerun passed 6 cases / 37 assertions.
  This is local verification, not a claim that the replacement CI run passed.

- Backend Windows Release executable and credential-free preview driver build
  successfully. After merging startup-limits, the full core suite passed all
  510 registered cases / 3414 assertions; the Windows launcher also builds.
  Lua suite: 17 registered cases / 165 assertions passed (optional external
  plugin corpora were not supplied).
- New coverage includes deterministic weighted tickets and zero weights,
  validation and readable import/export round trips, native variants containing
  nested legacy sample macros, Markdown/cached plain text, shared preview draws,
  literal user arguments, persona persistence/deletion/single-mode restoration,
  and actual router precedence for group/private selections.
- WebUI type check and production build passed. Full suite: 135 of 136 tests
  passed. The unchanged update-page test at system-update.test.mjs:158 expects
  LF bytes in about-page.tsx; this Windows checkout uses CRLF. The assertion
  passes after newline normalization; that unrelated source/test was not edited.
  All new weighted-template tests and the related 78-test subset passed,
  including tray settings and settings search after merging startup-limits.
- Docs: four-locale catalog check (810 linked texts) and production build passed.
- HTTP isolated preview exercised all nested sample branches; probability zero
  candidates were excluded. It uses the same template/transport rendering as
  the production preview endpoint and does not send platform messages.
- Browser automation failed twice with a host sandbox ACL initialization error.
  No visual/client or real-account delivery acceptance is claimed. The actual
  React pages remain available at the credential-free local preview for manual
  review. Backend and WebUI must be upgraded together; old-backend detection
  prevents saving the new native representation as literal reply text.

## 2026-10-04: Reply previews, Lua diagnostics and cloud comparison

Local implementation, not committed or published. Scope is deliberately split:

- Markdown/production text serializers: 21 cases / 76 assertions passed.
  Covers AI structure guards, literal plugin fragments, platform downgrade,
  QQ group/private/channel boundaries, KOOK cards and Discord literal output.
  No live AI request or actual platform delivery was made.
- Lua compatibility: 17 registered cases / 156 assertions passed. Literal TOML
  strings retain hashes, commas and regex backslashes; invalid cooldowns fail
  closed. Unsupported actions/conditions are visible and never partially run.
  Optional external corpus fixtures were not supplied. Legacy Dice! JS is
  intentionally excluded; JS uses the SealDice runtime and Python is not planned.
- Read-only cloud diff: 4 cases / 23 assertions passed. Includes missing versus
  null, arrays, escaped paths, parent deletion/type conflicts and bounded safe
  receipts. The new cloud-service integration test was added but not executed.
  Automatic sync and editable conflict handling remain design-only; no auto
  uploads, token persistence/refresh or force overwrite have been enabled.
- i18n/persona regression: 29 cases / 138 assertions passed.
- WebUI: 107 tests, TypeScript checks and production build passed; 3 PWA build
  artifact checks passed. Documentation-site production build passed. Existing
  bundle-size warnings remain. Local preview uses the production serializer CLI;
  manual UI checking confirmed QQ Markdown `msg_type: 2` becomes plain
  `msg_type: 0` when simulation is enabled, without saving or sending anything.
- The independent read-only frontend workflow is added, but has not run on
  GitHub because these changes have not been pushed.
- Existing native updater regression: 22 cases / 169 assertions passed again.
  This is not Windows package acceptance. The Windows checklist remains pending:
  [windows-update-acceptance.md](../../docs/windows-update-acceptance.md).
- The local machine lacks the full Drogon/CMake dependency environment. The
  complete backend executable and cloud-service integration suite were not built
  in this round; targeted standalone suites do not substitute for that check.

## 2026-10-03: Update recovery, version UI and default help

- Core `22904bb`: native update-service regression, 22 cases / 169 assertions
  passed. Covers cancellation, idle/attempt deadlines, source fallback and cache
  invalidation, partial cleanup, checksum rejection and retry. Stable-channel
  version smoke checks passed with `DICE_PRERELEASE=0`.
- WebUI `fe9060d`: 102 tests, TypeScript checks and production build passed.
  Includes version formatting, status reconciliation, request timeout, safe New
  icon, cancellation controls and the removal of redundant playground dividers.
- Core `75ec20d`: i18n/help regression, 29 cases / 138 assertions passed. Default
  help includes the two-line feedback group hint in all four locales and all
  three profiles; topic help and owner overrides retain their behavior.
- Release workflow [37108609089](https://github.com/DiceZone/Dice-Next/actions/runs/37108609089)
  completed successfully and beta.925 was published. The help-only `75ec20d`
  came later and is not part of that package; its security checks do not imply
  a new full release/test run.
- No new real-account delivery or Windows cancellation/install end-to-end test
  was performed by these local checks. Prior full Windows test records are in
  [CI build notes](../../docs/ci-release-build.md); retain their original dates.

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
