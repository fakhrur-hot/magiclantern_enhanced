#!/usr/bin/env python3
"""
validate_sidecar.py — ML_6D sidecar & session file validator.

Validates an SD card's ML_6D output structure:
  - Every .CR2 has a matching .ml6d sidecar in ML/DATA/SHOTS/
  - Each .ml6d parses as valid JSON with required fields
  - ml_export.json exists and parses correctly

Usage:
    python validate_sidecar.py /path/to/sd_card_root
    python validate_sidecar.py /path/to/sd_card_root --cr2-dir DCIM/100CANON

Output: pass/fail summary suitable for docs/VALIDATION.md.
"""

import argparse
import glob
import json
import os
import sys


# ---------------------------------------------------------------------------
# Schema constants
# ---------------------------------------------------------------------------

REQUIRED_SIDECAR_FIELDS = [
    "formatVersion",
    "schema_version",
    "filename",
    "lens",
    "pictureStyle",
    "dualIsoPatch",
]

REQUIRED_LENS_FIELDS = ["id", "caStrength", "fringeReduce"]

REQUIRED_ETTR_FIELDS = ["lightLevel", "sceneDR", "highlightHeadroom", "channelClip"]

REQUIRED_DUALISO_FIELDS = ["enabled", "isoBase", "isoAlternate", "interleavePeriod"]

REQUIRED_SESSION_FIELDS = [
    "formatVersion",
    "schema_version",
    "firmwareVersion",
    "body",
    "sensor",
    "session",
]

REQUIRED_SENSOR_FIELDS = ["pixelPitch", "cropFactor"]

REQUIRED_SESSION_BLOCK_FIELDS = ["dualIsoEnabled", "ettrEnabled", "pictureStyle"]

VALID_DUAL_ISO_PATCH_VALUES = ("unsupported", "applied")

EXPECTED_SCHEMA_VERSION = "2.0"
EXPECTED_FORMAT_VERSION = 2


# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------

class ValidationResult:
    """Accumulates pass/fail results for a single file or check."""

    def __init__(self):
        self.errors = []
        self.warnings = []

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)

    @property
    def passed(self):
        return len(self.errors) == 0


def validate_sidecar(filepath):
    """Validate a single .ml6d sidecar file. Returns a ValidationResult."""
    result = ValidationResult()

    # Parse JSON
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        result.error("Invalid JSON: {}".format(e))
        return result
    except OSError as e:
        result.error("Cannot read file: {}".format(e))
        return result

    # Check required top-level fields
    for field in REQUIRED_SIDECAR_FIELDS:
        if field not in data:
            result.error("Missing required field: '{}'".format(field))

    if result.errors:
        return result

    # schema_version check
    if data["schema_version"] != EXPECTED_SCHEMA_VERSION:
        result.error(
            "schema_version is '{}', expected '{}'".format(
                data["schema_version"], EXPECTED_SCHEMA_VERSION
            )
        )

    # formatVersion check
    if data["formatVersion"] != EXPECTED_FORMAT_VERSION:
        result.error(
            "formatVersion is {}, expected {}".format(
                data["formatVersion"], EXPECTED_FORMAT_VERSION
            )
        )

    # lens object validation
    lens = data.get("lens")
    if not isinstance(lens, dict):
        result.error("'lens' must be an object")
    else:
        for field in REQUIRED_LENS_FIELDS:
            if field not in lens:
                result.error("Missing lens field: '{}'".format(field))

    # dualIsoPatch value check
    patch_val = data.get("dualIsoPatch")
    if patch_val not in VALID_DUAL_ISO_PATCH_VALUES:
        result.error(
            "dualIsoPatch is '{}', expected one of {}".format(
                patch_val, VALID_DUAL_ISO_PATCH_VALUES
            )
        )

    # ettr block (conditional — validate structure if present)
    if "ettr" in data:
        ettr = data["ettr"]
        if not isinstance(ettr, dict):
            result.error("'ettr' must be an object")
        else:
            for field in REQUIRED_ETTR_FIELDS:
                if field not in ettr:
                    result.error("Missing ettr field: '{}'".format(field))
            # channelClip should be array of 3 numbers
            if "channelClip" in ettr:
                clip = ettr["channelClip"]
                if not isinstance(clip, list) or len(clip) != 3:
                    result.error("ettr.channelClip must be an array of 3 floats")
                elif not all(isinstance(v, (int, float)) for v in clip):
                    result.error("ettr.channelClip values must be numeric")

    # dualIso block (conditional — validate structure if present)
    if "dualIso" in data:
        dual = data["dualIso"]
        if not isinstance(dual, dict):
            result.error("'dualIso' must be an object")
        else:
            for field in REQUIRED_DUALISO_FIELDS:
                if field not in dual:
                    result.error("Missing dualIso field: '{}'".format(field))

    return result


