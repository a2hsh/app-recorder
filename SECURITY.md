# Security policy

## Reporting a vulnerability

Report it privately, through GitHub's **Report a vulnerability** button on the
[Security tab](https://github.com/a2hsh/app-recorder/security/advisories/new).
That opens a private advisory only the maintainer can see.

**Please do not open a public issue for a security problem.** apprecorder
updates itself from a signed release, so a flaw in that path is worth
disclosing to users and maintainer at the same moment rather than to the
internet first.

Say what you found, how to reproduce it, and what it lets an attacker do.
A proof of concept helps and is not required.

There is no bounty. This is a personal project.

## Supported versions

Only the newest release is supported. While the major version is `0`, a fix
ships as a new patch release rather than as a backport.

## What is in scope

The parts of apprecorder where a bug has a security consequence rather than
only a correctness one:

- **The updater.** Anything that would cause apprecorder to accept a manifest
  or an executable that the author's key did not sign, or to run one it did not
  verify. This is the highest-value target in the program and the one to look
  at first.
- **Session file parsing.** A `.json` session is untrusted input the moment
  somebody sends you one, and it is parsed by vendored C.
- **The system-minus-tree capture mode**, which records everything the machine
  plays. Anything that lets it be switched on without an explicit human yes —
  particularly from a session file — is a privacy vulnerability and is treated
  as one.
- **Memory safety anywhere reachable from untrusted input**: session files, the
  update manifest, file paths, and audio format descriptors coming back from
  Windows.

## What is not in scope

- **An attacker who is already running code as your user.** apprecorder is a
  user-level application with no service, no driver and no elevated component.
  Somebody at that level can replace the executable directly and does not need
  a bug in it.
- **That apprecorder can record audio.** That is the program.
- **A missing signature check on an unsigned local build.** A build compiled
  without a real public key has updates disabled entirely — it does not check,
  and it does not install.

## How releases are signed

This is the part of the design most worth knowing about if you are auditing it.

- Every release carries `release.json`, signed with **ECDSA P-256**, and
  `release.json.sig` beside it.
- The **public** key is compiled into the binary
  ([`src/platform/update_key.c`](src/platform/update_key.c)). Signature
  verification uses the in-box Windows BCrypt implementation, so there is no
  vendored cryptography.
- **The signed manifest carries the SHA-256 of the executable.** One signature
  protects the description and the payload together. A signature over the
  executable alone would leave the manifest unprotected.
- **A missing signature is treated exactly like a wrong one.** It is not a
  pass. See [docs/updating.md](docs/updating.md).
- **TLS certificate validation is never relaxed.** No flag disables it, and
  none is to be added.

### The private key is not in CI, and that is deliberate

The signing key lives on the author's machine and is never in this repository.
It is also **not a GitHub Actions secret**, and it will not become one.

The whole point of signing is that compromising the hosting account is not
enough to publish a binary that installs itself on other people's machines. A
key held by CI hands that capability straight back to the account, and the
signature stops meaning anything beyond "GitHub said so".

So [the CI workflow](.github/workflows/ci.yml) builds and tests, and uploads an
artifact that is explicitly marked **unsigned**. It cannot sign and it cannot
publish a release. Signing is a manual step the author performs locally, and
that is a feature.

**A CI artifact is not a release.** It is unsigned, apprecorder will not update
to it, and it should not be treated as one.

## Verifying a release yourself

Every release is verifiable with nothing but the public key already in the
repository — you do not have to trust GitHub, and you do not have to trust a
download.

```
uv run tools/release/apprelease.py verify --dir <folder with the release files>
```

That checks the same two things the product checks, in the same order: that the
manifest's signature verifies against the public key, and that the executable's
SHA-256 matches the hash inside the signed manifest.
