#!/usr/bin/env python3
"""analyze_log.py -- Task 10 on-camera validation helper.

Summarizes an exported unified_log.txt: counts error/fallback markers, validates
that every LightLevel is an integer in 0-255, and tabulates logger vs decision
records and per-scene decision distributions. Read-only; safe to run repeatedly.

Usage:
    python tools/analyze_log.py path/to/unified_log.txt
Exit code is non-zero if any anomaly (bad LightLevel, parse error) is found, so
it can gate a validation step.
"""

import sys

MARKERS = [
    "LUT_NOT_FOUND",
    "FALLBACK_USED",
    "HIST_NIL",
    "WB_PARSE_ERROR",
    "ISO_PARSE_ERROR",
]


def parse_blocks(text):
    """Yield each '---'-separated block as a dict of key=value pairs."""
    for block in text.split("---"):
        entry = {}
        for line in block.splitlines():
            if "=" in line and "|" not in line.split("=", 1)[0]:
                key, value = line.split("=", 1)
                entry[key.strip()] = value.strip()
        if entry:
            yield entry


def main(argv):
    if len(argv) != 2:
        print("usage: python tools/analyze_log.py <unified_log.txt>")
        return 2
    path = argv[1]
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    # Line-based marker counts (markers are appended as standalone lines).
    marker_counts = {m: 0 for m in MARKERS}
    for line in text.splitlines():
        for m in MARKERS:
            if line.startswith(m):
                marker_counts[m] += 1

    logger_records = 0
    decision_records = 0
    bad_light = []
    scene_decisions = {}
    for entry in parse_blocks(text):
        is_decision = any(k.startswith("Decision_") for k in entry)
        is_logger = "Hist" in entry
        if is_decision:
            decision_records += 1
            scene = entry.get("Scene", "?")
            iso = entry.get("Decision_ISO", "?")
            scene_decisions.setdefault(scene, {}).setdefault(iso, 0)
            scene_decisions[scene][iso] += 1
        elif is_logger:
            logger_records += 1

        if "LightLevel" in entry:
            raw = entry["LightLevel"]
            try:
                v = int(raw)
                if v < 0 or v > 255:
                    bad_light.append(raw)
            except ValueError:
                bad_light.append(raw)

    print(f"Log file: {path}")
    print(f"  logger records   : {logger_records}")
    print(f"  decision records : {decision_records}")
    print("  markers:")
    for m in MARKERS:
        print(f"    {m:16} {marker_counts[m]}")
    print("  per-scene decision ISO counts:")
    for scene in sorted(scene_decisions):
        counts = ", ".join(f"ISO{iso}x{n}" for iso, n in sorted(scene_decisions[scene].items()))
        print(f"    {scene:10} {counts}")

    anomalies = 0
    if bad_light:
        print(f"  ANOMALY: {len(bad_light)} non-integer / out-of-range LightLevel: {bad_light[:10]}")
        anomalies += 1
    if marker_counts["WB_PARSE_ERROR"] or marker_counts["ISO_PARSE_ERROR"]:
        print("  ANOMALY: parse errors present (see WB_PARSE_ERROR / ISO_PARSE_ERROR above)")
        anomalies += 1

    print("RESULT:", "OK" if anomalies == 0 else f"{anomalies} anomaly type(s) found")
    return 0 if anomalies == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
