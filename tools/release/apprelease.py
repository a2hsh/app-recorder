# /// script
# requires-python = ">=3.9"
# dependencies = ["cryptography>=42"]
# ///
"""apprelease.py -- the release side of apprecorder's updater.

Run it with uv, which fetches the one dependency into a throwaway environment:

    uv run tools/release/apprelease.py keygen
    uv run tools/release/apprelease.py sign --exe build/Release/apprecorder.exe \
        --version 0.0.2 --notes "What changed."
    uv run tools/release/apprelease.py verify --dir dist

===============================================================================
WHAT THIS PRODUCES, AND WHY IT IS SHAPED THIS WAY

A release is three assets, uploaded to one GitHub release:

    release.json        the manifest: version, asset name, SHA-256, notes
    release.json.sig    64 raw bytes: an ECDSA P-256 signature, r||s
    apprecorder.exe     the build itself

The installed program fetches the first two through the stable redirect

    https://github.com/<owner>/<repo>/releases/latest/download/<asset>

verifies the signature against a public key compiled into itself, and only then
looks at what the manifest says. The exe's hash is INSIDE the signed manifest,
so one signature protects both: a swapped binary fails the hash, and a rewritten
manifest fails the signature. See include/update.h for the whole threat model.

===============================================================================
THE SIGNATURE FORMAT IS RAW r||s, NOT DER

Windows' BCrypt speaks raw fixed-width r||s natively. Emitting DER would put an
ASN.1 decoder inside the updater, which is a parser running on bytes an attacker
chose, on the path that decides whether to replace the executable. There is no
such parser, and this script exists partly to make sure there never needs to be:
the conversion happens HERE, where a bug is a failed release rather than a
security hole.

===============================================================================
THE PRIVATE KEY NEVER ENTERS THE REPOSITORY

`keygen` writes it outside the tree (by default under your user profile),
refuses to overwrite an existing one, and prints only the PUBLIC half -- already
formatted as the C array to paste into src/platform/update_key.c.

If that key is ever lost, no future release can update an existing install:
every copy in the field trusts that key and nothing else. Back it up somewhere
you would back up an SSH key.

If it is ever STOLEN, generate a new one, put the new public half in
update_key.c, and publish that build signed WITH THE OLD KEY -- that is the last
release the old key can make, and it is what carries everyone onto the new one.
Then stop using the old key.

===============================================================================
`verify` NEEDS NO SECRET, ON PURPOSE

It checks the signature against the PUBLIC key in src/platform/update_key.c --
the same bytes the shipped binary trusts -- and not against the signing key.

That is what makes it useful twice. Anyone can run it on a downloaded release
without holding anything private, which is the property a signed release is
supposed to have. And on the author's own machine it catches the one release
mistake that is otherwise invisible until it is too late: signing with a key
whose public half is NOT the one compiled into the exe being shipped. Verified
against the private key, that release passes every check here and then fails on
every machine in the field, silently, for weeks.

Run it before uploading. It is the last thing standing between a bad keypair
and a release nobody can install.
"""

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils as asym_utils

DEFAULT_KEY = Path.home() / ".apprecorder" / "release-key.pem"
DEFAULT_ASSET = "apprecorder.exe"


# ---------------------------------------------------------------------------
# keygen
# ---------------------------------------------------------------------------

def cmd_keygen(args):
    out = Path(args.out).expanduser()
    if out.exists():
        # NEVER overwrite. Overwriting a signing key silently orphans every
        # install in the field, and it is the kind of mistake that is only
        # noticed months later when an update stops working.
        sys.exit(
            "%s already exists.\n"
            "Refusing to overwrite a signing key. Move it aside deliberately\n"
            "if you really mean to replace it -- every apprecorder already\n"
            "installed trusts the public half of that key and nothing else."
            % out
        )

    out.parent.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )
    # 0o600 before the bytes go in, so there is no window in which it is
    # world-readable. On Windows this is close to a no-op, which is why the
    # location -- your profile, not the repository -- is the real protection.
    fd = os.open(str(out), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)

    print("private key written to %s" % out)
    print("Back it up. Do not put it in the repository.\n")
    print("Paste this into src/platform/update_key.c, over the zeros:\n")
    print(public_key_c_array(key.public_key()))


