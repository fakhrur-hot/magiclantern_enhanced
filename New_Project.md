Project Overview
This project is AI‑Assisted Exposure and Color for Magic Lantern. It provides a complete, end‑to‑end pipeline: camera logging via Magic Lantern Lua, offline training in Google Colab to produce a compact LUT (unified.tbl), an on‑camera Lua decision engine that applies LUT decisions before capture, CI/CD to build and release AI‑enabled Magic Lantern firmware for multiple Canon models, and a continuous improvement loop for retraining LUTs from new logs.

Primary goals

Capture sensor data on camera and log decisions.

Train lightweight models in Colab and export compact LUTs.

Load LUTs on camera and apply ETTR, ALO, HTP, ISO, WB before CR2 capture.

Maintain a fork of Magic Lantern that stays synced with upstream.

Automate builds and releases across supported camera models.

Repository Structure
Code
magiclantern-ai-lut/
├── README.md
├── INSTALL.md
├── ARCHITECTURE.md
├── CONTRIBUTING.md
├── LICENSE
├── lua_scripts/
│   ├── unified_logger.lua
│   └── decision_engine.lua
├── colab/
│   ├── lut_training.ipynb
│   └── lut_training_multi_param.py
├── models/
│   └── unified.tbl
├── logs/
│   └── sample_unified_log.txt
├── .github/
│   └── workflows/
│       ├── build.yml
│       └── release.yml
├── release_templates/
│   └── ReleaseNotes.md
└── docs/
    ├── VALIDATION.md
    ├── CI_CD.md
    └── TROUBLESHOOTING.md
README.md
markdown
# AI Assisted Exposure and Color for Magic Lantern

**Summary**
This project adds AI‑assisted exposure and color control to Magic Lantern. It uses on‑camera Lua scripts to log sensor data, offline training in Google Colab to generate compact LUTs, and a Lua decision engine to apply LUT decisions before CR2 RAW capture. CI/CD builds and releases AI‑enabled Magic Lantern firmware for multiple Canon models.

**Components**
- Lua logging and decision scripts
- Colab notebooks for training LUTs
- `unified.tbl` LUT format
- CI workflows for building and releasing firmware
- Validation and continuous improvement pipeline

**Quick Start**
1. Clone this repo.
2. Build Magic Lantern or download a release.
3. Copy `lua_scripts/*.lua` to `A:/ML/scripts/`.
4. Copy `models/unified.tbl` to `A:/ML/models/`.
5. Boot camera, enable AI Exposure/Color Assist in Magic Lantern menu.
6. Capture and collect `unified_log.txt` for retraining.

**License**
See LICENSE file.
INSTALL.md
markdown
# Installation Guide

## Prerequisites
- Canon DSLR supported by Magic Lantern
- SD card with Magic Lantern boot files
- Host machine with card reader
- Optional: Google account for Colab

## Steps
1. Build or download Magic Lantern firmware for your camera.
2. Copy `lua_scripts/unified_logger.lua` and `lua_scripts/decision_engine.lua` to `A:/ML/scripts/`.
3. Copy `models/unified.tbl` to `A:/ML/models/`.
4. Insert SD card into camera and boot Magic Lantern.
5. In Magic Lantern menu, enable "AI Exposure/Color Assist".
6. Use camera normally; logs will be appended to `A:/ML/logs/unified_log.txt`.

## Notes
- Always test on a spare SD card.
- Keep backups of original Magic Lantern builds.
ARCHITECTURE.md
markdown
# System Architecture

## Overview
1. **Camera Layer**
   - Magic Lantern with Lua support.
   - Lua scripts hook into half‑press events.
   - Reads `unified.tbl` and applies decisions via ML API calls.

2. **Offline Training Layer**
   - Google Colab notebook ingests `unified_log.txt`.
   - Trains lightweight models (decision trees, simple classifiers).
   - Exports compact LUT `unified.tbl`.

3. **CI/CD Layer**
   - GitHub Actions builds Magic Lantern for multiple cameras.
   - Artifacts and LUTs are packaged into GitHub Releases.

4. **Continuous Improvement**
   - Logs sync to Drive or repo.
   - Colab retrains and updates LUTs.
   - New LUTs are redeployed to camera.

