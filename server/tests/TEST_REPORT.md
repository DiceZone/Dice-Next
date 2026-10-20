# Dice!Next command and plugin compatibility report

Historical baseline date: 2026-09-03. Later entries below record their own scope;
targeted checks must not be presented as a fresh full-suite or live-platform run.

## 2026-10-21: SSO merge acceptance (local)

- Merged the core and WebUI `feat/sso` implementations without dropping image
  management, list-avatar fixes or the Windows short-path alias repair. Existing
  branches and the original SSO worktrees are retained.
- The SSO subset passed **11 cases / 217 assertions**: PKCE, verified QQ email
  identities, current local trust, persistent/revocable identity sessions,
  command-test sender binding, interruptible background-task shutdown and
  restricted access. The write allowlist rejects legacy migration, rule-package
  modification and unknown new endpoints. Plugin-list/config/storage reads and
  backup downloads are denied; oversized/unparseable JSON is refused rather
  than returned with credentials intact.
- A separate diagnostic compiled against the actual SSO header confirmed that
  the previously missed plugin-list, migration and rule-upload/delete paths are
  now blocked. It used fake credentials and did not launch a bot or call BDC.
- Merged Windows Release server/core/Lua/resource builds passed. The unfiltered
  core run passed **892 cases / 50,496 assertions**, including the SSO cases,
  with `TMP` and `TEMP` set to the real `E:/DICERE~1` short path. The Lua run
  passed **66 cases / 611 assertions**; the isolated resource run passed **26
  cases / 139 assertions** under the short path (a subset of the core total).
- WebUI passed **340 of 343 tests**, with zero failures and three opt-in backend
  preview tests skipped; type checking and the production build passed. Five new
  tests exercise reverse-proxy bases, login/bind URL encoding and one-time
  callback-result cleanup. All four locales have the same 54 SSO translation
  keys. Release automation passed **33 tests**.
- No real OIDC/BDC identity, provider registration, adapter connection or live
  message was exercised. HTTPS callback/provider behavior still needs live
  acceptance; local checks do not claim a successful remote Release.

## 2026-10-21: Windows short-path image aliases (local fix)

