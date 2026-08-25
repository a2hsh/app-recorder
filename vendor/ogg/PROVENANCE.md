# Vendored: libogg 1.3.6

## What this is

The Ogg container used by `src/actions/action_ogg.c`, vendored in source form
and built as one static library (`vendor/ogg/CMakeLists.txt`) so apprecorder
stays a single executable with no runtime dependency to ship or version.

libogg does not encode anything. It is the page layer: it takes the Opus
packets the encoder produces, wraps them in `OggS` pages with a CRC and a
granule position, and that page structure is the whole reason a killed
recording still plays — see the kill section of `action_ogg.c`.

## Release and verification

| | |
|---|---|
| Version | 1.3.6 (2025-06-16) |
| File | `libogg-1.3.6.tar.xz` |
| Upstream URL | `https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz` |
| SHA-256 | `5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061` |
| SHA-512 | `2168548e57e23fadca4aa66351214b23d699bc3aa47f5d065f8b583a3fe85c99333014a6aa38e4021b0e3750115b88258a6c00cac1e1496cdee5f12cdc633814` |
| Size | 439,952 bytes |
| Fetched | 2026-08-26 |

Corroborated across three channels that do not share a distribution path,
which is the bar `vendor/lame/PROVENANCE.md` set:

1. **Xiph's own release hosting** (`downloads.xiph.org`, which redirects to the
   OSU Open Source Lab mirror). This is the copy the tree was taken from.
2. **The Debian archive** —
   `deb.debian.org/debian/pool/main/libo/libogg/libogg_1.3.6.orig.tar.xz`,
   downloaded separately and compared with `cmp`: **byte-identical**. Debian
   re-hosts the tarball on its own infrastructure, so this is an independent
   copy rather than a second read of the same one.
3. **nixpkgs** (`pkgs/by-name/li/libogg/package.nix`), which pins
   `hash = "sha256-XIJTQo4YGEDNINQfPKFlV6nMBLrUo9BMzoSAhnf6EGE="`. Decoded from
   SRI base64 that is exactly `5c825342...fa1061` above.
4. **Gentoo's distfile Manifest** (`media-libs/libogg/Manifest`) independently
   records the SHA-512 and the byte count in the table above. Gentoo hashes
   were computed by a different maintainer at a different time from a
   different fetch, which is the point of listing a fourth.

1.3.6 is the current release; 1.3.5 (2021) was the previous one and is what
Debian stable carried until 2026. There is no reason to prefer the older one —
the 1.3.6 changes are UBSan fixes, allocation-failure handling and warning
cleanups, all of which this build wants.

## Licence

**BSD-3-Clause.** `COPYING` is the upstream file, unmodified, and `AUTHORS`
and `CHANGES` are kept with it.

This is a materially easier release position than `vendor/lame`'s LGPL (design
section 8.1): there is no §6 relink obligation, so nothing has to ship
alongside the binary except the copyright notice and the three-clause text. A
release must reproduce `COPYING` in its documentation. That is all.

## What was and was not taken

Copied verbatim from the tarball:

- `include/ogg/ogg.h`, `include/ogg/os_types.h`
- `src/bitwise.c`, `src/framing.c`, `src/crctable.h`
- `COPYING`, `AUTHORS`, `CHANGES`, `README.md`

Left out, all of it build scaffolding or non-library material: the autoconf and
automake machinery (`configure`, `Makefile.in`, `m4/`, `aclocal.m4`, …),
`cmake/`, `win32/` (a VS2015 solution), `doc/`, the `.spec` files, the pkg-config
templates, and `src/Makefile.am`'s test programs.

`include/ogg/config_types.h.in` is deliberately absent, and unlike
`vendor/lame` **there is no file in this directory that is ours**. Upstream's
`os_types.h` defines `ogg_int16_t` and friends directly from `<stdint.h>` on
`_MSC_VER >= 1800`; the generated `config_types.h` is only reached on the
non-Windows fallback path, which this build never takes. So every file here
diffs clean against the release, with no exceptions to remember.
