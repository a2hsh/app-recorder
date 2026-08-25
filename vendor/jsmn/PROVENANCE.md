# Vendored: jsmn (JSON tokenizer)

## What this is

The JSON *reader* for session files (`src/session/session_load.c`). Design
section 9 names it by name: "JSON (hand-rolled writer; jsmn for reading)". The
writer stays ours because emitting JSON is trivial and a dependency there buys
nothing; parsing is where the sharp edges are.

jsmn is a single header, ~460 lines, no allocation of its own — it fills a
caller-supplied token array with `{type, start, end, size}` spans into the
caller's buffer. That is the right shape for this codebase: no heap, no
ownership, nothing to free on an error path, and a hard bound on how much a
malformed file can cost.

## Release and verification

| | |
|---|---|
| Version | `master` as of 2026-08-26 (upstream has no tag after v1.1.0; the header is the v1.1.0 line plus the `JSMN_HEADER`/`JSMN_API` split) |
| Upstream URL | `https://raw.githubusercontent.com/zserge/jsmn/master/jsmn.h` |
| SHA-256 | `c04533e9181e1e33baceb0f55ac449b05145bb936e8c68cc77dfe0d8277514fb` |
| Fetched | 2026-08-26 |
| License | MIT (`vendor/jsmn/LICENSE`), Copyright (c) 2010 Serge Zaitsev |

**Unmodified.** The file is byte-for-byte what was fetched. Configuration is
done entirely by the `#define`s in `session_load.c` before the include, so an
upgrade is a straight file replacement with no patch to re-apply.

## How it is configured, and why

`src/session/session_load.c` is the only translation unit that includes it:

```c
#define JSMN_STATIC   /* one TU; no symbol reaches the link */
#define JSMN_STRICT   /* primitives only where a value is legal */
```

- **`JSMN_STATIC`** keeps `jsmn_parse`/`jsmn_init` internal. Nothing else in
  the tree may parse JSON — the session module owns the format (rule 3).
- **`JSMN_STRICT`** is load-bearing rather than tidy. Without it jsmn accepts a
  bare primitive at the top level, so a text file that happens not to start with
  `{` parses as a one-token document and reaches our schema check as "a valid
  JSON document with no `apprecorder` object" rather than as "this is not JSON".
  Both are refused, but only one of them says the right thing.
- **`JSMN_PARENT_LINKS` is deliberately NOT defined.** It trades a bigger token
  for a faster in-place walk, and the reader here descends recursively with an
  explicit depth cap instead. A parent link would not remove the need for that
  cap, and the cap is what bounds a hostile file.

## LICENSE compatibility

MIT: permissive, no copyleft, no relinking obligation. Unlike
`vendor/lame` (LGPL — see its PROVENANCE for the static-link constraint on
release), jsmn imposes nothing on how apprecorder is distributed beyond keeping
the copyright notice, which `vendor/jsmn/LICENSE` is.
