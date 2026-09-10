# Cutting a release

This is the maintainer's checklist. If you are trying to *install* apprecorder,
you want the [README](../README.md); if you want to know what the updater does
to your machine, [updating.md](updating.md).

A release is four things uploaded to one GitHub release:

| Asset | What it is |
|---|---|
| `apprecorder.exe` | The build. |
| `release.json` | The signed manifest: version, asset name, SHA-256, notes. |
| `release.json.sig` | 64 raw bytes — the ECDSA P-256 signature over `release.json`. |
| `apprecorder-<version>-src.zip` | The source, which the LGPL requires. See below. |

## Before you start

**The signing key must be the one this build trusts.** It lives outside the
repository, at `%USERPROFILE%\.apprecorder\release-key.pem` by default, and its
public half is compiled into
[`src/platform/update_key.c`](../src/platform/update_key.c). If those two do
not match, every step below succeeds and the release is uninstallable by every
copy in the field. `verify` in step 4 is what catches that, so do not skip it.

**The key is not in CI and must not become a GitHub secret.** The reasoning is
in [SECURITY.md](../SECURITY.md). CI builds and tests; it does not sign and it
does not publish.

## 1. Set the version

Three integers in [`include/version.h`](../include/version.h), and nothing
else. The window title, `apprecorder version`, the manifest and the updater's
comparison all derive from them, so there is no second place to remember.

Add the release's section to [`CHANGELOG.md`](../CHANGELOG.md) at the same
time, while you still remember what changed.

## 2. Build and test

```
build.cmd Release test
```

Both must be clean. Read the skipped-cases list that `build.cmd` prints after
the run — a case that skipped for want of hardware did not test anything, and a
green run containing one proves less than it looks.

Check the size while you are here. The ceiling is 10 MB and CI enforces it, but
a sudden jump is worth understanding before it ships rather than after.

## 3. Sign

```
uv run tools/release/apprelease.py sign ^
  --exe build/Release/apprecorder.exe ^
  --version 0.0.1 ^
  --notes-file release-notes.txt ^
  --out dist
```

That hashes the executable, writes `release.json` with the hash inside it, and
signs the whole manifest. **The hash is inside the signed document on purpose**
— one signature then protects both the binary and the description of it.

## 4. Verify before uploading anything

```
copy build\Release\apprecorder.exe dist\
uv run tools/release/apprelease.py verify --dir dist
```

This checks the signature against the **public** key in `update_key.c` — the
bytes the shipped binary actually trusts — and then checks the executable
against the hash inside the signed manifest. It needs no secret, which is the
point: it is the same check anybody downloading the release can run, and it is
the only thing that catches a keypair mismatch before the field does.

Three lines and exit code 0, or do not upload.

## 5. Assemble the source zip

**This is a licence obligation, not a courtesy.** apprecorder links
libmp3lame statically, libmp3lame is LGPL-2.0-or-later, and **LGPL section 6
requires that a recipient of the binary be able to relink it against a modified
libmp3lame**. Publishing the source alongside the binary satisfies that. Full
reasoning in [licensing.md](licensing.md).

The zip must contain the tree the binary was built from — at minimum
`vendor/lame/` complete with its `COPYING` and `LICENSE`, and in practice the
whole repository at that tag, which is simpler to produce and simpler to
defend:

```
git archive --format=zip --prefix=apprecorder-0.0.1/ ^
  -o dist/apprecorder-0.0.1-src.zip v0.0.1
```

**This attaches the moment a compiled `apprecorder.exe` is uploaded anywhere** —
a GitHub release, a website, a zip sent to one person. It is not satisfied by
the repository merely existing somewhere; it travels with the binary.

## 6. Tag and publish

```
git tag -a v0.0.1 -m "apprecorder 0.0.1"
git push origin v0.0.1

gh release create v0.0.1 ^
  dist/apprecorder.exe ^
  dist/release.json ^
  dist/release.json.sig ^
  dist/apprecorder-0.0.1-src.zip ^
  --title "apprecorder 0.0.1" ^
  --notes-file release-notes.txt
```

**Upload order matters slightly.** An installed apprecorder that finds
`release.json` with no `release.json.sig` beside it refuses the release and
tries again later, which is correct but wastes a cycle. `gh release create`
with all four assets in one call avoids the window entirely; if you upload them
one at a time, put the signature or the exe last.

## 7. Check what the field will see

```
apprecorder update
```

from an older build, or against the published URL. The release is live for
every install the moment it becomes `latest`, so this is the first moment a
mistake stops being yours alone.

## The release notes

They are read by two audiences and neither of them is you.

`--notes` goes **inside the signed manifest** and is shown by the update prompt,
in a dialog, to somebody deciding whether to accept a download. Keep it to a
sentence or two. It is also read aloud, so no ASCII art, no bullet characters,
no version-control jargon.

The GitHub release body is the long form, and it can simply point at
[`CHANGELOG.md`](../CHANGELOG.md).

## What must be true of every release

- **It is signed, and the signature verifies against the key in
  `update_key.c`.** A release the field cannot verify is worse than no release.
- **The source zip is attached.** LGPL section 6.
- **`LICENSE` and `THIRD-PARTY-NOTICES.md` are in the source zip**, which they
  are automatically if it came from `git archive`.
- **The version in the manifest matches `include/version.h` in the tag.** The
  updater compares against what the running build says it is; a mismatch either
  offers an update that changes nothing or hides one that matters.
- **A CI artifact was not used as the binary.** CI builds unsigned artifacts
  for testing. They are not releases and apprecorder will not update to one.