def public_key_c_array(pub):
    nums = pub.public_numbers()
    raw = nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")
    lines = []
    for half, label in ((raw[:32], "X"), (raw[32:], "Y")):
        lines.append("    /* %s */" % label)
        for i in range(0, 32, 8):
            row = ", ".join("0x%02X" % b for b in half[i:i + 8])
            lines.append("    %s," % row)
    body = "\n".join(lines).rstrip(",")
    return (
        "static const uint8_t g_public_key[APR_UPDATE_PUBKEY_BYTES] = {\n"
        + body
        + "\n};"
    )


# ---------------------------------------------------------------------------
# sign
# ---------------------------------------------------------------------------

def load_key(path):
    p = Path(path).expanduser()
    if not p.exists():
        sys.exit("no signing key at %s -- run `keygen` first" % p)
    return serialization.load_pem_private_key(p.read_bytes(), password=None)


# ---------------------------------------------------------------------------
# The PUBLIC key, read out of the source file the product compiles in.
#
# VERIFY MUST NOT USE THE PRIVATE KEY. Checking a signature against the very
# key that just produced it is nearly a tautology: it passes whatever `sign`
# emitted, and it cannot see the one mistake that actually ships a broken
# release -- a keypair that does not match the bytes in update_key.c. That
# build trusts a different key, refuses the manifest, and every install in the
# field stops updating, which is discovered weeks later by somebody who cannot
# update to the fix.
#
# So the trust anchor here is the same one the binary uses: the array in
# src/platform/update_key.c. That also makes verification something ANYONE can
# do -- the public key is in the repository, the release is on GitHub, and no
# secret is involved -- which is the property a signed release is supposed to
# have and did not.
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PUBKEY_C = REPO_ROOT / "src" / "platform" / "update_key.c"


def load_pubkey_c(path):
    """Read g_public_key[] out of update_key.c as an EC public key."""
    p = Path(path).expanduser()
    if not p.exists():
        sys.exit("no public key source at %s" % p)

    text = p.read_text(encoding="utf-8", errors="replace")
    start = text.find("g_public_key")
    if start < 0:
        sys.exit("%s does not define g_public_key" % p)
    start = text.find("{", start)
    end = text.find("}", start)
    if start < 0 or end < 0:
        sys.exit("%s: could not find the g_public_key initializer" % p)

    raw = bytes(
        int(tok, 16)
        for tok in re.findall(r"0[xX][0-9a-fA-F]{1,2}", text[start:end])
    )
    if len(raw) != 64:
        sys.exit(
            "%s: g_public_key is %d bytes, expected 64" % (p, len(raw))
        )
    if not any(raw):
        # The same meaningful zero as apr_update_have_key(). A build with this
        # key does not check for updates at all, so a release signed against it
        # could never be installed by anything.
        sys.exit(
            "%s is still all zeros -- this build has no trusted key and its\n"
            "updater does not look. Run `keygen` and paste the printed array\n"
            "in before signing a release." % p
        )

    x = int.from_bytes(raw[:32], "big")
    y = int.from_bytes(raw[32:], "big")
    return ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()


