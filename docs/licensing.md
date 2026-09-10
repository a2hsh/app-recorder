# Licensing

Two separate questions live on this page. The first is open. The second is not, and it binds a binary release the moment one is uploaded.

## apprecorder's own licence — MIT

apprecorder is released under the **MIT License**. The full text is in
[`LICENSE`](../LICENSE) at the repository root.

MIT asks for one thing: keep the copyright notice and the permission notice with
the software. Beyond that you may use, modify, redistribute and sell it, in open
or closed products.

**That covers apprecorder's own code, and nothing else.** The vendored libraries
keep their own terms, and one of them binds anyone who distributes a *binary* —
see below. A permissive licence on the program does not dissolve an obligation
attached to a library inside it.

Three things are worth knowing about why the choice was free.

**The MP3 dependency does not force your hand.** LAME is LGPL-2.0-or-later, and the LGPL is specifically designed so that a library can be linked into a program under a different licence, including a proprietary one. It attaches an obligation to the *release* (see below), not a licence to the program.

**Nothing else in the tree constrains it either.** libogg and libopus are BSD-3-Clause and jsmn is MIT. All three are permissive: they ask for their notices to be reproduced and nothing more.

**So the choice is genuinely free.** The common candidates each mean something different here:

- **MIT or BSD-2-Clause** — anyone may do anything, including shipping a closed derivative. Simplest for adoption.
- **Apache-2.0** — the same, plus an explicit patent grant and a requirement to state changes. Usually the better permissive default for a project that touches codecs.
- **GPL-3.0** — derivatives must stay open. Note that this would *also* satisfy the LAME obligation below almost incidentally, because a GPL release already ships its source.
- **MPL-2.0** — file-level copyleft. A middle position: this project's files stay open, code linked alongside them does not have to.

Whichever it is, the LAME obligation in the next section still applies to a compiled binary.

## Third-party components

| Component | Version | Licence | Obligation on a binary release |
|---|---|---|---|
| LAME (libmp3lame) | 3.100 | **LGPL-2.0-or-later** | **Relinking must be possible.** See below. |
| libopus | 1.5.2 | BSD-3-Clause | Reproduce the copyright notice and the three-clause text in the documentation. |
| libogg | 1.3.6 | BSD-3-Clause | Same. |
| jsmn | v1.1.0 line | MIT | Reproduce the copyright notice and the permission text. |

Full provenance for each — exact release, checksums, the independent channels those checksums were verified against, and what was and was not taken from upstream — is in the `PROVENANCE.md` beside each vendored tree.

## The LAME obligation, in full

apprecorder links **libmp3lame statically**. The LGPL permits that, and conditions it.

**LGPL-2.0 section 6 requires that a recipient of the binary be able to relink the application against a modified libmp3lame.** Satisfying that means publishing, *alongside every binary release*, one of the following:

1. **The `vendor/lame/` source tree** as it was built, or
2. **The object files** of apprecorder's own code, sufficient for someone to relink them against their own build of libmp3lame.

Option 1 is what this project should do, because the source tree is already in the repository and diffs clean against the upstream release. It costs nothing to include a link or a tarball.

In addition, the release must carry:

- **The full LGPL text** — `vendor/lame/COPYING`, unmodified.
- **LAME's own notice** — `vendor/lame/LICENSE`, unmodified.
- **A statement that apprecorder uses libmp3lame and that libmp3lame is LGPL**, visible to a recipient of the binary. The README and an About box both count; a line buried in a source file does not.

**This is a release-engineering obligation, not a build one.** It has no effect during development. It attaches the moment a compiled `apprecorder.exe` is uploaded anywhere — a GitHub release, a website, a zip sent to one person.

Two footnotes. The MPEG audio patents referenced in LAME's own notice expired in 2017. And LAME 4.0 exists; 3.100 was kept deliberately, for the reasons in `vendor/lame/PROVENANCE.md`. Moving to 4.0 would not change any of the above.

## What the permissive ones need

libogg, libopus and jsmn need their copyright notice and licence text reproduced in the documentation accompanying the distribution. That is the whole obligation: no source, no object files, no relinking, no special packaging.

libopus additionally carries Xiph's patent statement, covering royalty-free grants from Xiph, Microsoft, Broadcom and Google for the RFC 6716 reference implementation. It is part of `vendor/opus/COPYING` and travels with it.

## Release checklist

Before publishing a compiled `apprecorder.exe`:

- [ ] apprecorder's own licence is chosen, and a `LICENSE` file exists at the repository root.
- [ ] The release includes `vendor/lame/` in source form, or apprecorder's object files.
- [ ] The release includes `vendor/lame/COPYING` and `vendor/lame/LICENSE`.
- [ ] The release includes `vendor/opus/COPYING`, `vendor/ogg/COPYING` and `vendor/jsmn/LICENSE`.
- [ ] A notice naming libmp3lame, libopus, libogg and jsmn, with their licences, is somewhere a recipient of the binary will actually see it.

The first item is a decision. The rest are file copies.
