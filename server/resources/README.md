# Bundled release resources

`default-data/` contains the read-only defaults that must be present in every Dice!Next release:

- `decks/`: built-in deck files, packaged as top-level `decks/` so user overrides in `data/decks/` remain separate;
- `helpdoc/`: bundled COC and DND reference entries;
- `plugins/js/`: Dice!Next-owned example plugins only;

These files are deliberately stored outside `server/data/`. The latter is ignored runtime state and may contain private user data. Windows, Linux, and macOS packaging must fail when any required directory or example plugin is missing; do not restore an optional fallback to `server/data/`.
