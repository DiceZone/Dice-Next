# Dice!Next command and plugin compatibility report

Date: 2026-09-01

## Outcome

The Release server builds successfully. The automated core suite passes all 280
test cases (1094/1094 assertions). The Lua compatibility suite passes all 9 test
cases (68/68 assertions without external files), including 123/123 assertions
when the user-provided real plugin corpus is mounted.

The real corpus includes:

- `ResourceSearchEngine.lua`, including cache reload and a detailed resource query.
- The supplied 求签 plugin directory.
- DailyNews load and scheduled `task_call news` execution, including its two
  asynchronous replies.

求签 and DailyNews remain ordinary third-party plugins. They are not treated as
built-in/systemized features and are not blocked.

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
server/build/tests/Release/dice-next-tests.exe

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
