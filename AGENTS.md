# Repository-local agent rules

## WebUI V1 / V2 transition (mandatory)

- WebUI `main` maintains V1; `webui-v2` maintains V2 (preview) in the separate
  Dice-Next-WebUI repository. Keep these branches distinct.
- Until the human explicitly establishes V2 as stable and removes V1, every
  feature addition/removal or behavior/API change affecting the WebUI MUST be
  implemented and tested in BOTH versions (including i18n, search and docs).
  Do not implement a feature only in the version currently open in the browser.
- Preserve the dual bundle (`web/dist/v1`, `web/dist/v2`), the instance-wide
  `config/webui.json` preference, shared authentication and fallback to V1.
- Report any blocked counterpart sync explicitly. Never claim parity based on
  copying built assets, and never silently merge the V2 redesign into V1.

- Do not create a Git commit unless the user explicitly asks for one.
- When a commit is requested, use the repository's existing human-configured
  Git identity. Never set, replace, or inject an LLM/AI author or committer
  email, and never add AI `Co-authored-by` or similar attribution trailers.
- Stage and commit only the changes that belong to the user-authorized task.
  Preserve unrelated user work and never bundle opportunistic fixes.
- An inspection, diagnosis, or report request is read-only unless the user also
  asks for implementation. Report unrelated findings instead of fixing them.
- Do not push, publish, open a pull request, or otherwise modify a remote unless
  the user explicitly requests that remote action.
- Use `Fixes #N` only for an issue that the committed code actually resolves and
  that has been verified in proportion to its risk.
- For an authorized feature/fix commit, write a concise Chinese subject. A large
  change may add an optional `Release-Notes:` block of user-facing bullets,
  terminated by `End-Release-Notes`. Prefix bullets with `feat:`, `fix:`, `perf:`
  or `breaking:` when they need a category different from the commit subject.
  Keep test logs, internal prompts and attribution/email trailers out of this
  block. Without it, CI uses the subject; routine build-counter/CI/docs commits
  stay in the collapsed full history instead of the feature summary.
