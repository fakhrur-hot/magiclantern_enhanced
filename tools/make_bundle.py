#!/usr/bin/env python3
"""make_bundle.py -- DEPRECATED minimal "AI Magic Lantern" overlay zip.

Superseded by tools/make_full_bundle.py, which produces the single, complete
flashable installer (full ML tree + ML-SETUP.FIR + AI-LUT payload). CI no longer
uses this. Kept only for reference / cards where a full installer is unavailable.

--- original docstring ---
assemble a convenience "AI Magic Lantern" bundle zip.

Packages the built firmware together with the AI-LUT payload (the two Lua
scripts + unified.tbl) laid out for a Magic Lantern SD card:

    autoexec.bin                        (card root -- runtime firmware)
    magiclantern.bin                    (card root -- core, for reference)
    ML/scripts/unified_logger.lua
    ML/scripts/decision_engine.lua
    ML/models/unified.tbl
    README.txt

IMPORTANT: entries are STORED (uncompressed, ZIP_STORED). The zip is only a
download container -- the tester extracts it onto the card, and the camera reads
plain, uncompressed files directly via io.open. Nothing is decompressed on the
DIGIC 5+ ARM core, so there is no SD/CPU decompression overhead on camera.

This is a convenience overlay, NOT a from-scratch flashable installer: it does
not include ML-SETUP.FIR or the full ML runtime tree (not vendored in this repo).
Use it to drop the AI-LUT files onto a card that already runs Magic Lantern.

Usage:
    python tools/make_bundle.py --camera 6D.116 \
        --build-dir source-dev/platform/6D.116/build \
        --out ai-magiclantern-6D.116.zip
"""

import argparse
import os
import sys
import zipfile

# Fixed timestamp for reproducible (byte-identical) zips across runs.
FIXED_DATE = (1980, 1, 1, 0, 0, 0)

# Single source of adjustment: the AI-integrated firmware (ettr.mo) drives
# ETTR/ISO/WB/ALO/HTP from ML/models/unified.tbl. Ship the logger (data
# collection) + the LUT, but NOT decision_engine.lua (superseded parallel
# adjustment source). See docs/ETTR_AI_INTEGRATION.md.
REPO_FILES = [
    ("models/unified.tbl", "ML/models/unified.tbl"),
]
FIRMWARE_FILES = ["autoexec.bin", "magiclantern.bin"]

# The scripts append to A:/ML/logs/unified_log.txt with io.open(..., "a"), which
# creates the file but NOT a missing directory. Ship a placeholder so the logs/
# folder exists on a fresh card and logging never silently fails.
LOGS_KEEP = "ML/logs/.keep"


def readme(camera, included_firmware):
    fw = ", ".join(included_firmware) if included_firmware else "(none -- build failed)"
    return (
        "AI Magic Lantern -- convenience bundle for " + camera + "\n"
        "======================================================\n\n"
        "Contents (all stored UNCOMPRESSED):\n"
        "  autoexec.bin / magiclantern.bin : firmware for this camera\n"
        "  ML/scripts/unified_logger.lua   : on-camera study/logging script\n"
        "  ML/models/unified.tbl           : the AI-trained lookup table\n"
        "  ML/logs/                        : where unified_log.txt is written\n\n"
        "Firmware included in this bundle: " + fw + "\n\n"
        "Adjustment is single-source: the AI-integrated firmware (ETTR module)\n"
        "reads ML/models/unified.tbl and drives ML's own ETTR/ISO/WB/ALO/HTP.\n"
        "unified_logger.lua ONLY logs sensor data (no settings changes) to\n"
        "ML/logs/unified_log.txt on each half-press, for offline training.\n\n"
        "HOW TO USE\n"
        "  This is NOT a complete from-scratch installer (no ML-SETUP.FIR or\n"
        "  full ML tree). If your card already runs Magic Lantern, copy the ML/\n"
        "  folder from this zip onto the card root, merging with the existing\n"
        "  ML/ folder. The files are plain text/binary -- the camera reads them\n"
        "  directly from the SD card with no decompression overhead.\n\n"
        "  Retrained a new unified.tbl? Just replace ML/models/unified.tbl on the\n"
        "  card; it hot-swaps on the next half-press (no reflash).\n"
    )


def add_stored(zf, arcname, data):
    """Add bytes as an uncompressed (ZIP_STORED) entry with a fixed timestamp."""
    info = zipfile.ZipInfo(arcname, date_time=FIXED_DATE)
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    zf.writestr(info, data)


def main(argv):
    ap = argparse.ArgumentParser(description="Assemble an AI Magic Lantern bundle zip.")
    ap.add_argument("--camera", required=True, help="camera id, e.g. 6D.116")
    ap.add_argument("--build-dir", required=True, help="platform build/ dir with the firmware bins")
    ap.add_argument("--out", required=True, help="output zip path")
    ap.add_argument("--repo-root", default=".", help="repo root for the AI-LUT files (default .)")
    args = ap.parse_args(argv[1:])

    included_fw = []
    missing = []
    with zipfile.ZipFile(args.out, "w", compression=zipfile.ZIP_STORED) as zf:
        for name in FIRMWARE_FILES:
            src = os.path.join(args.build_dir, name)
            if os.path.isfile(src):
                with open(src, "rb") as fh:
                    add_stored(zf, name, fh.read())
                included_fw.append(name)
            else:
                missing.append(name)
        for src_rel, arc in REPO_FILES:
            src = os.path.join(args.repo_root, src_rel)
            if not os.path.isfile(src):
                print(f"ERROR: required AI-LUT file missing: {src}", file=sys.stderr)
                return 1
            with open(src, "rb") as fh:
                add_stored(zf, arc, fh.read())
        add_stored(zf, LOGS_KEEP, b"")  # ensure A:/ML/logs/ exists on the card
        add_stored(zf, "README.txt", readme(args.camera, included_fw).encode("utf-8"))

    print(f"Wrote {args.out} (firmware: {included_fw or 'none'}; missing: {missing or 'none'})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