def validate_session(filepath):
    """Validate ml_export.json session file. Returns a ValidationResult."""
    result = ValidationResult()

    # Parse JSON
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        result.error("Invalid JSON: {}".format(e))
        return result
    except OSError as e:
        result.error("Cannot read file: {}".format(e))
        return result

    # Check required top-level fields
    for field in REQUIRED_SESSION_FIELDS:
        if field not in data:
            result.error("Missing required field: '{}'".format(field))

    if result.errors:
        return result

    # schema_version check
    if data["schema_version"] != EXPECTED_SCHEMA_VERSION:
        result.error(
            "schema_version is '{}', expected '{}'".format(
                data["schema_version"], EXPECTED_SCHEMA_VERSION
            )
        )

    # formatVersion check
    if data["formatVersion"] != EXPECTED_FORMAT_VERSION:
        result.error(
            "formatVersion is {}, expected {}".format(
                data["formatVersion"], EXPECTED_FORMAT_VERSION
            )
        )

    # sensor object
    sensor = data.get("sensor")
    if not isinstance(sensor, dict):
        result.error("'sensor' must be an object")
    else:
        for field in REQUIRED_SENSOR_FIELDS:
            if field not in sensor:
                result.error("Missing sensor field: '{}'".format(field))
            else:
                val = sensor[field]
                if not isinstance(val, (int, float)) or val <= 0:
                    result.error(
                        "sensor.{} must be a positive number, got {}".format(field, val)
                    )

    # session object
    session = data.get("session")
    if not isinstance(session, dict):
        result.error("'session' must be an object")
    else:
        for field in REQUIRED_SESSION_BLOCK_FIELDS:
            if field not in session:
                result.error("Missing session field: '{}'".format(field))

    return result


# ---------------------------------------------------------------------------
# Directory scanning
# ---------------------------------------------------------------------------

def find_cr2_files(root, cr2_dir=None):
    """Find all .CR2 files under root/DCIM or a custom subdir."""
    if cr2_dir:
        search_path = os.path.join(root, cr2_dir)
    else:
        # Default: look in DCIM/ recursively
        search_path = os.path.join(root, "DCIM")

    if not os.path.isdir(search_path):
        # Fallback: treat root itself as the CR2 directory
        search_path = root

    cr2_files = []
    for dirpath, _dirnames, filenames in os.walk(search_path):
        for fname in filenames:
            if fname.upper().endswith(".CR2"):
                cr2_files.append(os.path.join(dirpath, fname))

    return sorted(cr2_files)


# ---------------------------------------------------------------------------
# Main validation logic
# ---------------------------------------------------------------------------

