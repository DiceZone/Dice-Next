# Bundled decks

`legacy-builtins.json` is the standard JSON edition of the decks bundled by
the original Dice!/Suhui family. The user-supplied consolidated collection is
kept intact, with the following missing compatibility names restored from the
original Dice! `mPublicDeck` table:

- `大写字母` and `小写字母` (aliases retained alongside `大写英文` and
  `小写英文`);
- `地支`, `硬币`, `扑克牌`, `麻将牌`, and `性别`.

The `性别` deck is also required by references inside the consolidated file.
Keep this collection in the read-only release `decks/` directory. User decks
belong in `data/decks/` and are loaded afterwards, so a same-named user deck
can override a bundled one without modifying release files.
