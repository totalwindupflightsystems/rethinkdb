# FTS Tokenizer & Porter Stemmer — Design Decisions

## Implementation

- **Class:** `ql::fts_tokenizer_t` in `src/rdb_protocol/fts_tokenizer.{hpp,cc}`
- **Regex engine:** re2 (bundled) — `\w+` pattern for word tokenization
- **Stemmer:** Self-contained Porter stemming algorithm (Porter 1980)
  - Steps 1a–5 implemented in anonymous namespace
  - No external dependencies
- **Stop words:** Standard SMART IR system stop list (~120 words)
- **API:** `tokenize(text)` → stemmed+filtered tokens, `tokenize_raw(text)` → lowercased only

## Sindex FTS Architecture (in progress)

- `sindex_fts_bool_t` enum: `REGULAR = 0, FTS = 1`
- Stored in `sindex_config_t.fts` and `sindex_disk_info_t.fts`
- Serialization via `ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE` and `RDB_DECLARE_SERIALIZABLE`

## Next Tasks (FTS-3)

The FTS-3 task needs:
1. `fts_tokenize()` ReQL term — wraps the tokenizer for direct use
2. `fts_match()` ReQL term — queries FTS indexes
3. Wiring FTS-aware index creation into sindex execution path

## Build Notes

- Build system auto-discovers `.cc` files via `find` — no Makefile changes needed
- Incremental build: ~2 min
- GitReins: Tier 1 guard passes clean (secrets, lint, tests)

## Pitfalls

- When adding fields to `sindex_config_t`, ALL call sites of the old 4-arg constructor must be updated (or use default args)
- `sindex_disk_info_t` constructor uses a default arg for `fts` — existing call sites with explicit args still work
