# MCP server

MCP is opt-in (`server/mcp/enabled`, default `false`) and independent of bot AI.
The owner configures it at **系统设置 → MCP 服务** (`#/mcp`). User guide:
[MCP 服务](https://docs.dice.zone/manage/mcp.html).

## Boundary

- `POST /mcp`: dedicated hashed Bearer credentials, never WebUI cookies, BDC keys,
  management API keys or caller-supplied player identity.
- Streamable HTTP JSON responses: modern `2026-07-28` discovery, metadata and
  mirrored headers; legacy `2025-11-25` / `2025-06-18` initialization without
  session state. No stdio, old HTTP+SSE, automatic OAuth or arbitrary executors.
- `roll_dice`, `search_help`, `get_status`, `get_character_card` only. Card grants
  pin card IDs and owners, honor read locks, and never evaluate formulas.
- Pure rolls instantiate an isolated Dice!Next parser with checked integer math,
  bounded input/nesting, at most 1,000 random samples and 1,000,000 sides.
  Global expression-mode preferences and bot command behavior are not changed.
- Default access is loopback-only with Host validation and no forwarded
  requests. Remote use is explicit; exact Host and Origin allowlists, real TLS
  for direct remote peers, independently protected TLS reverse proxies.
- Per-credential and peer rate limits; 32 KiB input, 64 KiB output. Help traversal
  stops after 10,000 entries or 4 MiB of source text and reports truncation.
- `/api/system/mcp*` uses the existing owner management boundary. Restricted SSO
  admins cannot read or modify grants. Raw credentials are returned once only.
  The shared guard normalizes route case to match Drogon's routing semantics;
  MCP management additionally requires an affirmative authentication attribute.
  Browser writes also require JSON, a custom request header and same-origin
  Origin/Fetch Metadata validation; no cross-origin management CORS is exposed.
  Verified native owner API-key requests without browser metadata remain usable.
  Rotation invalidates the old token without changing scope/status/expiry.
  Editing preserves expiry unless `expires_days` explicitly requests renewal.
  Disabling/removing access works even for deleted/locked cards; changing card
  ownership requires explicit removal and re-authorization, not a label edit.
- Audit records in `data/audit/mcp.db` are bounded to 5,000 records and omit raw
  tokens, parameters and card data. Audit-enabled backups use SQLite's backup API
  to include live WAL state without changing the source database. Optional
  `client_id`/`outcome` filters apply before counting and pagination, including
  records for revoked grants. Host names are case-insensitive; Origin matching
  normalizes scheme/host case and default ports, never wildcard subdomains.
- MCP configuration is committed as one atomic `server.json` replacement, not a
  rewrite of all sections. After a successful commit, a secondary audit failure
  returns `warnings: ["audit_unavailable"]` without withholding the new token or
  falsely claiming the mutation failed. Tool calls still fail closed when they
  cannot write their required audit record.
- Two tool workers and one separate management worker use bounded queues (eight
  waiting requests per lane, five-second queue expiry). One queued/running tool
  call per credential is admitted; further calls return 429 / Retry-After: 1
  without executing. Initialization and metadata requests do not reserve this
  slot. Hashes, not raw Bearer values, identify transport admission slots.
  Queries have a five-second execution budget and shared cooperative stop token;
  traversal, dice samples and native-query boundaries check it. Shutdown signals
  cancellation before joining workers/closing stores. No detached threads or
  unsafe thread termination: an uninterruptible OS/DB call must return before
  its boundary check can run. Expired/cancelled results are discarded. Queries
  release the grant mutex while reading and revalidate scope before returning.
- WebUI reads time out after 10 seconds and writes after 20 seconds, with no
  automatic mutation retries. `X-Dice-MCP-Operation-ID` identifies an accepted
  write; the owner-only `GET /api/system/mcp/operations/{id}` exposes only its
  queued/running/succeeded/failed status and grant ID, never a token or body.
  The temporary ledger is bounded to 128 entries and expires completed entries
  after 10 minutes/on restart; unknown does not mean failed. Lost one-time tokens
  require a deliberate rotation after checking the current state. Resolving an
  uncertain write requires a successful settings read-back too; refresh failure
  or cancelled review retains the operation lock and existing editor draft.
- Restoring config/the main database invalidates old credential hashes, disables
  grants and the service, but retains grant labels/scopes for owner review. A
  restored grant cannot be enabled until explicitly rotated. The durable startup
  barrier `.dicenext-mcp-restore-reset` is cleared only after both configuration
  and recovery snapshot are safe; snapshot failure aborts startup and retains
  the barrier. Resource-only partial restores do not reset access. The actual
  payload is checked, not just manifest selection flags. Windows and POSIX use
  core's same JSON-aware restore path before opening config/databases.

The protocol adapter cannot call an Adapter, command router, template evaluator,
plugin executor or model. Its business-query callbacks are injected by main.cpp.

## Owner connection checks and grant visibility

`POST /api/system/mcp/self-check` checks the saved local service configuration,
actual audit writeability (a rolled-back test insert), and whether any usable
credential exists. An optional existing `token` performs initialization and
tool listing through the same local protocol handler; optional `tool` checks
membership in that returned list. It never executes a tool, creates a credential,
contacts a user-supplied URL, or stores/echoes the token. This is explicitly a
**local protocol check**, not verification of public DNS, TLS, a proxy, listener
reachability or browser Origin. A native client must still test its real route.

`POST /api/system/mcp/card-details` accepts at most 100 card IDs and an optional
grant ID. It returns only ID, name, owner and readable/unavailable/owner-changed
state, not attributes. Missing and read-locked cards deliberately share one
unavailable state. Both POST endpoints enforce the same owner and CSRF boundary
as credential management; they do not enter the mutation ledger.

Settings include `server_time`, `usage_available`, and each grant's nullable
`last_used_at`. Usage is the latest successful tool call in retained audit rows,
not initialization, self-check or label edits. Empty history is not proof of
never having been used. Audit failure is explicit and does not block revocation.
The WebUI advances from server time using a monotonic clock, updates expiry
badges while open, and marks credentials due within seven days. It does not
change expiry or send external expiry notifications.

## Verification

```sh
cmake --build server/build --config Release --target dice-next-tests
ctest --test-dir server/build -C Release --output-on-failure
```

With Node available in the build environment, CTest also builds and runs
`dice-next-mcp-probe` via `tools/test-mcp-http.mjs`. The probe binds only to
127.0.0.1, uses disposable configuration/credentials/databases, never starts a bot,
and is not shipped. Node is a test/build dependency, not a runtime requirement.

Unit and actual HTTP checks cover both protocol eras, structured real rolls,
denied tools/cards, forged identities, expiry/revocation, Host/Origin/CORS,
restricted admin boundaries, independent credentials, request limits, integer
overflow, expiry preservation, rotation, stale-card permission removal,
persistent filtered audit pagination, SQLite audit snapshots, CSRF, atomic-save
faults, audit faults, pending write recovery, slow queues and in-flight revocation.
Additional checks cover per-credential concurrency, cooperative deadline/shutdown,
lost-reply read-back failures, restored revoked keys, snapshot-recovery retries
and resource-only/mislabelled partial restore payloads.

For official-client HTTPS interop (required by Windows/Linux test CI and the
macOS packaging job):

```sh
npm --prefix tools/mcp-interop ci --ignore-scripts
cmake -S server -B server/build -DDICENEXT_MCP_INTEROP=ON
cmake --build server/build --config Release --target dice-next-tests
ctest --test-dir server/build -C Release --output-on-failure
```

This uses locked official SDK 1.32.1 / Inspector 2.12.0 against a disposable
loopback fixture behind an HTTPS prefix proxy, testing both protocol eras.
OpenSSL creates a temporary test CA; only child test clients trust it. TLS
verification stays enabled. These npm packages/certificates are not shipped.

WebUI browser regression uses an isolated Vite preview with in-memory example
data, never a production backend or personal browser profile:

```sh
# Run in the Dice-Next-WebUI repository.
npm ci
npx --no-install playwright install chromium --only-shell
npm run test:mcp-browser
```

It covers one-time token copying/clearing, local self-check results, server-clock
skew, pagination/filters, stale card summaries, stable editor size, narrow
screens, and failed settings read-back retaining the mutation lock. CI installs
Chromium's Linux dependencies and runs this suite before packaging the WebUI.
The macOS job builds only the server and isolated MCP probe, then runs the HTTP
and HTTPS interop checks; it does not redundantly build all backend unit tests.