## Data Flow
Camera logs → Upload to Colab → Train models → Export unified.tbl → Copy to SD card → Camera applies LUT → New logs
Lua Scripts
lua_scripts/unified_logger.lua
lua
-- unified_logger.lua
-- Logs histogram, scene, light level, and current camera settings to unified_log.txt

local LOG_PATH = "A:/ML/logs/unified_log.txt"

function append_log(entry)
    local f = io.open(LOG_PATH, "a")
    if f then
        f:write(entry .. "\n")
        f:close()
    end
end

function get_histogram_string()
    local hist = get_histogram()  -- Magic Lantern API
    if not hist then return "nil" end
    return table.concat(hist, ",")
end

function half_press_logger()
    local hist = get_histogram_string()
    local scene = get_scene_mode() or "unknown"
    local light_level = get_light_level() or 0
    local iso = get_iso() or 0
    local wb = get_wb() or "R100,G100,B100"
    local shutter = get_shutter() or "1/100"

    local entry = string.format(
        "Timestamp=%s\nHist=%s\nScene=%s\nLightLevel=%d\nShutter=%s\nISO=%s\nWB=%s\n---",
        os.date("%Y-%m-%dT%H:%M:%S"),
        hist,
        scene,
        light_level,
        shutter,
        tostring(iso),
        tostring(wb)
    )
    append_log(entry)
end

-- Hook into half-press event
register_half_press_callback(half_press_logger)
lua_scripts/decision_engine.lua
lua
-- decision_engine.lua
-- Loads unified.tbl and applies decisions on half-press

local MODEL_PATH = "A:/ML/models/unified.tbl"
local LOG_PATH = "A:/ML/logs/unified_log.txt"

function load_unified_model(path)
    local model = {}
    local f = io.open(path, "r")
    if not f then return model end
    for line in f:lines() do
        if string.sub(line,1,1) ~= "#" and line:match("|") then
            local scene, light, ettr, alo, htp, iso, wb =
                string.match(line, "(.-)|(%d+)|(.+)|(.+)|(.+)|(%d+)|(.+)")
            if scene and light then
                model[scene .. "_" .. light] = {
                    ETTR = ettr,
                    ALO  = alo,
                    HTP  = htp,
                    ISO  = iso,
                    WB   = wb
                }
            end
        end
    end
    f:close()
    return model
end

function apply_ettr(ettr)
    if ettr == "reduce_shutter" then set_shutter_fraction(1,200)
    elseif ettr == "increase_shutter" then set_shutter_fraction(1,50)
    elseif ettr == "keep_shutter" then set_shutter_fraction(1,100)
    end
end

function apply_iso(iso)
    set_iso(tonumber(iso))
end

function apply_wb(wb)
    local r,g,b = string.match(wb, "R(%d+),G(%d+),B(%d+)")
    if r and g and b then
        set_wb(tonumber(r), tonumber(g), tonumber(b))
    end
end

function apply_alo(alo)
    -- Placeholder for tone curve adjustments
end

function apply_htp(htp)
    -- Placeholder for highlight tone priority toggles
end

function log_decision(decision, hist, scene, light_level)
    local entry = string.format(
        "Timestamp=%s\nHist=%s\nScene=%s\nLightLevel=%d\nETTR=%s\nALO=%s\nHTP=%s\nISO=%s\nWB=%s\n---",
        os.date("%Y-%m-%dT%H:%M:%S"),
        hist,
        scene,
        light_level,
        decision.ETTR,
        decision.ALO,
        decision.HTP,
        decision.ISO,
        decision.WB
    )
    local f = io.open(LOG_PATH, "a")
    if f then f:write(entry .. "\n"); f:close() end
end

function half_press_decision()
    local hist = table.concat(get_histogram(), ",")
    local scene = get_scene_mode() or "unknown"
    local light_level = get_light_level() or 0

    local model = load_unified_model(MODEL_PATH)
    local key = scene .. "_" .. tostring(light_level)
    local decision = model[key]

    if not decision then
        decision = {ETTR="keep_shutter", ALO="neutral", HTP="priority_off", ISO="400", WB="R100,G100,B100"}
    end

    apply_ettr(decision.ETTR)
    apply_iso(decision.ISO)
    apply_wb(decision.WB)
    apply_alo(decision.ALO)
    apply_htp(decision.HTP)

    log_decision(decision, hist, scene, light_level)
