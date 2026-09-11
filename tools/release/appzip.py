# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""appzip.py -- build app-recorder-win-x64.zip, the thing people download.

    uv run tools/release/appzip.py --build build/Release --out dist

===============================================================================
WHY A SCRIPT AND NOT A COMMAND IN THE CHECKLIST

Because two of the files in it are load-bearing in a way that is invisible when
they are missing, and a hand-assembled zip is one distracted afternoon away
from omitting either.

  - apprecorder.com WITHOUT apprecorder.exe is a launcher with nothing to
    launch: every command prints one line and exits 7.
  - apprecorder.exe WITHOUT apprecorder.com is the bug shipped in 0.0.1 --
    `apprecorder version > out.txt` writes an empty file, and PowerShell does
    not wait for a recording to finish. Nothing announces this; it just
    silently stops being a command-line program.

So both are required, by name, and this refuses to produce a zip without them.

===============================================================================
WHAT ELSE GOES IN, AND WHY

The docs, because somebody who downloads a recorder should not have to go back
to a website to find out what a bus is -- and because the person this was
written for reads by screen reader, where "it's on the website" is a worse
answer than it sounds.

LICENSE and THIRD-PARTY-NOTICES.md, because THE SECOND ONE IS AN OBLIGATION.
libmp3lame is LGPL and a recipient of the binary has to be told so somewhere
they will actually see it. The About box carries the same sentence for someone
who never unzips the docs; this is the written copy that travels with the
files. See docs/licensing.md.

Deterministic by construction: entries are added in a fixed order with a fixed
timestamp, so building the same tree twice produces byte-identical zips and a
published checksum means something.
"""

import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Both, always. See the header.
REQUIRED_BINARIES = ("apprecorder.exe", "apprecorder.com")

# (source relative to the repo, name inside the zip)
DOCS = [
    ("README.md", "README.md"),
    ("CHANGELOG.md", "CHANGELOG.md"),
    ("LICENSE", "LICENSE"),
    ("THIRD-PARTY-NOTICES.md", "THIRD-PARTY-NOTICES.md"),
    ("docs/using-apprecorder.md", "docs/using-apprecorder.md"),
    ("docs/command-line.md", "docs/command-line.md"),
    ("docs/accessibility.md", "docs/accessibility.md"),
    ("docs/updating.md", "docs/updating.md"),
    ("docs/licensing.md", "docs/licensing.md"),
]

# A fixed timestamp, so two builds of one tree differ in no byte.
FIXED_DATE = (2026, 1, 1, 0, 0, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default="build/Release",
                    help="directory holding the built binaries")
    ap.add_argument("--out", default="dist",
                    help="directory to write the zip into")
    ap.add_argument("--name", default="app-recorder-win-x64.zip")
    args = ap.parse_args()

    build = Path(args.build)
    if not build.is_absolute():
        build = REPO / build
    out_dir = Path(args.out)
    if not out_dir.is_absolute():
        out_dir = REPO / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / args.name

    missing = [n for n in REQUIRED_BINARIES if not (build / n).is_file()]
    if missing:
        sys.exit(
            "%s is missing from %s.\n"
            "Both executables are required and neither is optional -- see the\n"
            "header of this script for what each one is doing there.\n"
            "Run `build.cmd Release` first."
            % (" and ".join(missing), build)
        )

    missing_docs = [src for src, _ in DOCS if not (REPO / src).is_file()]
    if missing_docs:
        sys.exit("missing from the repository: %s" % ", ".join(missing_docs))

    entries = []
    for name in REQUIRED_BINARIES:
        entries.append((build / name, name))
    for src, arc in DOCS:
        entries.append((REPO / src, arc))

    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path, arc in entries:
            info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, path.read_bytes())

    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    print("%s" % out)
    print("  %d entries, %d bytes" % (len(entries), out.stat().st_size))
    print("  sha256 %s" % digest)
    for _, arc in entries:
        print("    %s" % arc)


if __name__ == "__main__":
    main()
