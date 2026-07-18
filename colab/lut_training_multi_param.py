#!/usr/bin/env python3
"""lut_training_multi_param.py -- AI-LUT offline training (REQ-004).

Standalone CLI replica of colab/lut_training.ipynb. Reads unified_log.txt from
the current working directory, trains a DecisionTreeClassifier(max_depth=4) that
predicts ISO from LightLevel, applies the finalized integer threshold rules for
ETTR / ALO / HTP and the scene-based WB multiplier table, and writes a
deterministic unified.tbl (fixed seed, sorted rows) plus iso_model.joblib for
incremental retraining.

Design is finalized: the thresholds (200/30/20/50/30) and the WB table are the
same integers used by the on-camera decision engine -- there is no translation
layer. Output is byte-identical to the notebook for the same input.

Traceability (all deterministic, so byte-identity is preserved):
  * The unified.tbl header records the training sample count and flags a low
    sample count (recommended minimum RECOMMENDED_MIN_SAMPLES) and any scenes
    that fell back to neutral WB.
  * Skipped/corrupt rows, low-sample notes, and WB fallbacks are also written to
    a separate training_audit.log for offline diagnosis.

Future hook: ISO is predicted from LightLevel alone, so predictions are coarse.
If logs gain more features (shutter, histogram skew), extend FEATURE_COLUMNS and
the training frame -- the LUT format and thresholds do not change.
"""

import sys

import joblib
import pandas as pd
from sklearn.tree import DecisionTreeClassifier

LOG_PATH = "unified_log.txt"
LUT_PATH = "unified.tbl"
MODEL_PATH = "iso_model.joblib"
AUDIT_PATH = "training_audit.log"
RANDOM_STATE = 42
RECOMMENDED_MIN_SAMPLES = 200  # advisory; training still proceeds below this
FEATURE_COLUMNS = ["LightLevel"]  # future: add "RawMed", "RawP99", "RawR", "RawB"
COLUMN_HEADER = "# Scene|LightLevel|ETTR|ALO|HTP|ISO|WB"

# On-camera limits (modules/ettr/ai_lut.h): the firmware reads the LUT with ONE
# 8 KB FIO call and keeps at most 128 rows -- anything beyond is silently lost.
# Quantizing bounds the row count deterministically: LightLevel buckets of 8
# (<=32 per scene) and ISO snapped to full stops (mirrors ai_iso_to_raw).
LUT_MAX_ROWS = 128
LUT_MAX_BYTES = 8192
LIGHT_BUCKET = 8
ISO_MAX_STOPS = 8  # 100 << 8 = 25600


def quantize_iso(iso):
    """Nearest full stop from 100..25600; same integer rule as the firmware."""
    iso = int(iso)
    v, stops = 100, 0
    while v * 2 <= iso and stops < ISO_MAX_STOPS:
        v *= 2
        stops += 1
    if stops < ISO_MAX_STOPS and iso * 2 >= v * 3:
        v *= 2
    return v

# Scene -> (R, G, B) integer sensor-gain multipliers, G=100 reference.
WB_TABLE = {
    "daylight": (120, 100, 90),
    "shade": (110, 100, 105),
    "tungsten": (150, 100, 70),
    "lowlight": (130, 100, 80),
    "unknown": (100, 100, 100),
}


def parse_log(path):
    """Parse a key=value log (entries separated by '---') into a DataFrame."""
    with open(path, "r", encoding="utf-8") as fh:
        raw = fh.read()

    records = []
    for block in raw.split("---"):
        block = block.strip()
        if not block:
            continue
        entry = {}
        for line in block.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                entry[key.strip()] = value.strip()
        if entry:
            records.append(entry)

    if not records:
        raise ValueError(
            f"No log entries found in {path!r}; cannot train on an empty log."
        )
    return pd.DataFrame(records)