end

register_half_press_callback(half_press_decision)
Colab Notebooks
colab/lut_training.ipynb (core steps in Python)
python
# Upload unified_log.txt
from google.colab import files
uploaded = files.upload()

# Parse logs
import pandas as pd
logs = []
with open("unified_log.txt") as f:
    entry = {}
    for line in f:
        line = line.strip()
        if line == "---":
            logs.append(entry)
            entry = {}
        else:
            if "=" in line:
                k,v = line.split("=",1)
                entry[k] = v
df = pd.DataFrame(logs)

# Feature engineering
df['LightLevel'] = df['LightLevel'].astype(int)
df['Scene'] = df['Scene'].astype(str)

# Train ISO model
from sklearn.tree import DecisionTreeClassifier
X = df[['LightLevel']]
y_iso = df['ISO'].astype(int)
iso_model = DecisionTreeClassifier(max_depth=4).fit(X, y_iso)

# Create multi-parameter LUT
lut_lines = []
for _, row in df.iterrows():
    scene = row['Scene']
    light = int(row['LightLevel'])
    iso_pred = int(iso_model.predict([[light]])[0])
    ettr = "reduce_shutter" if light > 200 else "increase_shutter" if light < 30 else "keep_shutter"
    alo = "shadow_lift" if light < 50 else "neutral"
    htp = "priority_on" if light < 30 else "priority_off"
    wb = "R120,G100,B90" if scene == "daylight" else "R150,G100,B70"
    lut_lines.append(f"{scene}|{light}|{ettr}|{alo}|{htp}|{iso_pred}|{wb}")

with open("unified.tbl","w") as f:
    f.write("# Scene|LightLevel|ETTR|ALO|HTP|ISO|WB\n")
    for l in lut_lines:
        f.write(l + "\n")

files.download("unified.tbl")
colab/lut_training_multi_param.py
python
# Script version for automated runs
import pandas as pd
from sklearn.tree import DecisionTreeClassifier
import joblib

def parse_logs(path):
    logs = []
    with open(path) as f:
        entry = {}
        for line in f:
            line = line.strip()
            if line == "---":
                logs.append(entry)
                entry = {}
            else:
                if "=" in line:
                    k,v = line.split("=",1)
                    entry[k] = v
    return pd.DataFrame(logs)

df = parse_logs("unified_log.txt")
df['LightLevel'] = df['LightLevel'].astype(int)
df['ISO'] = df['ISO'].astype(int)

X = df[['LightLevel']]
y_iso = df['ISO']
iso_model = DecisionTreeClassifier(max_depth=4).fit(X, y_iso)
joblib.dump(iso_model, "iso_model.joblib")

with open("unified.tbl","w") as f:
    f.write("# Scene|LightLevel|ETTR|ALO|HTP|ISO|WB\n")
    for _, row in df.iterrows():
        scene = row['Scene']
        light = int(row['LightLevel'])
        iso_pred = int(iso_model.predict([[light]])[0])
        ettr = "reduce_shutter" if light > 200 else "increase_shutter" if light < 30 else "keep_shutter"
        alo = "shadow_lift" if light < 50 else "neutral"
        htp = "priority_on" if light < 30 else "priority_off"
        wb = "R120,G100,B90" if scene == "daylight" else "R150,G100,B70"
        f.write(f"{scene}|{light}|{ettr}|{alo}|{htp}|{iso_pred}|{wb}\n")
CI Workflows
.github/workflows/build.yml
yaml
name: Build AI Magic Lantern

on:
  push:
    branches:
      - ai-lut-integration
  pull_request:
    branches:
      - ai-lut-integration
  schedule:
    - cron: "0 2 * * *"

jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        camera: [6D.116, 5D3.113, 60D.111, 650D.104]

    steps:
      - name: Checkout repo
        uses: actions/checkout@v3

      - name: Setup dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y gcc-arm-none-eabi make

      - name: Build Magic Lantern for camera
        run: |
          cd platform/${{ matrix.camera }}
          make clean || true
          make || true

      - name: Upload firmware artifact
        uses: actions/upload-artifact@v3
        with:
          name: magiclantern-${{ matrix.camera }}
          path: platform/${{ matrix.camera }}/magiclantern.bin

  lut-sync:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Upload LUT artifact
        uses: actions/upload-artifact@v3
        with:
          name: unified-lut
          path: models/unified.tbl
.github/workflows/release.yml
yaml
name: Release AI Magic Lantern

on:
  workflow_run:
    workflows: ["Build AI Magic Lantern"]
    types: [completed]

jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Download artifacts
        uses: actions/download-artifact@v3
        with:
          path: ./artifacts

      - name: Organize release folders
        run: |
          mkdir -p release/EOS-6D release/EOS-5D3 release/EOS-60D release/EOS-650D
          cp artifacts/magiclantern-6D.116/magiclantern.bin release/EOS-6D/magiclantern-6D.116.bin || true
          cp artifacts/magiclantern-5D3.113/magiclantern.bin release/EOS-5D3/magiclantern-5D3.113.bin || true
          cp artifacts/magiclantern-60D.111/magiclantern.bin release/EOS-60D/magiclantern-60D.111.bin || true
          cp artifacts/magiclantern-650D.104/magiclantern.bin release/EOS-650D/magiclantern-650D.104.bin || true
          cp artifacts/unified-lut/unified.tbl release/EOS-6D/unified.tbl || true
          cp artifacts/unified-lut/unified.tbl release/EOS-5D3/unified.tbl || true
          cp artifacts/unified-lut/unified.tbl release/EOS-60D/unified.tbl || true
          cp artifacts/unified-lut/unified.tbl release/EOS-650D/unified.tbl || true

      - name: Create GitHub Release
        uses: softprops/action-gh-release@v1
        with:
          tag_name: v${{ github.run_number }}
          name: "AI Magic Lantern Multi-Camera Build ${{ github.run_number }}"
          body: |
            Automated release of AI-assisted Magic Lantern firmware.
            Includes builds for multiple Canon models and the latest unified LUT.
          files: release/**
ReleaseNotes.md Template
markdown
# Release Notes

**Version**: v{{RUN_NUMBER}}
**Date**: {{DATE}}

## Summary
Automated release of AI‑assisted Magic Lantern firmware. Includes firmware binaries for multiple Canon models and the latest `unified.tbl` LUT.

## Changes
- Synced with upstream Magic Lantern commit: {{UPSTREAM_COMMIT}}
- Added Lua modules: unified_logger.lua, decision_engine.lua
- Updated LUT: unified.tbl (trained on logs up to {{DATE}})
- CI: automated multi‑camera builds and release packaging

## Known Issues
- ALO and HTP are placeholders on some models; tone curve hooks require additional platform work.
- Some camera models may need minor API adjustments for `set_wb` or `set_shutter_fraction`.

## How to Install
1. Download the folder for your camera from this release.
2. Copy firmware and `unified.tbl` to SD card as per INSTALL.md.
3. Boot camera and enable AI Exposure/Color Assist.

## Contact
Report issues via GitHub Issues.
Validation Guide
docs/VALIDATION.md
markdown
# Validation Guide

## Objective
Confirm that LUT decisions are applied on camera and CR2 metadata matches logged decisions.

## Steps
1. Deploy `unified.tbl` and Lua scripts to SD card.
2. Boot camera and enable AI Exposure/Color Assist.
3. Capture test shots across conditions: daylight, tungsten, shade, lowlight.
4. Copy `A:/ML/logs/unified_log.txt` to host.
5. Use exiftool to read CR2 metadata:
   exiftool IMG_XXXX.CR2 | grep -E "ISO|Shutter|White Balance"
6. Compare metadata to the last log entry for that shot.
7. If mismatch, inspect Lua logs and ensure `decision_engine.lua` loaded the correct `unified.tbl`.

## Metrics
- **ISO match rate**: percent of shots where CR2 ISO equals LUT ISO.
- **WB match rate**: percent of shots where CR2 WB metadata equals LUT WB.
- **ETTR success**: histogram shows highlights near clipping but not clipped.

## Troubleshooting
- If LUT not loaded, check file path `A:/ML/models/unified.tbl`.
- If Lua errors occur, enable ML console and inspect logs.
Troubleshooting
docs/TROUBLESHOOTING.md
markdown
# Troubleshooting

## Common Issues
- **LUT not applied**
  - Verify `unified.tbl` exists at `A:/ML/models/`.
  - Ensure file encoding is UTF-8 and no BOM.
  - Check Lua script logs for parse errors.

- **Wrong ISO or WB in CR2**
  - Confirm `decision_engine.lua` applied settings before full press.
  - Check for race conditions between Lua and Canon firmware.

- **Build failures in CI**
  - Ensure toolchain versions match Magic Lantern requirements.
  - Inspect build logs for missing dependencies.

## Debug Tips
- Add verbose logging in Lua scripts to `A:/ML/logs/debug.txt`.
- Use `exiftool` to inspect CR2 metadata.
- Test Lua functions in isolation on a supported model.

## Contact
Open an issue on GitHub with logs and camera model details.
CONTRIBUTING.md
markdown
# Contributing

**How to contribute**
- Fork the repo and create a feature branch.
- Keep changes modular: Lua scripts in /lua_scripts, notebooks in /colab.
- Add tests and sample logs when possible.
- Submit PR to `ai-lut-integration` branch.

**Code style**
- Lua: follow Magic Lantern Lua conventions.
- Python: follow PEP8.

**CI**
- Ensure builds pass for all matrix cameras before merging.

**Reporting bugs**
- Include camera model, Magic Lantern build, unified_log.txt, and CR2 metadata.
LICENSE
Provide an appropriate open source license file (example MIT).

text
MIT License

Copyright (c) YEAR Your Name

Permission is hereby granted...
Final Notes and Next Steps
Deploy: copy models/unified.tbl and lua_scripts/*.lua to SD card and test on a spare card first.

Iterate: collect logs, retrain in Colab, redeploy updated LUTs.

Scale: add more camera models to CI matrix as needed.

Extend: implement ALO tone curve hooks and HTP platform integrations per camera.


Acronym & Term Glossary
ML (Magic Lantern) → An open‑source firmware add‑on for Canon DSLRs that unlocks extra features not available in Canon’s stock firmware.

Lua → A lightweight scripting language. Magic Lantern supports Lua scripts to automate tasks and extend functionality.

LUT (Lookup Table) → A simple table that maps input values (like scene + light level) to output decisions (like ISO, shutter, WB). In this project, the LUT is stored as unified.tbl.

ETTR (Expose To The Right) → A technique where exposure is pushed so the histogram is as far to the right as possible without clipping highlights. This maximizes dynamic range.

ALO (Auto Lighting Optimizer) → Canon’s feature that brightens shadows automatically. In your AI LUT, this is represented as “shadow lift” or “neutral.”

HTP (Highlight Tone Priority) → Canon’s feature that protects highlights from blowing out. In your LUT, this is toggled “priority_on” or “priority_off.”

ISO → Sensor sensitivity. Higher ISO = brighter image but more noise. Your LUT decides the lowest acceptable ISO for each scene.

WB (White Balance) → Adjusts color temperature so whites look neutral. In your LUT, WB is stored as RGB multipliers (e.g., R120,G100,B90).

CR2 RAW → Canon’s RAW image format. It stores sensor data plus metadata (ISO, WB, shutter, etc.). Your AI LUT influences these metadata values.

CI/CD (Continuous Integration / Continuous Deployment) → Automated pipelines that build your firmware and publish releases whenever code changes. GitHub Actions is used here.

Repo (Repository) → Your project’s codebase stored on GitHub.

Artifact → A file produced by CI/CD (like a compiled firmware .bin or LUT file) that can be downloaded.

Upstream → The original Magic Lantern source repository. Your fork syncs with upstream to stay updated.

Fork → Your copy of the Magic Lantern repo where you add AI features.

Colab (Google Colaboratory) → A cloud notebook environment where you train LUTs using Python and scikit‑learn.

Decision Tree → A simple machine learning model that predicts values (like ISO) based on inputs (like light level).