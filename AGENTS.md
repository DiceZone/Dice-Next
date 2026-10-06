# Repository-local agent rules

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
