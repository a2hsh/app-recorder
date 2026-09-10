# Third-party notices

apprecorder itself is MIT — see [`LICENSE`](LICENSE). It statically links the
libraries below, which keep their own terms.

**A permissive licence on the program does not dissolve an obligation attached
to a library inside it.** One of these binds anyone who distributes a *binary*.

## libmp3lame — LGPL-2.0-or-later

`vendor/lame/` · [`COPYING`](vendor/lame/COPYING) ·
[`PROVENANCE.md`](vendor/lame/PROVENANCE.md)

Statically linked, so **LGPL section 6 applies to every binary release**: a
recipient must be able to relink apprecorder against a modified libmp3lame.

In practice a release must ship **either** the object files **or** this source
tree, together with `vendor/lame/COPYING` and a visible notice saying so. This
attaches to the release, not to apprecorder's licence — MIT is unaffected.

The MP3 *decoder* is deliberately excluded from the shipping image
(`HAVE_MPGLIB` without `DECODE_ON_THE_FLY`), so `hip_decode` exists only in the
test binary.

## libogg — BSD-3-Clause

`vendor/ogg/` · [`COPYING`](vendor/ogg/COPYING) ·
[`PROVENANCE.md`](vendor/ogg/PROVENANCE.md)

Reproduce the copyright notice and disclaimer. No further obligation.

## libopus — BSD-3-Clause

`vendor/opus/` · [`COPYING`](vendor/opus/COPYING) ·
[`PROVENANCE.md`](vendor/opus/PROVENANCE.md)

Reproduce the copyright notice and disclaimer. No further obligation.

Pinned at 1.5.2 rather than 1.6.x: the 1.6 line's headline features — deep
redundancy, packet-loss concealment, speech enhancement, neural bandwidth
extension — all address real-time speech over lossy networks. apprecorder
writes files, so there is no packet loss to conceal.

## jsmn — MIT

`vendor/jsmn/` · [`LICENSE`](vendor/jsmn/LICENSE) ·
[`PROVENANCE.md`](vendor/jsmn/PROVENANCE.md)

Reproduce the copyright notice and permission notice. No further obligation.

---

Full release checklist in [`docs/licensing.md`](docs/licensing.md).
