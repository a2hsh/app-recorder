# Updating

apprecorder can notice that a newer version has been published, download it,
prove it came from the person who publishes apprecorder, and put it in place the
next time it starts.

This page is what it does, what it sends, how to turn it off, and why it is
built the way it is. The last part matters more than usual: an updater is a
program that downloads code and runs it, so it is worth being able to see
exactly what it will and will not do.

## The short version

- It checks a public GitHub release page. It does not use the GitHub API.
- It checks **at startup, every five minutes, and after a recording stops** —
  and the last of those only if five minutes have already passed.
- **Nothing installs itself.** It asks. If you say yes, it downloads and
  verifies, and the swap happens at the next start.
- **It never touches anything during a recording**, not even a paused one.
- `apprecorder update --disable` stops it, permanently, and that setting is
  remembered.

## Checking

### From the window

**Help → Check for Updates.** Somebody who asks is owed an answer, so a manual
check ignores the five-minute interval and goes to the network immediately. It
still respects the opt-out: if you have turned updates off, it says so rather
than quietly making the request you refused.

The result is announced. While the window is in front that is the status line;
while it is not, it is a notification-area balloon, for the reason described in
[accessibility.md](accessibility.md) — a live region on an unfocused background
window is not reliably announced by any screen reader.

### From the command line

```
apprecorder update
```

Prints one of: this is the newest version, a newer one is available, or which
version is installed when the check could not be completed.

| Option | Meaning |
|---|---|
| `--install` | Download the newer version and put it in place at the next start. |
| `--enable` | Check for newer versions from now on. Changes the setting and checks nothing. |
| `--disable` | Stop checking for newer versions. Changes the setting and checks nothing. |

`--enable` and `--disable` cannot both be given. apprecorder remembers this
setting, so accepting both would mean remembering the wrong one.

**Neither toggle touches the network.** `--disable` obviously must not make one
last callback on its way out — that is the exact request you just refused — and
if `--enable` made one, then the two commands would differ in whether they
reach the network, which is not something anybody would remember correctly.
Somebody who wants both can ask for both.

### When it happens on its own

The cadence the author asked for was: at startup, every five minutes, and after
a recording stops.

The third one needs a guard. Starting and stopping while you set levels is
completely normal, and "check after every stop" turns ten takes into ten
requests. **So a stop only checks if five minutes have elapsed since the last
check.**

The last-check time is written to disk rather than kept in memory, and that is
what makes startup obey the same gate as everything else. A process that
starts, checks, crashes and restarts is a request storm if the timestamp lives
only in memory — and the machine doing that is by definition one whose owner is
not watching it.

Only an explicit "check now" from you bypasses the interval.

## What goes over the network

A check is an HTTPS GET of two small files from GitHub's stable download
redirect:

```
https://github.com/a2hsh/app-recorder/releases/latest/download/release.json
https://github.com/a2hsh/app-recorder/releases/latest/download/release.json.sig
```

The ETag from the last successful fetch is sent back as `If-None-Match`, so an
unchanged release answers `304 Not Modified` with no body. That is a few
hundred bytes on the wire and no work on either end, and it is what makes a
five-minute interval defensible rather than rude.

**Certificate validation is never relaxed.** There is no flag in apprecorder
that turns it off and none is to be added. The signature does not make TLS
redundant: the signature proves the file is genuine, and TLS is what stops an
observer on the network learning which builds this machine is running.

**It is not the GitHub API on purpose.** `api.github.com` allows 60 requests an
hour per IP address for unauthenticated callers, and a household, an office or
a campus behind one NAT shares that budget with every other tool on it. The
failure mode would be "updates stopped working for everybody in the building,
intermittently", which is worse than not having an updater.

### What a check tells the server

Exactly three things, and it is worth being precise rather than reassuring:

- **Your IP address and roughly when you asked**, which is true of any HTTPS
  request to anywhere.
- **That this machine is running apprecorder**, because that is what this URL
  is for.
- **Which version.** The request carries a User-Agent of
  `apprecorder/<version>` — so a check from this build says `apprecorder/0.0.1`.

It sends nothing else. **No machine name, no user name, no install id, no
serial number, no count of how many times you have run it, and no information
about what you record** — not the applications you capture, not the devices,
not the file names, not how long anything ran. There is no telemetry endpoint,
no crash reporter and no analytics in this program at all; the release URL is
the only address it ever contacts.

The ETag it sends back as `If-None-Match` is the server's own tag for the
current release, which is the same value for everybody on that release. It is
not an identifier for you.

The comparison between what is published and what you are running happens on
your machine, after the answer arrives — the server is not asked what to do.

That is still a callback, and whether to make it is your decision rather than
apprecorder's, which is why the setting exists and why it persists.

## Proving a download is genuine

