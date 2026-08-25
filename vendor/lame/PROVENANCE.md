# Vendored: LAME 3.100 (libmp3lame)

## What this is

The MP3 encoder used by `src/actions/action_mp3.c`, vendored in source form and
built as one static library (`vendor/lame/CMakeLists.txt`) so apprecorder stays
a single executable with no runtime dependency to ship or version.

## Release and verification

| | |
|---|---|
| Version | 3.100 (2017-10-13, the last 3.x release) |
| Upstream URL | `https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz` |
| SHA-256 | `ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e` |
| MD5 | `83e260acbe4389b54fe08e0bdbf7cddb` |
| Fetched | 2026-08-26 |

The checksum was corroborated against three channels that do not share a
distribution path:

1. **SourceForge**, the upstream project's own release hosting.
2. **The Debian archive** — `deb.debian.org/debian/pool/main/l/lame/lame_3.100.orig.tar.gz`
   downloaded separately and compared with `cmp`: **byte-identical**.
3. **nixpkgs** (`nixos-24.05`, `pkgs/development/libraries/lame/default.nix`),
   which pins `sha256 = "07nsn5sy3a8xbmw1bidxnsj5fj6kg9ai04icmqw40ybkp353dznx"`.
   Decoded from nix-base32 that is the same
   `ddfe36ca...b1da1e`.

LAME 4.0 exists as of 2026 (Homebrew and Arch have moved to it). 3.100 was kept
deliberately: it is the release with a decade of MSVC field use behind it, and
the encoder API this action needs (`lame_encode_buffer_interleaved_ieee_float`,
`lame_encode_flush`, `lame_get_lametag_frame`) is unchanged between them. A move
to 4.0 is a self-contained swap of this directory whenever someone wants it.

## Licence

**LGPL-2.0-or-later.** `COPYING` (GNU LGPL v2) and `LICENSE` (LAME's own notice)
are the upstream files, unmodified. `ChangeLog` is kept for the same reason.

apprecorder links this **statically**, which the LGPL permits but conditions:
§6 requires that a recipient be able to relink the application against a
modified libmp3lame. Satisfying that means shipping either the object files or
this source directory alongside a binary release, plus the licence text and a
notice that libmp3lame is used and is LGPL. That is a release-engineering
obligation, not a code one — flagged here so it is not discovered late.

The upstream tarball also carries a patent notice about MPEG audio. Those
patents expired in 2017.

## What was and was not taken

Copied verbatim from the tarball:

- `include/lame.h`
- `libmp3lame/*.c`, `libmp3lame/*.h`
- `libmp3lame/vector/{lame_intrin.h, xmm_quantize_sub.c}`
- `mpglib/*.c`, `mpglib/*.h`, `mpglib/AUTHORS`
- `COPYING`, `LICENSE`, `README`, `ChangeLog`

Left out, all of it build scaffolding or non-library code: `frontend/`
(the `lame` CLI), `Dll/`, `ACM/`, `dshow/`, `mac/`, `macosx/`, `misc/`,
`doc/`, `debian/`, `vc_solution/`, the autoconf/automake machinery, the
`libmp3lame/i386/*.nas` NASM assembly (this build is pure C — see `config.h`
note 3 for what replaces it), and the DLL resource files
(`libmp3lame/lame.rc`, `logoe.ico`).

## The one file that is ours

`config.h` is written by us, not upstream. It stands in for the `config.h`
autoconf would generate and is a reduction of upstream's own `configMS.h`, with
three deliberate differences documented at the top of that file: no duplicate
`stdint.h` typedefs, `HAVE_MPGLIB` without `DECODE_ON_THE_FLY` (so the test can
decode without the shipping binary carrying the decoder), and `MIN_ARCH_SSE` on
x86-64.

No `.c` or `.h` file under `libmp3lame/` or `mpglib/` was patched. That is the
property worth keeping: everything here diffs clean against the release.
