#!/usr/bin/env python3
"""make_full_bundle.py -- inject the AI-LUT payload into a COMPLETE ML installer.

Unlike make_bundle.py (a minimal convenience overlay), this takes the full
Magic Lantern installer zip produced by the build (`build/magiclantern.zip` --
autoexec.bin, ML-SETUP.FIR, all modules, fonts, data, stock scripts, etc.) and
adds the AI-LUT payload so the result is a single, complete, flashable install:

    <all original ML installer files, unchanged>
  + ML/scripts/unified_logger.lua
  + ML/scripts/decision_engine.lua
  + ML/models/unified.tbl
  + ML/logs/.keep                     (so the log dir exists on a fresh card)

The AI files are added STORED (uncompressed); original entries keep their
original compression. After extraction to the card root, every file is plain --
the camera reads them directly, no on-camera decompression.

NOTE: requires a base magiclantern.zip, which only builds where ML-SETUP.FIR and
src/zip.txt are present (the working tree, not stock CI -- those files are not
vendored in this repo).

Usage:
    python tools/make_full_bundle.py \
        --base-zip source-dev/platform/6D.116/build/magiclantern.zip \
        --out dist/ai-magiclantern-6D.116-full.zip
"""

import argparse
import os
import sys
import zipfile

FIXED_DATE = (1980, 1, 1, 0, 0, 0)

# Single source, firmware-driven: on-camera ETTR/ISO/WB/ALO/HTP AND data logging
# are both handled by the AI-integrated firmware (ettr.mo reads
# ML/models/unified.tbl and appends ML/logs/unified_log.txt). So the card ships
# only the LUT; NO Lua scripts -- decision_engine.lua (parallel adjustment) and
# unified_logger.lua (ML Lua has no histogram, so it could never log) are both
# retired. See docs/ETTR_AI_INTEGRATION.md.
AI_FILES = [
    ("models/unified.tbl", "ML/models/unified.tbl"),
    ("models/lens_tune.tbl", "ML/models/lens_tune.tbl"),  # per-lens picture tune
]
LOGS_KEEP = "ML/logs/.keep"  # firmware writes unified_log.txt here


# Hard camera limits: the firmware reads each .tbl with a SINGLE 8 KB FIO read
# and caps the parsed LUT at 128 rows. An oversized table must never be bundled.
TBL_MAX_BYTES = 8192
TBL_MAX_ROWS = 128


def check_tbl(path, data):
    """Enforce the firmware caps on a .tbl payload. Returns an error string or None."""
    if len(data) >= TBL_MAX_BYTES:
        return f"{path}: {len(data)} bytes >= {TBL_MAX_BYTES} (single FIO read cap)"
    rows = [ln for ln in data.decode("utf-8", errors="replace").splitlines()
            if "|" in ln and not ln.lstrip().startswith("#")
            and not ln.lstrip().lower().startswith("scene|")]  # skip header
    if len(rows) > TBL_MAX_ROWS:
        return f"{path}: {len(rows)} data rows > {TBL_MAX_ROWS} (firmware row cap)"
    return None


def add_stored(zf, arcname, data):
    info = zipfile.ZipInfo(arcname, date_time=FIXED_DATE)
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    zf.writestr(info, data)


def main(argv):
    ap = argparse.ArgumentParser(description="Inject AI-LUT payload into a full ML installer zip.")
    ap.add_argument("--base-zip", required=True, help="path to the built magiclantern.zip")
    ap.add_argument("--out", required=True, help="output complete bundle zip path")
    ap.add_argument("--repo-root", default=".", help="repo root for the AI-LUT files (default .)")
    args = ap.parse_args(argv[1:])

    if not os.path.isfile(args.base_zip):
        print(f"ERROR: base installer zip not found: {args.base_zip}\n"
              "Build it first (make in the platform dir); it needs ML-SETUP.FIR + src/zip.txt.",
              file=sys.stderr)
        return 1

    ai_arcs = {arc for _, arc in AI_FILES} | {LOGS_KEEP}
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

    with zipfile.ZipFile(args.base_zip, "r") as base, \
            zipfile.ZipFile(args.out, "w") as out:
        copied = 0
        for info in base.infolist():
            # do not double-add if the base somehow already ships our filenames
            if info.filename in ai_arcs:
                continue
            out.writestr(info, base.read(info.filename), compress_type=info.compress_type)
            copied += 1
        for src_rel, arc in AI_FILES:
            src = os.path.join(args.repo_root, src_rel)
            if not os.path.isfile(src):
                print(f"ERROR: required AI-LUT file missing: {src}", file=sys.stderr)
                return 1
            with open(src, "rb") as fh:
                data = fh.read()
            if arc.endswith(".tbl"):
                err = check_tbl(src, data)
                if err:
                    print(f"ERROR: {err} -- refusing to bundle a table the camera "
                          "cannot load", file=sys.stderr)
                    return 1
            add_stored(out, arc, data)
        add_stored(out, LOGS_KEEP, b"")

    print(f"Wrote {args.out}: {copied} original ML files + {len(AI_FILES)} AI files + logs dir")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