- Release run [39475215795](https://github.com/DiceZone/Dice-Next/actions/runs/39475215795)
  built all five platform packages and passed WebUI/Linux tests, but the Windows
  core run had one failed assertion: the migrated module's work-relative old
  image path was not registered. CI uses a `RUNNER~1` temporary-path component;
  the instance data root was canonical while the source path was not.
- Reproduced the same failure locally before the fix by running the existing
  owner-migration case under `E:/DICERE~1` rather than `E:/DiceRefactor`.
  The fix canonicalizes the already safety-checked source for relative aliases,
  retaining both original and canonical absolute aliases. Existing link/reparse
  and source-root checks remain in place.
- A new Windows regression obtains a real 8.3 path, imports Unicode owner/image
  names and checks relative/absolute aliases before and after reopening the
  resource index. The short-name branch actually ran on this host. The resource
  suite passed **26 cases / 139 assertions** under both full and short temporary
  roots; these are repeated checks, not 52 distinct cases.
- Windows Release server/core/Lua builds passed. Unfiltered runs passed **881
  core cases / 50,279 assertions** and **66 Lua cases / 611 assertions**. SSO is
  not included in these results; a new remote Release result is not yet claimed.

## 2026-10-21: Managed image files, portable deck archives and list avatars (local)

- Added authenticated image-file management with server-side metadata paging,
  directory operations, explicit image preview/code copying, shared upload/picker
  entry points, recycling/restoration and image ZIP import/export. Entering a
  directory or inserting an existing image code does not download image bytes.
  Deck images stay under the exact deck filename; plugin/module images stay
  under their owner and retain internal subdirectories.
- Twenty-five isolated image-resource cases passed **126 assertions**, covering
  upload/content validation, literal search/paging, owner layout, known-reference
  protection, rename/alias/share continuity, rollback/recovery, partial catalog
  restoration and bounded malformed templates. ZIP fixtures cover traversal,
  ambiguous names, CRC corruption and encrypted input. Deck round trips retain
  weights/nested sample, collect static cross-file dependencies and reject missing
  local assets rather than exporting incomplete packages. Recycled aliases do
  not fall back to retained legacy files; unmigrated old cache URLs still work.
- A real SQL/config/deck integration case adds **11 passing assertions**, updating
  stored references and live deck caches together while retaining other values.
  A real Lua-manager case adds **5 passing assertions** for managed image read/write
  aliases, unchanged compatibility copies and unchanged ordinary file IO. Dynamic
  plugin paths cannot be exhaustively identified; source scripts are not rewritten
  and uncertain old private files are retained for compatibility/recovery.
- Group and player list/card/detail avatars share one component. Four frontend
  cases check QQ/OneBot aliases, Milky, virtual/OpenID rejection, lazy loading,
  visible error fallback and identity changes after a failed image. This verifies
  URL selection/component behavior, not QQ CDN availability or live bot profiles.
- Windows amd64 Release server, core, Lua and isolated image-resource builds passed.
  Unfiltered native runs passed **880 core cases / 50,266 assertions** and **66 Lua
  cases / 611 assertions**. The isolated 25-case resource run is a subset already
  included in the core total, not additional coverage to add to that total.
- WebUI passed **335 of 338 tests**, zero failures; three opt-in production-backend
  preview tests were skipped. Image-management/avatar tests execute the actual
  component callbacks/effects with deterministic substitutes. TypeScript and the
  production WebUI build passed; the documentation-site build also passed. The
  pre-existing large-bundle advisory remains non-fatal.
- Browser checks used the real narrow-screen React pages with isolated in-memory
  data: path-only listing, explicit image preview and nonempty avatar fallbacks.
  No real bot was launched or platform messages sent. Optional external plugin
  corpora, native Linux/macOS runs, Windows symlink-creation branches and Release
  CI/publication are not claimed. Results were recorded before publication;
  the Release workflow and published assets must be verified separately.

## 2026-10-20: Data-volume selection and server-paged game sessions (local)

- Dashboard storage now queries the filesystem containing `Database::path()`
  (the primary database actually opened), not every mounted/fixed drive or a
  hard-coded macOS root. Canonical paths follow symlinks; POSIX file bind mounts
  query the file itself. Failed queries return no disk and an explicit UI
  unavailable state, not a guessed system drive. The UI labels whole-volume
  usage and exposes the selected data path; it does not claim folder size.
- System-info regressions cover duplicate Linux mount devices, component-boundary
  and longest-prefix selection, root overlays, file bind mounts, optional fields,
  escaped spaces/backslashes/tabs, invalid paths and real Windows Unicode paths.
  Eight registered cases produced **28 passing assertions** with MSVC; directory
  symlink creation was unavailable on this host, so that conditional branch did
  not execute. Linux/macOS mount parsing uses fixtures; no native execution on
  those systems is claimed.
- Game-session listing now reads four indexed configuration fields in one
  query, combines active and historical session metadata inside SQLite, and
  filters/sorts before SQL LIMIT/OFFSET. Count and page items share one SQL
  statement; only page items are serialized for a paged response. No message or
  image scan is performed. Materialized CTEs avoid repeated JSON projection;
  group-name reads are bounded to requested IDs except global group-name search.
- Seven game-session cases / **73 assertions** cover 2,507 sessions (2,100 active),
  first/middle/last pages, stable global sorting, source deduplication, status
  counts, global name/code/group/GM/player search, literal SQL metacharacters,
  malformed metadata, integer overflow, deletion/page clamping, empty results
  and the old no-page array contract. An SQLite authorizer forbids message-table
  reads and all database writes during listing. First/middle/last queries took
  about **46 ms combined** in one local in-memory fixture run, including response
  assembly but not HTTP, platform lookups or the preceding configuration query;
  this is not a deployment latency guarantee.
- A real Lua-manager configuration-snapshot case adds **13 assertions** for index
  scope, selected fields only, nonexistent/invalid entries, corrupted indexes,
  literal key binding and unchanged source data. WebUI tests execute the real
  hooks/page callbacks with deferred responses: 20 rendered cards for 2,507 total,
  independent pages, server search/sort, retry, page clamping, stale responses,
  unmount and stale operation-completion reloads. Frontend selects and pagination
  remain shared styled components; these are deterministic component/hook tests,
  not browser-layout or live-bot acceptance.
- Windows amd64 Release server/core/Lua builds passed. Unfiltered native runs
  passed **854 core cases / 50,129 assertions** and **65 Lua cases / 606 assertions**.
  Optional external-plugin corpus environment variables were not enabled in this
  run. BDC owner SSO is recorded as pending in the roadmap, not implemented.
- WebUI **322 tests passed**, zero failures/skips; TypeScript lint and production
  build passed. The existing large-bundle advisory remains non-fatal. These local
  results do not establish cross-platform CI or Release publication success.

## 2026-10-19: macOS atomic shared-pointer build compatibility (local)

- Release run 39204426328 failed in the [macOS arm64 compile job](https://github.com/DiceZone/Dice-Next/actions/runs/39204426328/job/117904252765).
  Its Xcode 15.4 libc++ lacks the C++20 atomic shared-pointer specialization;
  diagnostics name the deferred-reply state and both JS/Lua directory snapshots.
  The same run passed Windows/Linux backend tests and all four other packages;
  publication was correctly skipped, not partially declared successful.
- Replaced these three specializations with shared_ptr atomic free functions,
  already used elsewhere in the application. Atomic load/store/exchange retain
  the previous sequentially consistent semantics, immutable snapshot ownership
  and cancellation behavior; no unsynchronized pointer reads were introduced.
  The compiler, minimum operating-system requirements and safety gates did not
  change. The fix is based on main a1626de and retains intervening optimizations.
- Added two CI source-compatibility checks, bringing the Release Python tests
  to **33 passing tests**. A native regression repeats concurrent posting,
  counting and cancellation 32 times, checking queue bounds and that canceled
  callbacks cannot deliver. Existing JS runtime-lock and Lua directory/reload
  regressions were rerun, not replaced by the source check.
- Windows amd64 Release server, core and Lua test builds passed. Unfiltered
  CTest passed in **164.55 seconds**: core **833 cases / 49,992 assertions** and
  Lua **63 cases / 668 assertions**, zero failures. All four previously reviewed
  external Lua corpora were enabled with the same guarded test doubles.
- No local Apple SDK/macOS execution is claimed. The newly dispatched Release
  must confirm macOS compilation and complete publication separately; the local
  Windows and source checks do not establish that final result.

## 2026-10-19: Legacy conveniences final follow-up (full local regression)

- Completed the seven audited convenience areas: bounded text functions and
  render-local assignments, shared case/grade selection, Lua message fields and
  safe dot paths, scoped multi-level help aliases, resource directories, and
  range/member session-deck generators. Lua len counts raw tables/Sets without
  invoking length/iteration metamethods; native game.pls/gms counts also accept
  numeric member IDs. Identity display uses the invoking account's names/cache,
  not a different account or an on-demand network lookup.
- File decks now share the bounded template evaluator and a copy-on-write
  catalogue snapshot. Selected branches retain weights, depletion, media and
  escaping; inserted nicknames/captures never become dice blocks or macros.
  Catalogue reentry, reload during rendering and recursive decks are covered.
  JS directories read immutable metadata without waiting on the runtime or
  executing accessors/Proxy traps; Lua stored-variable conditions are checked
  without consuming probabilities, cooldowns or daily quotas. Dynamic speech
  keys and SelfData-file conditions remain conservatively omitted.
- wait uses bounded asynchronous replies or yieldable Lua message coroutines,
  never a formatting-thread sleep. Tests cover selected-branch accumulation,
  opaque inserted controls, previews, memory/count limits, cancellation,
  coroutine context and reload. Native deferred delivery also checks the exact
  connection/account, permission snapshot, blacklist, group state and runtime
  generations. Changed group plugin/reply/rule settings cancel prepared work;
  shutdown cancels queued callbacks before host teardown. Existing migration
  expectations now distinguish supported waits from still-preserved JS/Python.
- Final Windows amd64 Release builds succeeded for the server, core tests, Lua
  tests and standalone production preview. After the last source changes,
  unfiltered CTest passed both executables in **135.53 seconds**: core **829
  cases / 49,848 assertions**, Lua **63 cases / 668 assertions**, zero failures.
  This supersedes, rather than adds to, the focused totals below.
- All four external Lua corpus entries were actually enabled: ResourceSearchEngine,
  fortune/DailyNews, official PC_Inventory and PC_Slots. Reviewed sources were
  copied unchanged into guarded fixtures; HTTP, platform delivery and timers
  use test doubles. This is not an operating-system sandbox for hostile Lua.
- WebUI type checking and production build passed; **298 tests passed with zero
  skips**, including the actual C++ preview for nested samples/references and
  non-blocking waits. All four documentation locales linked **903 editable
  texts** and the documentation production build passed. Release automation
  Python tests passed **31/31**; Git diff checks passed in all three repositories.
- No live bot/listening production server or real platform moderation/message
  delivery was exercised. Linux/macOS compilation and real account delivery
  remain separate CI/platform acceptance, not claims implied by these local
  results. Arbitrary template scripts, virtual-message privilege escalation,
  persistent template writes and host-secret traversal remain unsupported.

## 2026-10-19: Safe legacy conveniences, contextual templates and generated decks (first focused pass)

- Native replies and Lua speech now share bounded sample/case/grade plus
  integer ran, UTF-8 text-byte len, expired, render-local var, validated at and
  display-only print. Computed values use opaque slots until final substitution;
  inserted nicknames, captures, fields and local values are never template code.
  Markdown/code-span escaping, FormFeed escaping and reference budgets remain
  covered. No script, blocking wait, persistent mutation or permission bypass
  was added to the template evaluator. Assignment names cannot be dotted paths.
- Lua message get/format/echo reads scalar fields and bounded public dot paths.
  Internal fields/functions are denied, false is preserved in returned replies
  and coroutine results, and formatting retains the invoking message rather than
  reading an unrelated global context. Existing coroutine/reentry coverage was
  rerun. Help aliases resolve only after topic selection, under locale/source/
  scope precedence with bounded cycles; authored aliases remain in management
  data. Custom-reply help references use that same selection path.
- Public directories filter current account/group/user limits. Lua uses atomic
  immutable trigger metadata; inspecting directories does not run scripts or
  consume cooldown/daily/probability state. Variable-conditioned Lua entries are
  conservatively omitted at this stage (the final follow-up adds pure stored
  conditions and JS snapshots). Hidden decks and the installation-wide bot inventory
  are not exposed, and output is capped at 200 entries / 32 KiB.
- Session range/member generators preserve the existing .deck set semantics,
  GM/admin/owner checks and transactional save protection. Ranges reject invalid
  steps and overflow; member lookup stays on the current adapter. OneBot's
  previous empty member-list implementation now reads its response cache using
  a deduplicated ID vector and a name index, avoiding per-user network lookups
  and quadratic JSON copies. Empty caches request a refresh and require retry.
  Generated member cards are literal: weights/macros are not interpreted, and
  CQ-action prefixes/page controls in nicknames are neutralized.
- Final focused Windows native runner passed **225 cases / 1,670 assertions**,
  including command/help/import/nickname/weighted-template/FormFeed regressions,
  persona/Markdown/presentation tests and the new convenience checks. Relevant
  production sources were rebuilt; unchanged support objects were reused.
  Real Lua tests passed **62 cases / 655 assertions**, with all four opt-in
  corpora actually enabled: ResourceSearchEngine, fortune/DailyNews, official
  PC_Inventory and PC_Slots. Sources were copied unchanged into guarded fixtures;
  network/transport/scheduling were test doubles, not live platform operations.
- Windows amd64 Release server/Lua targets built successfully. WebUI type
  checking, production build and **295 tests passed** (297 registered, 2 optional
  live-renderer checks skipped); documentation build and diff checks passed.
  This is not a full-core or cross-platform run. No real bot/listening server,
  actual moderation/message delivery, commit, push or Release was started.

## 2026-10-19: Legacy roulette self-moderation and invalid private targets (local)

- The supplied roulette plugin calls `eventMsg('.group G ban U M', 0,
  getDiceQQ())` after a hit. The old host treated this as a normal private
  command and sent its permission receipt back to an empty/zero bot identity;
  `.group ban` was also not implemented. A narrow compatibility bridge now
  accepts only integer-minute self-moderation for the native invoking group,
  player, plugin and account. It does not expose general `.group ban`, restore
  arbitrary virtual administrator privileges or accept duration expressions.
- Group disable/lock/removal, blacklist, global silence, plugin selection,
  adapter support, verified bot role and protected group members are checked.
  Zero minutes means unmute; the upper bound is 30 days. Unknown roles request
  refresh and fail closed. Other virtual self-calls execute without receipts;
  ordinary group/user events retain their previous reply path.
- Native invocation provenance cannot be rewritten by Lua `msg` fields.
  Virtual-origin markers survive nested events, waiting coroutines and queued
  dispatches. WebUI test calls and scheduled virtual plugin commands cannot use
  this bridge to acquire moderation privileges. Host markers are thread-local.
- OneBot identity is established from valid inbound `self_id`, preserved across
  missing/zero lifecycle fields and only updated by correlated login responses,
  not arbitrary member/stranger API objects. The cache is synchronized; invalid
  private/group sending targets are rejected locally before platform submission.
- Focused native regressions passed **188 cases / 1,458 assertions**, including
  seven new bridge/identity cases plus the prior help, command, migration,
  nickname, weighted-template and FormFeed coverage. Real Lua tests passed
  **60 registered cases / 557 assertions**, including the new provenance/yield
  case; optional external-corpus cases remain conditional on their fixtures.
- An unchanged copy of the supplied roulette file was exercised against the
  production event bridge with an isolated database and fake adapter: valid
  identity submitted one ban request, invalid identity and virtual escalation
  submitted none, and all scenarios submitted zero receipt messages. The
  source plugin was not changed or bundled into the repository.
- Windows amd64 Release server/Lua targets and documentation build passed.
  This was not a full-core, cross-platform or live-platform verification. No
  real bot, listening server, actual ban, commit, push or Release was started.

## 2026-10-19: Custom help library and source filters (local)

- Historical `help.topic.*` overrides were retained in the database but omitted
  from the fixed-topic help catalog, leaving them unclassified in the text
  editor and unavailable to normal `.help` lookup. The catalog now enumerates
  actual loaded help templates, including override-only custom topics and
  previously omitted builtin topics. No data/schema rewrite or reimport is
  required; raw text, locale, format and persona precedence are preserved.
- Management browsing does not evaluate samples or persona templates. Runtime
  help renders only the selected template; JS/Lua priority and lazy Mod help
  rendering remain covered. Builtin overrides retain builtin provenance;
  ASCII-case variants, empty entries and shadowed sources are handled
  consistently. Embedded rule-pack documents retain their original scope name
  while reporting rule-pack origin.
- The library and source counts share a catalog, with category, exact-source
  and search filters applied before paging. Tests include 20,010 plugin topics
  plus a custom entry beyond the first page. The frontend routes standalone
  global help out of unclassified text, preserves command/persona editing,
  edits the displayed content locale and keeps explicit format metadata.
  Versioned exports include help overrides in every language and custom files;
  imports still accept the original file-only formats and validate before writes.
- The focused Windows runner passed **181 cases / 1,403 assertions**, including
  seven new help-library cases, existing visibility checks, legacy commands,
  BotInfo, migration, group/nickname and weighted/sample/FormFeed regressions.
  Changed i18n and affected test sources were freshly compiled; unchanged
  production objects were reused. Real Lua help regression passed **2 cases /
  26 assertions**. The existing Mod-priority test now loads real builtin bundles
  instead of relying on missing-resource placeholder keys.
- Windows amd64 Release server and real Lua test targets built successfully.
  WebUI type checking/build and **294 tests passed**, with 2 optional live
  production-renderer tests skipped (296 registered). Documentation build and
  diff whitespace checks passed. This was not a full-core, cross-platform or
  live-HTTP run; no real adapter, listener, commit, push or Release was started.

## 2026-10-18: Legacy FormFeed message breaks (local)

- Original Dice! maps `{FormFeed}` to U+000C and splits the outbound message;
  it is not an ordinary newline. Next now recognizes the authored control in
  i18n/persona templates, custom replies, deck entries and Lua Mod templates.
  BotMsg uses the template renderer before final identity interpolation.
- Explicit page boundaries are processed before optional length-based splitting,
  including on platforms exempt from automatic splitting. Leading, trailing,
  consecutive and whitespace-only pages do not submit empty messages. The
  scoped Lua sender also respects these explicit boundaries. Existing deck
  names, Lua context/speech precedence and escaped markers remain intact.
- Migration recognizes the builtin instead of reporting an unavailable variable
  or missing deck. Case variants and nested sample are covered; interpolated
  nicknames/captures containing the marker are not reparsed. Preview serializes
  each page independently before Markdown downgrade, and the editor renders
  separate message previews without offering text styling for this control.
- **8 new MessageBreak cases / 105 assertions passed.** The combined targeted
  Windows runner passed **113 cases / 773 assertions**, including previous
  BotInfo, group, nickname, migration and weighted/sample regressions. Changed
  i18n, deck, configuration and version sources were freshly compiled; unchanged
  production objects were reused. Real Lua targeted regression passed **4 cases
  / 43 assertions** (FormFeed, speech precedence and help/script lookups).
- Windows amd64 Release server and real Lua test targets built successfully.
  WebUI type checking, production build and **288 tests passed**, with 2 optional
  production-renderer tests skipped (290 registered). Documentation build passed.
  This was not a full-core or cross-platform run, and no real adapter, listening
  server or Release was started for this verification.

## 2026-10-18: .bot system information and BotMsg migration (local)

- The first three `.bot` lines are generated by the program. The author name
  uses persisted `i18n.default_locale`, not a browser's interface preference or
  the current message's language. Compatibility follows the actual expression
  order resolved for the receiving account. Legacy headers remain below these
  fixed lines, followed by the editable BotMsg.
- New and older configurations without `bot_msg` receive the default feedback
  notice. An explicit empty string stays empty. Global, platform and account
  appendices retain the existing scope priority and survive configuration reload.
- `strBotMsg` and `strBotHeader` migrate to active configuration, retaining the
  owner's content. Migration normalizes line endings and appends the default
  notice exactly once at the bottom; an existing non-default appendix is kept
  unless overwrite was explicitly enabled, with the original conflict reported.
- **10 new BotInfo cases / 65 assertions passed.** A combined targeted Windows
  runner passed **97 cases / 569 assertions**, covering the actual repository
  BotInfo, legacy-import release, group lifecycle/account scoping, nickname,
  `.me` and log-name/face tests. It freshly compiled the changed configuration
  and version sources and reused unchanged production objects. This is not a
  new full-core or cross-platform test run.
- Windows amd64 Release server build passed without starting the executable.
  WebUI type checking, production build and **286 tests passed** (288 registered,
  2 optional live-renderer tests skipped). BotMsg tests cover exact text and blank
  persistence, UTF-8 size limits, scope metadata, four-language help and search.
  The VitePress documentation build passed; only existing chunk-size warnings
  remained. No real adapter, listening server, group departure or Release ran.

## 2026-10-16: Issues #51 / #52 management regressions (local)

- #52: the AI settings write schema discarded vision.pass_url, after which
  read-side defaults re-enabled it. Read/write now share the production vision
  normalizer. Explicit false survives repeated saves and restart; clients that
  omit this newer field preserve an existing value. Tests cover both states,
  defaults, image-count bounds, invalid boolean types and file persistence.
- #51: actual command handling records a precise UTC timestamp per adapter
  account while retaining the existing inactivity-day semantics. Group DTOs
  expose account and aggregate times. Historical date-only values retain their
  precision; missing history is not treated as evidence of inactivity.
- Group lists, cards, tables and details show the time, with recent/older
  sorting. Manual selection is bounded to 100 groups, supports pagination and
  confirms the exact bot/account targets. Changing search/account/tab clears
  selection. Failed submissions remain selected; leaving the page cancels
  remaining submissions. Records, character sheets and transcripts are kept.
- Destructive API actions reject offline/unsupported adapters, missing or
  mismatched account/native-endpoint bindings and already-left/removed/leaving
  groups. OneBot V11 and Milky advertise the implemented leave capability;
  platforms inheriting the no-op method cannot report a submitted leave.
- Stable-source Windows amd64 Release server and both test executables built.
  Full core regression passed **743 cases / 48928 assertions**. Default real-Lua
  regression passed **528 assertions** across 58 registrations; the four
  optional external corpus entries were not enabled (54 self-contained cases).
- WebUI full regression passed **276 tests**, with **2 optional production
  renderer tests skipped** (278 registered), including 13 new management/AI
  cases. TypeScript/production build passed. Four-language documentation
  catalog validation and VitePress build passed (existing chunk warnings only).
- Browser checks used the real React pages and an isolated in-memory preview,
  not a running Dice backend: exact-account selection, unsupported/multi-bot
  restrictions, date-only display, filter clearing, styled confirmation and
  partial-failure retry selection were observed. No real group leave, live
  credentials, external-model request or Release was performed. Linux/macOS
  and platform-confirmed leave delivery were not locally certified.

## 2026-10-16: Named resources and native Mod help followup (local)

- Three added cases failed before their fixes: case-varied named scripts picked
  the first Mod instead of the last loaded override; a directory init.lua had no
  callable package name; and uppercase HELP / mixed-case cross-Mod help aliases
  returned no text. Lookup now follows the original case-insensitive namespaces
  without changing stored UTF-8 paths or display names. Package-name aliases
  retain Next's explicit foo.init spelling and existing path/link/disable checks.
- The same merged help index now supplies listings and template references.
  Aliases may cross Mods and chain, duplicate topics follow load order, disabled
  overrides disappear on reload, and cycles cannot hang the catalog. Descriptor
  key events retain declaration-order override semantics without the quadratic
  object lookup cost of ordered_json for large query/help Mods.
- Selected native .help entries and explicit/legacy reply help references use
  the real Lua template implementation with message context. Catalog generation
  remains raw and does not pre-render every entry. Router bridge tests retain
  builtin/JS priority and scope filtering; the core executable uses a test
  callback, while separate real-Lua tests verify case-folded help references,
  message-first precedence, speech/sample recursion and context-sensitive aliases.
- Template recursion is bounded by depth, reference work, result length and
  cumulative intermediate bytes; adversarial fan-out and oversized inputs return
  empty rather than allocate indefinitely, and a later valid render still works.
  No unsafe language/runtime/permission boundary was relaxed.
- Final stable-source Windows amd64 Release server and both test executables
  built successfully. Full core regression passed **735 cases / 48885 assertions**;
  full real-Lua regression with all four corpus entries active passed **58 cases /
  612 assertions** (54 self-contained cases plus four external entries). With
  external paths unset, the same executable passed **528 assertions**, with 58
  registrations but four early-return corpus entries. The targeted native-help
  case passed 11 assertions; it is part of the full core run, not extra coverage.
- The documentation catalog passed all 902 editable keys in four languages and
  VitePress built (existing large-chunk warning only). The 246 corpus source
  files retained the preceding content/path digest; no original plugin was
  modified. No live Dice service, real account delivery, commit, push or Release
  was started. Linux/macOS verification remains outstanding. Earlier same-day
  sections below are historical scopes, not the current final counts.

## 2026-10-16: Audited external Mod/plugin corpus followup (local)

- The complete Lua executable with all four optional external entries enabled
  passed **54 cases / 571 assertions** (50 self-contained cases plus four
  external entries), with no early-return corpus entries in that run. These
  totals supersede, not add to, the earlier same-day Lua scope below.
- External inputs: ResourceSearchEngine with its original cache/dictionaries;
  the seven fortune decks, fortune entry and DailyNews-fixed; old official
  PC_Inventory; and PC_Slots. Tested resource detail/reload, moved and mixed-
  encoding cache paths, fortune dependencies, fresh news fetches, task output,
  inventory edits/deletion, counted items, status rename, spells/feats, and
  journal creation/update/detail/list/deletion, including nested loadLua.
- A real PC_Slots run initially returned no text for `查看法术`. Original Dice!
  uses case-insensitive speech dictionaries; Next's case-sensitive lookup was
  wrong. Speech now folds ASCII keys at loading/lookup, preserves variable
  precedence, recursively expands aliases and selects JSON/YAML array entries
  uniformly. Empty arrays are safely empty and mixed invalid arrays are not
  partially activated. Synthetic tests cover case-varied overrides/reload,
  aliases, loops, Unicode keys, scalar/array variants and message variables.
- Two new isolated regression cases failed before the script-environment fix:
  a second directory invocation inherited temporary helper/counter globals;
  an event inherited counter 3 from two suspended replies. File/text script
  entrypoints now get their own environment, while nested loadLua shares that
  invocation's environment and registered function closures retain Next's
  existing captured-local behavior. Reverse-order continuation and synchronous
  message/event context regressions now pass.
- A third new regression reproduced nested eventMsg dispatch replacing the
  outer message: `Outer1:Outer1:1` incorrectly became `Inner:inner-value:9`.
  Dispatch now restores the previous global message on every return path, with
  runtime-generation protection. File scripts and registered function closures
  both render the correct outer message after the nested call.
- Only audited text corpora were executed. Inputs are copied without links or
  reparse points under bounded size/count limits. Test-only Lua guards restrict
  files to the copied workspace and disable shell/process/native-library/default
  IO/environment introspection; HTTP and outbound events are doubles. This is
  defense in depth for these samples, NOT an OS sandbox for hostile code, and
  does not relax or replace production permissions. No real network or adapter
  send was performed. Corpus scripts/data are not vendored into the repository.
- The supplied PC_Slots delete-item script parses `name` but uses undefined
  `item`; this is a source-plugin bug, not justification to retain variable
  pollution. Its supported `物品-名称` path is covered. The two overlapping
  inventory Mods were tested separately, not installed as if their same-named
  replies/scripts were independent. Platform-specific image-file existence,
  actual news-service availability and real delivery remain outside this run.
  Dispatch used synthetic numeric QQ IDs; source plugins that do arithmetic
  on fromQQ are not certified for platforms with opaque OpenIDs.
- Opt in with `DICENEXT_LEGACY_RESOURCE_PLUGIN` (ResourceSearchEngine.lua),
  `DICENEXT_LEGACY_FORTUNE_PLUGIN` (the 求签 directory with its sibling entry
  and DailyNews-fixed.lua), and `DICENEXT_LEGACY_OFFICIAL_MOD_ROOT` (containing
  PC_Inventory and PC_Slots). The optional entries skip when paths are absent;
  the final no-corpus run passed 487 assertions, with 54 registrations but only
  50 self-contained cases exercising behavior. Do not present registration
  counts as executed corpus coverage in that case.
- Final Windows amd64 Release server and test executable builds passed; the
  full core suite passed **734 cases / 48874 assertions**. This core run uses
  its existing Lua bridge doubles; the separate complete real-Lua run above
  verifies the runtime changes. The documentation catalog passed all 902 keys
  in four languages and VitePress built (existing large-chunk warning only).
  All 246 original corpus files retained their content hashes across the run.
  Linux/macOS and real-account sends remain unverified. No listening server,
  firewall change, commit, push or Release was performed.

## 2026-10-16: Account identity, Session decks and LogEnd followup (local)

- Final Windows amd64 Release server and both test executables built. The final
  isolated suites passed **734 core cases / 48874 assertions** and **47 registered
  Lua cases / 443 assertions**. Two Lua external-corpus entries were deliberately
  not enabled, so **45 self-contained Lua cases** ran. No failed assertions.
- Session-specific core filtering passed 10 cases / 106 assertions (a subset of
  the full run, not additional coverage to add to its totals). Fixtures verify
  original `meta/idxs/size` permutations, weighted copies, exhaustion/reset,
  explicit public copying, unchanged group-temp replacement behavior, shared
  Session areas, independent Sessions, source gates, atomic concurrent depletion,
  checked-save failure without consumption and invalid-state rejection.
- Migration fixtures retain original archives and activate only a fully valid
  collection; default repeat import does not refill a depleted pool. Separate
  real Lua SQLite tests preserve depletion across runtime reload and database
  destruction/reopen. Native command fixtures use a storage bridge double; these
  checks do not claim a live server end-to-end migration or adapter send.
- Lua identity tests cover two adapters, self-variable gates, unscoped fallback,
  unknown identities, native events and interleaved suspended continuations.
  Three-argument draw calls keep platform/account/chat; host callback exceptions
  are converted to Lua nil/error results, not allowed across the native boundary.
- LogEnd tests pass the actual host-built context through Lua and distinguish
  the real filename/path from a user-authored display title. Game-over regression
  re-enters `.game new` from a hook after checking that the log is closed: the
  game lock is not held. Completed-game hooks do not retain a mutable old Session.
- Concurrent text/reference expansion and public-source reload are covered by a
  bounded watchdog test. Session rendering runs outside the game/storage locks.
  Four-language Session help/messages and both served documentation pages and
  roadmaps were synchronized; catalog checks passed all 902 editable texts in
  each language and VitePress built (large-chunk warning only).
- MinGW C++20 syntax checking of the data-only Session header passed; this is
  not Linux/macOS verification. WSL did not provide a usable local environment.
  Real user plugins, real accounts and Linux/macOS runs remain outstanding.
  No listening server, firewall change, commit, push or Release was performed.

## 2026-10-15: Safe legacy Dice! Mod compatibility expansion (local)

- Windows amd64 Release server, core test executable and Lua test executable
  built successfully. Final isolated runs passed **721 core cases / 48749
  assertions** and **42 registered Lua cases / 403 assertions**, with no failed
  assertions. The Lua count includes two optional external-corpus entries that
  return early when their opt-in environment paths are absent: **40 Lua cases
  actually exercised self-contained fixtures**. No real user plugin was executed.
- New fixtures cover full TOML multiline/quoted/inline declarations, empty and
  weighted replies, typed false/zero/text/empty values, IDs, self/user/group
  conditions, dice policies, persistent shared cooldown/day quotas, mixed Lua
  counter tables and queued locks. Invalid conditions/actions remain fail-closed.
- Clock/startup/day hooks use a simulated server clock. Tests cover coroutine
  resume, scheduler failure, reload/destruction cancellation, event-default
  admission and account preservation across immediate/event/delayed sends.
  Native poke policy context uses the actor, not the bot recipient. Request
  hooks cannot grant permission beyond the host and cannot undo another veto.
- Nested/disabling/management fixtures cover namespaced scripts, Chinese paths,
  last-loaded overrides, same-named directory/single-file IDs, rule-aware manuals,
  game gates and text-tape reply reuse. Core integration checks protect recovery
  commands, censor/disable/plugin gates and log closing before LogEnd publication.
- Numeric model checks cover integer DSL preservation, explicit floating maths,
  precedence, short circuit, ternary/default expressions, bounded parsing,
  invalid/divide-by-zero/overflow rejection, XML data extraction and deterministic
  enabled-mod priority. Arbitrary XML scripts are never evaluated.
- The MIT toml++ 3.4.0 header is byte-for-byte identical to the reference copy
  (SHA-256 `bc03fbb9e73e732c10f07308fde72e02e29f6e57ed37f36069baf890e51d5b62`).
  It adds no vcpkg dependency. The documentation catalog and VitePress build
  passed; VitePress reported only its existing large-chunk warning.
- This is local verification, not a Release or Linux/macOS/live-adapter claim.
  No listening server, firewall change, commit, push or publication was started.
  Old JS/Python/bytecode, arbitrary XML code, unsafe policy hooks and remote
  auto-install remain excluded. Next's single-selected-reply dispatch and
  synchronous decision hooks are retained; see
  [the compatibility scope](../../docs/legacy-mod-compatibility.md).

## 2026-10-12: Release Linux timeout and updater shutdown race

- Release run 38234306582 compiled and packaged all five platforms, but Linux
  `dice_next_tests` timed out after 900.04 seconds while running
  `ExplicitRestartInsideContainerIsStillAnOrdinaryRestart`. The separate Lua
  executable passed; publication was skipped. This was a test-process hang,
  not a compiler error.
- Before patching production code, a new local Windows stress case reproduced
  the hang on the fourth repeated run of 2000 updater lifecycles. Its 15-second
  watchdog terminated the process with exit code 2. The idle worker can read a
  false stop predicate, miss a notification sent without its mutex, then sleep
  forever while the destructor waits in `join()`.
- The updater now changes its atomic stop flag while holding the same mutex as
  the worker's condition-variable wait. Notification and joining remain outside
  that lock. Atomic reads still support cancellation in active manifest probes
  and download transfers; installation/container policy is unchanged.
- Three regression cases cover immediate/yielded/ordinary-restart shutdown
  across 2000 lifecycles, active-download cancellation with transfer joining and
  partial-file cleanup, and queued-install cancellation without restarting or
  releasing a held package. The original container-restart, portable-worker and
  active-manifest shutdown cases also have watchdog protection.
- Final local Windows amd64 Release server and test executables compiled.
  The targeted `Update` filter passed 45 cases / 999 assertions. Twenty further
  stress runs completed all 40000 lifecycles. Full CTest then passed 615 core
  cases / 6672 assertions and 22 Lua cases / 209 assertions in 80.76 seconds.
- No usable local Linux/WSL environment was available. The shared fix has not
  yet been verified by a new Linux CI run, and these checks do not certify a new
  Release. These were local verification checks only; no listening server or
  firewall change was involved. Release success requires the subsequent CI run.

## 2026-10-12: QQ official faces and repeated configuration reloads

- Local Windows amd64 Release server and test executables built successfully.
  Final CTest passed 612 core cases / 5993 assertions and 22 Lua cases / 209
  assertions. The documentation site built. No listening server, live QQ
  account, commit, push or Release was started for these checks. The preceding
  uncommitted reverse-WebSocket address-cache work remains intact.
- Incoming QQ built-in faces become readable `[表情]`; custom faces take their
  actual image URL from an attachment or explicit URL metadata, never from
  `faceId=0`. Adapter-boundary and actual log-storage tests cover mixed text,
  multiple faces, ordinary/image-only attachments, no duplicated attachment,
  scheme-less URLs, invalid ext data, CQ parameter escaping and non-image
  attachments. Existing raw CQ and other-platform text remain unchanged.
- TXT (including the no-config renderer), CSV's shared content conversion,
  Seal JSON and Parquet rows use standard CQ image references. Historical
  official face markers normalize at export; a missing historical URL remains
  `[图片]`, since the marker alone cannot recover it. CSV routing was checked
  in source and compiled; no live authenticated HTTP export was invoked.
- Six configuration cases cover 100 unchanged reloads without callbacks or
  snapshot writes, whitespace-only edits and no-op saves, valid edits applied
  once, invalid edits preserving the old state, immediate external edits after
  an own save, stable auto-generated API keys and temporary/session/nested file
  exclusion. The native Windows watcher receives a burst of 300 unrelated
  writes, then applies one real JSON edit without a reload loop. Windows keeps
  one pending directory read and waits for cancellation before releasing its
  buffer/handle; the Linux implementation was updated in source only, not built
  or run on Linux here.
- Dice-Next-log-renderer was fetched from origin. Each branch was updated
  independently: master at 1f6fbe5, pr-for-main at 3c78a84, legacy at 5cfb59a.
  Separate worktrees preserve their distinct code/history; no branch was merged
  into another. Each passed seven tests and its own vue-tsc/Vite production
  build. Checks cover raw and HTML-escaped face filtering, filtering disabled,
  malformed/literal text, existing CQ/mirai/image filters, editor token matching,
  CQ image query parameters and escaped parameters during rkey refresh.

## 2026-10-12: Cached reverse-WebSocket connection addresses

- Final local Windows amd64 Release core and test executables built. CTest passed
  601 core cases / 5799 assertions and 22 Lua cases / 209 assertions. WebUI type
  checking and production build passed; 220 tests passed with two optional checks
  skipped. The documentation site built. No commit, push or Release was made for
  this feature during these checks.
- Six service cases verify startup prefetch, independently published LAN data
  while WAN is delayed, cached reads without redetection, coalesced refreshes,
  explicit failed/stale states and recovery, browser-independent periodic
  refresh, cancellation on shutdown and native interface enumeration. Network
  state never claims an active check before its worker starts.
- Eleven frontend cases exercise the actual hook with deterministic effects and
  timers, the actual API helper with mocked fetch, address/port validation,
  separate LAN/WAN values, interface selection, coalescing, completion/failure
  polling cleanup, old-core errors, unmount safety, onboarding write suppression,
  all four languages and the shared styled dialog. Opening reads the server
  cache; only an explicit refresh posts a detection request.
- Native Windows interface enumeration was read-only. Public detectors were
  mocked; no external address-service availability, inbound NAT/firewall
  reachability, live OneBot connection or other-platform build was claimed.
  No server/listening smoke was started and no firewall setting was changed.

## 2026-10-12: Six-level logs and correlated message processing

- The final local Windows amd64 Release backend and both test executables built.
  CTest passed all 595 core cases / 5747 assertions and 22 Lua cases / 209
  assertions. Optional external Lua corpora were not supplied. All 31 Release
  automation tests passed; WebUI type checking and production build passed,
  with 209 tests passing and two optional checks skipped. The documentation site
  also built. Existing bundle-size/compiler warnings were not hidden.
- Logger cases verify TRACE/DEBUG/INFO/SUCCESS/WARN/ERROR ordering, independent
  SUCCESS, default INFO, legacy aliases, four human-readable fields, separate
  source/thread diagnostics, bounded recent records, secret/inline-media
  redaction and malformed UTF-8 replacement. Output plans, actual bodies,
  segments and failures retain the same internal msgID and processing source.
- JS/Lua checks cover named self-reported levels, delayed/timer/sleep origins,
  cross-plugin await continuations and preserved reply text. Rejected JS command,
  initialization and timer Promises report errors rather than completed work.
  Protocol helpers distinguish outgoing events from read-only polling and do
  not copy base64 file bodies into human logs. Final async/event amendments were
  covered by these local compilation and unit checks.
- An isolated real-core smoke with a local fake OneBot peer passed standard roll,
  JS/Lua/timer replies, actual/proposed contents, failure correlation, level API
  validation, heartbeat suppression and API-key redaction. No real account,
  external service or production data was connected. Early fixture connection
  timing failures were repaired in the harness, not by relaxing core behavior.
- The three Windows smoke attempts used copied executables with HTTP/OneBot
  listeners and triggered firewall prompts. Their processes and temporary
  directories were verified cleaned up. The driver now refuses Windows startup
  without explicit `--allow-windows-listener` consent; that refusal was tested
  without opening listeners. No firewall settings were changed, and no further
  Windows listening smoke was run.
- These are local checks, not acceptance of live QQ/Milky/KOOK/Discord delivery,
  tray behavior, other architectures or a completed GitHub Release.

## 2026-10-12: Windows Release macro compatibility

- Release run 38178243240 failed both Windows package builds at
  `script_execution.h:22`: Windows' function-like `min` macro expanded the
  new `std::min` call. The macro-safe call preserves the shared nested deadline;
  no production execution budget or cancellation mechanism was relaxed.
- A separate lightweight compilation unit deliberately defines `min`/`max`
  before including the header on every platform. Its two cases / ten assertions
  verify expired outer deadlines and restoration after a shorter inner budget.
- Local Windows amd64 Release backend and both test executables built. CTest
  passed all 587 core cases / 5689 assertions and 21 Lua cases / 202 assertions;
  all 31 Release automation tests passed. Optional Lua corpora were not supplied.
- The healthy Lua native-pipe test now has a two-second startup budget and checks
  captured output plus successful/nonzero exit status. Slow-command cancellation
  and looping finalizers remain in the separate 100 ms watchdog-bounded test.
- These are isolated local checks, not an ARM64 build, live bot delivery or a
  claim that the replacement Release has completed.

## 2026-10-12: Runtime reliability fault injection

- Final local macOS arm64 Debug core, unit tests and real Lua runtime compiled.
  The full core suite passed 598 cases / 5816 assertions and Lua passed 20 cases /
  191 assertions, including metadata stack baselines and direct JS deletion.
- New watchdog-bounded tests exercise JS/Lua infinite loops, caught errors,
  getters/string conversion, nested coroutines, memory exhaustion/recovery,
  timed mutex admission, failed/self-clearing timers, pathological regex,
  queue overflow, worker exceptions, native subprocess timeout and looping Lua
  finalizers. Tests fail the process rather than hang CI if interruption breaks.
- Isolated real POSIX service smoke passed with `--reliability`: timed-out JS
  commands and loading, Lua commands and sleep continuations, concurrent auth
  reads, visible suspension hints, deletion/reload, normal roll recovery and
  shutdown while a JS worker was executing an infinite loop. A 16+ MiB synthetic
  log also verified bounded dashboard tail reads. The supplied standalone
  `是还是不是.lua` passed the normal upload/delete/reload/exit smoke with the final
  binary. No bot accounts, real messages or external APIs were used.
- Windows native process-tree/tray behavior and malicious native library/process
  isolation were not tested locally. Optional external Lua corpus variables were
  not supplied. CI workflows and compile parallelism settings were not changed.

## 2026-10-12: Runtime logging and exception diagnostics

- Local macOS arm64 Debug backend, core tests and Lua tests compiled with the
  same pinned temporary dependencies used for the rule-reload fix. The final
  complete core suite passed 592 cases / 5780 assertions; Lua compatibility
  passed 17 cases / 156 assertions. Optional external corpus environment
  variables were not supplied. These are local results, not Release/CI or
  Windows/Linux live acceptance.
- Nine logger cases cover JSON/query/header/URL credential redaction, configured
  and short credentials, idempotence, multiline/control-character handling,
  UTF-8 truncation, nested/non-standard exceptions, log-level fallback, HTTP
  business failures, polling noise and native framework/file output. The
  registered Drogon exception callback is invoked with a nested exception to
  verify its HTTP 500 response, private request-body/header exclusion and
  preserved file-log cause; this is not a production broken-endpoint test.
- Two runtime diagnostics cases verify error-notification reentry/concurrent
  throttling and isolation of throwing message/event handlers. Existing
  rule-reload regressions remain in the full suite. Production notification
  delivery is queued to the event loop, with lifetime checks, so logging while
  holding a config/plugin lock does not synchronously send a notification.
- Both the minimal fixture and the supplied `是还是不是.lua` passed the expanded
  real-core POSIX smoke driver: upload/precheck, enable/disable, Lua/JS reload,
  delete/reupload, concurrent API reads and rolls, deliberate JS/Lua execution
  failures, request IDs, operation phases/durations, source/traceback records,
  dashboard parsing, API-key exclusion from files and graceful shutdown.
  No bot account, QQ UI or external service was used.
- One lightweight logger test compilation unit was added to the existing core
  test executable. No extra CI job, build matrix, dependency or heavy router
  compilation unit was introduced. See [runtime logging](../../docs/runtime-logging.md)
  for format, levels, privacy limitations and troubleshooting.

## 2026-10-11: Plugin deletion/upload rule-reload deadlock

- The supplied single-file Lua was not an infinite loop. Upload, toggle, delete
  and reload all called `reloadRulePacks`, whose exclusive `rulesLock` was
  reacquired by default-skill normalization through `canonical`. Waiting HTTP
  handlers also prevented a normal server shutdown. Default-skill lookup had a
  second recursive shared-lock path; both now use the existing unlocked query
  while their caller owns the lock.
- JS template reload now owns the same exclusive rules lock, its source
  templates are copied under the JS runtime mutex, and the explicit JS reload
  API synchronizes templates just like upload/toggle/delete already do.
- macOS arm64 local Debug backend, core tests and Lua tests compiled with the
  pinned library versions (temporary non-vcpkg dependency installation). The
  complete core suite passed 581 cases / 5729 assertions; Lua compatibility
  passed 17 cases / 156 assertions. New rule-reload tests cover default/alias
  restoration after upload, disable, enable and deletion, writer exclusion and
  concurrent default-skill checks. A bounded watchdog reports lock regressions
  instead of leaving CI hung; no extra compilation unit or CI target was added.
- `node tools/test-plugin-reload.mjs <core-binary> [lua-file]` starts a real core
  with complete split configuration, loopback-only HTTP, isolated temporary
  storage, no adapters, and automatic update/backup/cloud services disabled.
  Both the supplied Lua and a minimal reply plugin passed precheck/upload,
  disable/enable, Lua/JS reload, delete/reupload, concurrent API reads and rolls,
  followed by a successful graceful POSIX shutdown with no leftover process.
  The minimal plugin also verifies that disabled/deleted replies do not fire.
- These are local checks, not Release/CI or Windows tray acceptance. Windows
  tray exit still needs a Windows live run; the POSIX smoke tool explicitly
  does not claim to test Windows console/tray shutdown or arbitrary scripts'
  execution time limits. Optional external Lua corpora were not supplied.

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