def run_validation(root, cr2_dir=None):
    """Run full validation on an SD card root. Returns (summary_text, exit_code)."""
    lines = []
    total_pass = 0
    total_fail = 0
    total_warn = 0

    lines.append("## ML_6D Sidecar Validation Report")
    lines.append("")
    lines.append("Card root: `{}`".format(os.path.abspath(root)))
    lines.append("")

    # --- ml_export.json ---
    session_path = os.path.join(root, "ML", "DATA", "ml_export.json")
    lines.append("### Session File: ml_export.json")
    lines.append("")

    if not os.path.isfile(session_path):
        lines.append("- **FAIL**: `ML/DATA/ml_export.json` not found")
        total_fail += 1
    else:
        res = validate_session(session_path)
        if res.passed:
            lines.append("- **PASS**: `ML/DATA/ml_export.json` valid")
            total_pass += 1
        else:
            total_fail += 1
            lines.append("- **FAIL**: `ML/DATA/ml_export.json`")
            for err in res.errors:
                lines.append("  - {}".format(err))

    lines.append("")

    # --- CR2 / sidecar pairing ---
    cr2_files = find_cr2_files(root, cr2_dir)
    lines.append("### Sidecar Coverage")
    lines.append("")
    lines.append("CR2 files found: **{}**".format(len(cr2_files)))
    lines.append("")

    if not cr2_files:
        lines.append("- **WARN**: No .CR2 files found")
        total_warn += 1
    else:
        sidecar_dir = os.path.join(root, "ML", "DATA", "SHOTS")
        missing_sidecars = []
        sidecar_results = []

        for cr2_path in cr2_files:
            basename = os.path.splitext(os.path.basename(cr2_path))[0]
            sidecar_path = os.path.join(sidecar_dir, basename + ".ml6d")

            if not os.path.isfile(sidecar_path):
                missing_sidecars.append(basename)
                total_fail += 1
            else:
                res = validate_sidecar(sidecar_path)
                sidecar_results.append((basename, res))
                if res.passed:
                    total_pass += 1
                else:
                    total_fail += 1

        # Report missing sidecars
        if missing_sidecars:
            lines.append("#### Missing Sidecars ({})".format(len(missing_sidecars)))
            lines.append("")
            for name in missing_sidecars:
                lines.append("- **FAIL**: `{}.ml6d` not found".format(name))
            lines.append("")

        # Report sidecar validation results
        passed_sidecars = [s for s in sidecar_results if s[1].passed]
        failed_sidecars = [s for s in sidecar_results if not s[1].passed]

        if passed_sidecars:
            lines.append(
                "#### Valid Sidecars ({}/{})".format(
                    len(passed_sidecars), len(sidecar_results)
                )
            )
            lines.append("")
            for name, _res in passed_sidecars:
                lines.append("- **PASS**: `{}.ml6d`".format(name))
            lines.append("")

        if failed_sidecars:
            lines.append("#### Invalid Sidecars ({})".format(len(failed_sidecars)))
            lines.append("")
            for name, res in failed_sidecars:
                lines.append("- **FAIL**: `{}.ml6d`".format(name))
                for err in res.errors:
                    lines.append("  - {}".format(err))
            lines.append("")

    # --- Summary ---
    lines.append("### Summary")
    lines.append("")
    lines.append("| Result | Count |")
    lines.append("|--------|-------|")
    lines.append("| PASS   | {}    |".format(total_pass))
    lines.append("| FAIL   | {}    |".format(total_fail))
    lines.append("| WARN   | {}    |".format(total_warn))
    lines.append("")

    overall = "PASS" if total_fail == 0 else "FAIL"
    lines.append("**Overall: {}**".format(overall))
    lines.append("")

    exit_code = 0 if total_fail == 0 else 1
    return "\n".join(lines), exit_code


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Validate ML_6D sidecar (.ml6d) and session (ml_export.json) files "
        "from an SD card or directory.",
        epilog="Exit code: 0 = all checks pass, 1 = one or more failures.",
    )
    parser.add_argument(
        "card_root",
        help="Path to SD card root (or directory containing ML/ and DCIM/ structure)",
    )
    parser.add_argument(
        "--cr2-dir",
        default=None,
        help="Relative subdirectory under card_root containing .CR2 files "
        "(default: searches DCIM/ recursively, falls back to card_root)",
    )
    parser.add_argument(
        "--output",
        "-o",
        default=None,
        help="Write report to a file instead of stdout",
    )

    args = parser.parse_args()

    if not os.path.isdir(args.card_root):
        print("ERROR: '{}' is not a directory".format(args.card_root), file=sys.stderr)
        sys.exit(2)

    report, exit_code = run_validation(args.card_root, args.cr2_dir)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(report)
        print("Report written to: {}".format(args.output))
    else:
        print(report)

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