def raw_signature(key, data):
    """ECDSA P-256 over `data`, as the 64 raw bytes r||s that BCrypt wants."""
    der = key.sign(data, ec.ECDSA(hashes.SHA256()))
    r, s = asym_utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def cmd_sign(args):
    exe = Path(args.exe).expanduser()
    if not exe.is_file():
        sys.exit("no such file: %s" % exe)

    notes = args.notes or ""
    if args.notes_file:
        notes = Path(args.notes_file).expanduser().read_text(encoding="utf-8")
    notes = " ".join(notes.split())          # one line; the UI reads it aloud

    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    manifest = {
        "version": args.version,
        "asset": args.asset,
        "sha256": digest,
        "size": exe.stat().st_size,
        "notes": notes,
    }

    # SIGNED OVER EXACTLY THE BYTES THAT ARE PUBLISHED. The updater verifies
    # the response body as received -- no re-serialization, no normalization
    # (update.c note 1) -- so these bytes are built once and both written and
    # signed. ensure_ascii keeps the file byte-identical however it is later
    # opened and re-saved by a text editor that means well.
    blob = json.dumps(manifest, indent=2, ensure_ascii=True).encode("utf-8")

    key = load_key(args.key)
    sig = raw_signature(key, blob)
    assert len(sig) == 64

    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    (out / "release.json").write_bytes(blob)
    (out / "release.json.sig").write_bytes(sig)

    print("release.json      %s" % (out / "release.json"))
    print("release.json.sig  %s (64 bytes)" % (out / "release.json.sig"))
    print("sha256(%s) = %s" % (exe.name, digest))
    print()
    print("Upload all three to the GitHub release, and UPLOAD THE SIGNATURE")
    print("LAST or the exe last -- an installed apprecorder that finds a")
    print("manifest with no signature refuses the release and tries again,")
    print("which is correct but wastes everyone's time. The three assets are:")
    print("  release.json, release.json.sig, %s" % args.asset)


# ---------------------------------------------------------------------------
# verify -- the same checks the product makes, before anyone downloads it
# ---------------------------------------------------------------------------

def cmd_verify(args):
    d = Path(args.dir).expanduser()
    blob = (d / "release.json").read_bytes()
    sig = (d / "release.json.sig").read_bytes()

    if len(sig) != 64:
        sys.exit("release.json.sig is %d bytes, expected 64" % len(sig))

    pub = load_pubkey_c(args.pubkey)
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    try:
        pub.verify(asym_utils.encode_dss_signature(r, s), blob,
                   ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        # SAY WHAT THIS MEANS. A bare InvalidSignature traceback is the one
        # outcome that must not look like the tool broke: this is the sentence
        # somebody sees when a release was tampered with, and it is printed to
        # people who did not write this script.
        sys.exit(
            "SIGNATURE DOES NOT VERIFY.\n"
            "\n"
            "release.json in %s was not signed by the key in %s.\n"
            "It has been altered since it was signed, or it was signed by\n"
            "somebody else. Do not install this release." % (d, args.pubkey)
        )
    print("signature verifies against %s" % args.pubkey)

    manifest = json.loads(blob)
    exe = d / manifest["asset"]
    if exe.is_file():
        got = hashlib.sha256(exe.read_bytes()).hexdigest()
        if got != manifest["sha256"]:
            sys.exit(
                "HASH MISMATCH.\n"
                "\n"
                "%s is not the file the signed manifest describes.\n"
                "  signed:   %s\n"
                "  this file: %s\n"
                "The manifest is genuine, so the executable beside it has been\n"
                "swapped. Do not install this release."
                % (exe, manifest["sha256"], got)
            )
        print("%s matches the signed hash" % exe.name)
    else:
        print("(%s is not in this directory; hash not checked)" % manifest["asset"])
    print("version %s" % manifest["version"])


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("keygen", help="create the release signing keypair")
    g.add_argument("--out", default=str(DEFAULT_KEY))
    g.set_defaults(func=cmd_keygen)

    s = sub.add_parser("sign", help="hash an exe and write a signed release.json")
    s.add_argument("--exe", required=True)
    s.add_argument("--version", required=True)
    s.add_argument("--asset", default=DEFAULT_ASSET)
    s.add_argument("--notes", default="")
    s.add_argument("--notes-file", default="")
    s.add_argument("--key", default=str(DEFAULT_KEY))
    s.add_argument("--out", default="dist")
    s.set_defaults(func=cmd_sign)

    v = sub.add_parser("verify", help="check a release directory the way the product will")
    v.add_argument("--dir", default="dist")
    # The PUBLIC key the product compiles in -- not the signing key. See
    # load_pubkey_c(). No secret is needed to verify a release, and requiring
    # one would mean only the author could.
    v.add_argument("--pubkey", default=str(DEFAULT_PUBKEY_C))
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
