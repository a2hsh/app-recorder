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
"""

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

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

    key = load_key(args.key)
    pub = key.public_key()
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    pub.verify(asym_utils.encode_dss_signature(r, s), blob,
               ec.ECDSA(hashes.SHA256()))
    print("signature verifies")

    manifest = json.loads(blob)
    exe = d / manifest["asset"]
    if exe.is_file():
        got = hashlib.sha256(exe.read_bytes()).hexdigest()
        if got != manifest["sha256"]:
            sys.exit("HASH MISMATCH: %s is not what the manifest describes" % exe)
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
    v.add_argument("--key", default=str(DEFAULT_KEY))
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
