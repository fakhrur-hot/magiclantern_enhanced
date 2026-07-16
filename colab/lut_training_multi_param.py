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
FEATURE_COLUMNS = ["LightLevel"]  # future: add "Shutter", histogram-skew, etc.
COLUMN_HEADER = "# Scene|LightLevel|ETTR|ALO|HTP|ISO|WB"

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
    return df.reset_index(drop=True)


def train_iso_model(df, audit):
    """Train DecisionTreeClassifier(max_depth=4) on FEATURE_COLUMNS -> ISO."""
    n = len(df)
    if n < RECOMMENDED_MIN_SAMPLES:
        msg = (f"LOW_SAMPLES|count={n}|recommended={RECOMMENDED_MIN_SAMPLES}"
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


def build_header(samples, wb_fallback):
    """Deterministic comment header lines (byte-identity preserved)."""
    lines = ["# AI-LUT unified.tbl -- generated by lut_training",
             f"# training_samples={samples}"]
    if samples < RECOMMENDED_MIN_SAMPLES:
        lines.append(f"# low_sample_warning: {samples} < recommended {RECOMMENDED_MIN_SAMPLES}")
    if wb_fallback:
        lines.append("# wb_fallback_scenes=" + ",".join(wb_fallback))
    lines.append(COLUMN_HEADER)
    return lines


def write_lut(rows, header_lines, path):
    """Write UTF-8 (no BOM), LF line endings, comment header, then rows."""
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for line in header_lines:
            fh.write(line + "\n")
        for row in rows:
            fh.write(row + "\n")


def write_audit(audit, path):
    """Write the audit trail (UTF-8, LF). Deterministic -- no timestamps."""
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        if not audit:
            fh.write("OK|no warnings\n")
        else:
            for line in audit:
                fh.write(line + "\n")


def main():
    audit = []
    df = parse_log(LOG_PATH)
    df = engineer_features(df, audit)
    clf = train_iso_model(df, audit)
    rows, wb_fallback = generate_lut_rows(df, clf, audit)
    header_lines = build_header(len(df), wb_fallback)
    write_lut(rows, header_lines, LUT_PATH)
    write_audit(audit, AUDIT_PATH)
    joblib.dump(clf, MODEL_PATH)
    print(f"Wrote {LUT_PATH} ({len(rows)} rows), {AUDIT_PATH} "
          f"({len(audit)} note(s)), and {MODEL_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