def engineer_features(df, audit):
    """Cast LightLevel/ISO to int and Scene to str; drop rows missing either.

    Appends one SKIPPED_ROW audit line per dropped row (deterministic order).
    """
    if "LightLevel" not in df.columns or "ISO" not in df.columns:
        raise ValueError("Log is missing required 'LightLevel' or 'ISO' column.")

    df = df.copy()
    raw_light = df["LightLevel"] if "LightLevel" in df else None
    raw_iso = df["ISO"] if "ISO" in df else None
    df["LightLevel"] = pd.to_numeric(df["LightLevel"], errors="coerce")
    df["ISO"] = pd.to_numeric(df["ISO"], errors="coerce")
    if "Scene" not in df.columns:
        df["Scene"] = "unknown"
    df["Scene"] = df["Scene"].fillna("unknown").astype(str)

    bad_mask = df["LightLevel"].isna() | df["ISO"].isna()
    for idx in df.index[bad_mask]:
        lv = raw_light.iloc[idx] if raw_light is not None else "?"
        iso = raw_iso.iloc[idx] if raw_iso is not None else "?"
        audit.append(f"SKIPPED_ROW|reason=non-numeric LightLevel/ISO|LightLevel={lv}|ISO={iso}")
    df = df[~bad_mask]

    df["LightLevel"] = df["LightLevel"].astype(int)
    df["ISO"] = df["ISO"].astype(int)
    # Quantize so the LUT stays within the camera's 128-row / 8 KB caps and ISO
    # matches the firmware's full-stop ladder. Deterministic (integer rules).
    df["LightLevel"] = (df["LightLevel"] // LIGHT_BUCKET) * LIGHT_BUCKET
    df["ISO"] = df["ISO"].map(quantize_iso)
    return df.reset_index(drop=True)


def train_iso_model(df, audit, min_samples):
    """Train DecisionTreeClassifier(max_depth=4) on FEATURE_COLUMNS -> ISO."""
    n = len(df)
    if n < min_samples:
        msg = (f"LOW_SAMPLES|count={n}|recommended={min_samples}"
               "|note=training proceeds but ISO predictions may be unreliable")
        audit.append(msg)
        print("WARNING:", msg)
    clf = DecisionTreeClassifier(max_depth=4, random_state=RANDOM_STATE)
    clf.fit(df[FEATURE_COLUMNS], df["ISO"])
    return clf


def ettr_for(light):
    if light > 200:
        return "reduce_shutter"
    if light < 30:
        return "increase_shutter"
    return "keep_shutter"


def alo_for(light):
    if light < 20:
        return "shadow_boost"
    if light < 50:
        return "shadow_lift"
    return "neutral"


def htp_for(light):
    if light < 30:
        return "priority_on"
    return "priority_off"


def wb_for(scene):
    r, g, b = WB_TABLE.get(scene, (100, 100, 100))
    return f"R{r},G{g},B{b}"


def generate_lut_rows(df, clf, audit):
    """One row per unique (Scene, LightLevel), sorted by Scene then LightLevel.

    Records WB_FALLBACK_USED (once per unknown scene) and returns the sorted list
    of scenes that fell back to neutral WB.
    """
    pairs = sorted(set(zip(df["Scene"], df["LightLevel"])))
    rows = []
    wb_fallback = set()
    for scene, light in pairs:
        light = int(light)
        if scene not in WB_TABLE and scene not in wb_fallback:
            wb_fallback.add(scene)
            audit.append(f"WB_FALLBACK_USED|scene={scene}|fallback=R100,G100,B100")
        iso = int(clf.predict(pd.DataFrame({col: [light] for col in FEATURE_COLUMNS}))[0])
        rows.append(
            f"{scene}|{light}|{ettr_for(light)}|{alo_for(light)}|"
            f"{htp_for(light)}|{iso}|{wb_for(scene)}"
        )
    return rows, sorted(wb_fallback)


def build_header(samples, wb_fallback, min_samples):
    """Deterministic comment header lines (byte-identity preserved)."""
    lines = ["# AI-LUT unified.tbl -- generated by lut_training",
             f"# quantized: LightLevel bucket={LIGHT_BUCKET}, ISO=full stops",
             f"# training_samples={samples}"]
    if samples < min_samples:
        lines.append(f"# low_sample_warning: {samples} < recommended {min_samples}")
    if wb_fallback:
        lines.append("# wb_fallback_scenes=" + ",".join(wb_fallback))
    lines.append(COLUMN_HEADER)
    return lines


def write_lut(rows, header_lines, path):
    """Write UTF-8 (no BOM), LF line endings, comment header, then rows.

    Hard-fails if the LUT exceeds the camera caps -- a deterministic error is
    better than silent truncation at ai_lut_load() on camera.
    """
    content = "".join(l + "\n" for l in header_lines) + "".join(r + "\n" for r in rows)
    if len(rows) > LUT_MAX_ROWS:
        raise ValueError(f"LUT has {len(rows)} rows; camera keeps only {LUT_MAX_ROWS}.")
    if len(content.encode("utf-8")) > LUT_MAX_BYTES:
        raise ValueError(f"LUT is {len(content.encode('utf-8'))} bytes; camera reads only {LUT_MAX_BYTES}.")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(content)


def audit_summary(samples, audit, wb_fallback):
    """Deterministic count summary written at the top of the audit file."""
    skipped = sum(1 for line in audit if line.startswith("SKIPPED_ROW"))
    return [
        f"TRAINING_SAMPLES={samples}",
        f"SKIPPED_ROW_COUNT={skipped}",
        f"WB_FALLBACK_COUNT={len(wb_fallback)}",
    ]


def write_audit(summary, audit, path):
    """Write the audit trail (UTF-8, LF): summary counts, then detail lines."""
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for line in summary:
            fh.write(line + "\n")
        if audit:
            for line in audit:
                fh.write(line + "\n")
        else:
            fh.write("OK|no detail warnings\n")


def parse_args(argv):
    import argparse
    p = argparse.ArgumentParser(description="AI-LUT offline training (REQ-004).")
    p.add_argument("--min-samples", type=int, default=RECOMMENDED_MIN_SAMPLES,
                   help=f"advisory minimum training samples (default {RECOMMENDED_MIN_SAMPLES}); "
                        "training still proceeds below it")
    return p.parse_args(argv[1:])


def main(argv):
    args = parse_args(argv)
    audit = []
    df = parse_log(LOG_PATH)
    df = engineer_features(df, audit)
    clf = train_iso_model(df, audit, args.min_samples)
    rows, wb_fallback = generate_lut_rows(df, clf, audit)
    header_lines = build_header(len(df), wb_fallback, args.min_samples)
    write_lut(rows, header_lines, LUT_PATH)
    write_audit(audit_summary(len(df), audit, wb_fallback), audit, AUDIT_PATH)
    joblib.dump(clf, MODEL_PATH)
    print(f"Wrote {LUT_PATH} ({len(rows)} rows), {AUDIT_PATH} "
          f"({len(audit)} note(s)), and {MODEL_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