**The host is treated as untrusted.** GitHub replaces infrastructure, not
trust: it makes hosting somebody else's problem, and it makes the account the
one thing standing between a stranger and a binary that every install
downloads and runs. A stolen token, a phished password or a compromised CI job
publishes a release, and an updater that trusts its host runs whatever was
published.

So the only thing trusted is the author's signing key.

- `release.json` describes the release and is signed with **ECDSA P-256**. The
  **public** key is compiled into apprecorder. The private key is held by the
  author, is not in the repository, and is deliberately not a CI secret —
  see [SECURITY.md](../SECURITY.md).
- **The manifest carries the SHA-256 of the executable.** One signature
  protects both, which is why the hash is inside the signed document rather
  than sitting beside it. A signature over the executable alone would leave the
  description — which version, which asset, what it claims to be — unprotected.
- The downloaded executable is hashed and compared against the signed manifest
  **before anything moves**.

**A missing signature is not a pass.** Publishing a release is several uploads,
and between the first and the last there is a window in which `release.json`
exists and `release.json.sig` does not. An updater that reads "no signature,
must be fine" is one badly-timed upload away from installing anything. Here it
is exactly as fatal as a wrong signature, with one difference: it is not
announced, because it is the expected shape of a release still being uploaded
rather than a sign that anything is wrong. The check produces nothing, the ETag
is not remembered, and the next tick tries again.

### When verification fails

A signature that does not verify, or a payload whose hash does not match the
signed manifest, is **announced, logged as an error, and the downloaded file is
deleted**. The message says the download was refused because it is not signed
by the person who publishes apprecorder, and that nothing was installed.

This is deliberately not the same sentence as an ordinary download failure. One
of them means the network is unreliable; the other means somebody is trying
something. Folding them into a single "update failed" would hide the second
behind the first.

An update that simply could not be checked — offline, DNS down, a captive
portal, a proxy that eats it — is **not an error worth telling you about**. It
goes in the log at INFO and nothing else happens.

## Installing

When a newer version exists, apprecorder tells you and asks. It does not
install by itself.

> apprecorder 0.0.2 is available and this is 0.0.1. Nothing is replaced now:
> the new version is downloaded, checked, and put in place the next time
> apprecorder starts. The version you have is kept until the new one has
> started once.

If you accept, the new executable is downloaded beside the current one as
`apprecorder.exe.new` and verified there. Then, at exit:

1. `apprecorder.exe` is renamed to `apprecorder.exe.old`
2. `apprecorder.exe.new` is renamed to `apprecorder.exe`

Windows forbids writing to or deleting a running image, but it permits
**renaming** one, which is what makes this possible with no second process:
there is no `updater.exe`, no scheduled task, no service, and nothing left
behind afterwards to become its own attack surface.

**The old version is not deleted at swap time.** It is kept until the new build
has started successfully once, so a version that does not start is one rename
away from recovery rather than a reinstall. If you ever need to do that by
hand, delete `apprecorder.exe` and rename `apprecorder.exe.old` back.

### Never during a recording

Asked to apply an update while a recording is running, apprecorder refuses and
says so:

> apprecorder will not replace itself while a recording is running. Stop the
> recording first; the newer version is kept and will be installed then.

It does not stop the recording, and it does not pause it, to make room for
itself. A take is unrepeatable and an update is a convenience. A paused
recording counts as a recording for this purpose — the files are still open.

## Turning it off

```
apprecorder update --disable
```

The setting is remembered, so this is permanent until you reverse it with
`--enable`. With updates off, the periodic check does not run, the startup
check does not run, and a manual check says updates are off rather than making
the request anyway.

This is treated as your decision and not a preference apprecorder gets to
override, because a check is a network callback that tells a server this
machine is running apprecorder and how often.

## Where the state lives

apprecorder installs nothing and has no configuration file. The only thing it
writes outside its own folder is the updater's state:

```
HKCU\Software\apprecorder\Update
```

| Value | Type | What it is |
|---|---|---|
| `Enabled` | REG_DWORD | Whether to check at all. |
| `LastCheck` | REG_QWORD | When the last check happened, so the interval survives a restart. |
| `ETag` | REG_SZ | The last release's ETag, so an unchanged release answers 304. |
| `Pending` | REG_SZ | A downloaded version awaiting its first successful start. |

Deleting that key resets the updater to its defaults and loses nothing else.
Session files are ordinary files wherever you saved them, and recordings are
wherever you told them to go.

## What this does not do

- **It does not install itself silently.** For a release going to friends, a
  binary that swaps itself unasked is worse than one that prompts.
- **It does not update in the background while you work.** The download happens
  when you accept it; the swap happens at the next start.
- **It does not roll back automatically.** It keeps the previous version so
  that you can.
- **It does not check anything but the one release URL above.** There is no
  telemetry endpoint, no crash reporter and no analytics of any kind.
