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
"""

import sys

import joblib
import pandas as pd
from sklearn.tree import DecisionTreeClassifier

LOG_PATH = "unified_log.txt"
LUT_PATH = "unified.tbl"
MODEL_PATH = "iso_model.joblib"
RANDOM_STATE = 42
HEADER = "# Scene|LightLevel|ETTR|ALO|HTP|ISO|WB"

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


def engineer_features(df):
    """Cast LightLevel/ISO to int and Scene to str; drop rows missing either."""
    if "LightLevel" not in df.columns or "ISO" not in df.columns:
        raise ValueError("Log is missing required 'LightLevel' or 'ISO' column.")

    df = df.copy()
    df["LightLevel"] = pd.to_numeric(df["LightLevel"], errors="coerce")
    df["ISO"] = pd.to_numeric(df["ISO"], errors="coerce")
    if "Scene" not in df.columns:
        df["Scene"] = "unknown"
    df["Scene"] = df["Scene"].fillna("unknown").astype(str)

    bad = df[df["LightLevel"].isna() | df["ISO"].isna()]
    for _ in bad.index:
        print("WARNING: skipping row with missing LightLevel or ISO")
    df = df.dropna(subset=["LightLevel", "ISO"])

    df["LightLevel"] = df["LightLevel"].astype(int)
    df["ISO"] = df["ISO"].astype(int)
    return df.reset_index(drop=True)


def train_iso_model(df):
    """Train DecisionTreeClassifier(max_depth=4) on [LightLevel] -> ISO."""
    if len(df) < 5:
        print(f"WARNING: only {len(df)} training samples (<5); training anyway.")
    clf = DecisionTreeClassifier(max_depth=4, random_state=RANDOM_STATE)
    clf.fit(df[["LightLevel"]], df["ISO"])
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


def generate_lut_rows(df, clf):
    """One row per unique (Scene, LightLevel), sorted by Scene then LightLevel."""
    pairs = sorted(set(zip(df["Scene"], df["LightLevel"])))
    rows = []
    for scene, light in pairs:
        light = int(light)
        iso = int(clf.predict(pd.DataFrame({"LightLevel": [light]}))[0])
        rows.append(
            f"{scene}|{light}|{ettr_for(light)}|{alo_for(light)}|"
            f"{htp_for(light)}|{iso}|{wb_for(scene)}"
        )
    return rows


def write_lut(rows, path):
    """Write UTF-8 (no BOM), LF line endings, comment header."""
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(HEADER + "\n")
        for row in rows:
            fh.write(row + "\n")


def main():
    df = parse_log(LOG_PATH)
    df = engineer_features(df)
    clf = train_iso_model(df)
    rows = generate_lut_rows(df, clf)
    write_lut(rows, LUT_PATH)
    joblib.dump(clf, MODEL_PATH)
    print(f"Wrote {LUT_PATH} ({len(rows)} rows) and {MODEL_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
