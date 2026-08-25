# Vendored: libopus 1.5.2

## What this is

The Opus encoder used by `src/actions/action_ogg.c`, vendored in source form
and built as one static library (`vendor/opus/CMakeLists.txt`) so apprecorder
stays a single executable with no runtime dependency to ship or version.

## Release and verification

| | |
|---|---|
| Version | 1.5.2 (2024-04-12) |
| File | `opus-1.5.2.tar.gz` |
| Upstream URL | `https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz` |
| SHA-256 | `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1` |
| SHA-512 | `78d963cd56d5504611f111e2b3606e236189a3585d65fae1ecdbec9bf4545632b1956f11824328279a2d1ea2ecf441ebc11e455fb598d20a458df15185e95da4` |
| Size | 7,839,412 bytes |
| Fetched | 2026-08-26 |

Corroborated across three channels that do not share a distribution path:

1. **Xiph's own release hosting** (`downloads.xiph.org`, which redirects to the
   OSU Open Source Lab mirror). This is the copy the tree was taken from.
2. **The Debian archive** —
   `deb.debian.org/debian/pool/main/o/opus/opus_1.5.2.orig.tar.gz`, downloaded
   separately and compared with `cmp`: **byte-identical**.
3. **Gentoo's distfile Manifest** (`media-libs/opus/Manifest`), which
   independently records the SHA-512 and the byte count in the table above,
   computed by a different maintainer from a different fetch.

nixpkgs was checked as a fourth and confirms the **version** but not the
tarball: `pkgs/by-name/op/opus/package.nix` pins 1.5.2 by `fetchFromGitHub` of
the `v1.5.2` git tag, so its hash is a NAR hash of a source tree and is not
comparable to a release tarball's SHA-256. Recorded here so nobody re-derives
that dead end.

## Why 1.5.2 and not 1.6.1

1.6 (2025-12) and 1.6.1 (2026-01) exist. 1.5.2 was kept deliberately, for the
same reason `vendor/lame` stayed on 3.100:

- **Size.** The 1.6 line moves the neural-network features into the default
  build shape; its tarball is 10 MB against 1.5.2's 7.8 MB and 1.6.0's was
  35 MB. This build is shaped around a sub-1 MB image and does not compile the
  DNN at all (below), so the newer release buys nothing it can use.
- **Field time.** 1.5.2 is two years old, is what Debian stable ships, and is
  the release every distribution has been building against on MSVC.
- **The API this action needs is unchanged between them**
  (`opus_encoder_create`, `opus_encode_float`, `OPUS_GET_LOOKAHEAD`,
  `OPUS_SET_BITRATE`). Moving to 1.6.x is a self-contained swap of this
  directory whenever someone wants it.

## What it costs the image

**220.5 KB of the Release build**: `apprecorder.exe` is 784,896 bytes with
`src/actions/action_ogg.c` present and 559,104 without it, measured by removing
that one file and reconfiguring. That is against MP3's 58 KB, and it is the
number to hold this dependency to.

It is also the number to expect, and the reason is worth stating so nobody
spends a day trying to beat it: **there is no encoder-only slice of libopus to
carve out.** LAME's decoder is a separate library half (`mpglib/`) that shares
nothing with the encoder, which is why `vendor/lame/PROVENANCE.md` can promise
the decoder is absent from the shipping binary. Opus's decoder shares the range
coder, the MDCT, the FFT, the mode tables and most of SILK with its encoder.

An attempt to measure the decoder's marginal cost — forcing a reachable
`opus_decoder_create` into `apprecorder.exe` — grew the Release image by 2 KB.
So did the same experiment with `opus_multistream_encoder_create`, which is
definitely not otherwise linked, so the experiment **does not discriminate**
between "already in the image" and "the probe was dead-stripped by /GL /LTCG
/OPT:REF". No claim is made either way. What is claimed is the 220.5 KB above,
which is measured end to end and does not depend on knowing the answer.

## Licence

**BSD-3-Clause.** `COPYING` is the upstream file, unmodified; `AUTHORS`, `NEWS`
and `README` are kept with it.

Unlike `vendor/lame`'s LGPL-2.0-or-later there is **no relink obligation** to
satisfy at release time (design section 8.1): static linking BSD code requires
reproducing the copyright notice and the three-clause text in the
documentation, and nothing else. No object files, no source tree, no special
packaging. Ogg/Opus is the cheapest of apprecorder's four encoders to ship, and
that is worth knowing before a release, not during one.

The tarball also carries Xiph's patent statement (`COPYING`, the "Opus patent
license" section): the RFC 6716 reference implementation is covered by
royalty-free grants from Xiph, Microsoft, Broadcom and Google.

## What was and was not taken

Copied verbatim from the tarball:

- `include/opus.h`, `opus_custom.h`, `opus_defines.h`, `opus_multistream.h`,
  `opus_projection.h`, `opus_types.h`
- `celt/*.h` (all) and the 18 `.c` files of upstream's `CELT_SOURCES`
- `silk/*.h`, `silk/*.c` (all), and `silk/float/*`
- `src/*.h` and the 14 `.c` files of upstream's `OPUS_SOURCES` plus
  `OPUS_SOURCES_FLOAT`
- `COPYING`, `AUTHORS`, `NEWS`, `README`

Left out:

- **`dnn/` — 18 MB of the 22 MB extracted tree.** LPCNet / DRED / OSCE weights
  and inference code. Every reference to it from `silk/`, `celt/` and `src/`
  sits inside `#ifdef ENABLE_DEEP_PLC`, `#ifdef ENABLE_DRED` or
  `#ifdef ENABLE_OSCE`, none of which this build defines — they are all
  *decoder-side* concealment features, and a recorder never decodes.
- **`silk/fixed/`.** This is a float build (`config.h` note 1), so SILK
  compiles `silk/float/` and the fixed-point tree would be dead source.
- **`celt/x86/`, `silk/x86/`, `celt/arm/`, `silk/arm/`.** No runtime CPU
  dispatch (`config.h` note 2); libopus takes its portable C paths. This is the
  same trade `vendor/lame/config.h` makes by dropping the NASM kernels.
- The command-line programs: `src/opus_demo.c`, `src/opus_compare.c`,
  `src/repacketizer_demo.c`, `celt/opus_custom_demo.c`.
- `tests/`, `doc/`, `meson/`, `cmake/`, and the whole autoconf/automake
  machinery.

## The one file that is ours

`config.h`, which stands in for the header autoconf would generate and for the
one-line `cmake/config.h.cmake.in`. The four configuration choices it makes
are argued at the top of that file. `OPUS_BUILD` and `USE_ALLOCA` are on the
compiler command line in `CMakeLists.txt`, which is where upstream's own CMake
puts them.

No `.c` or `.h` file under `celt/`, `silk/`, `src/` or `include/` was patched.
That is the property worth keeping: everything here diffs clean against the
release tarball.
