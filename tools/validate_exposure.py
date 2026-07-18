#!/usr/bin/env python3
"""validate_exposure.py -- measure true CR2 exposure vs the logged LightLevel.

Answers two questions the on-camera log alone cannot:

  A. INPUT FIDELITY -- does the logged LightLevel (from the LiveView *preview*
     histogram) track the *actual* exposure of the captured CR2 RAW?
  B. EXPOSURE CORRECTNESS (ETTR oracle) -- is each CR2 exposed to the right:
     highlights just below clipping, without blowing them?

Ground truth comes from the RAW pixels (via rawpy/libraw), NOT metadata -- ISO
and shutter in EXIF are what the camera *used*, not how the photo came out.

Pairing: each log record carries FileNum (the file number at half-press). Fully
pressing then writes IMG_<FileNum+1>.CR2, so CR2 number N pairs with the log
record whose FileNum is N-1 (falls back to N).

Usage:
    python tools/validate_exposure.py <unified_log.txt> <dir-with-CR2s>
"""

import os
import re
import sys
import glob

import numpy as np

try:
    import rawpy
except ImportError:
    print("ERROR: rawpy not installed. Run: python -m pip install rawpy", file=sys.stderr)
    sys.exit(2)

# ETTR oracle thresholds (on the RAW green channel, black-subtracted, linear).
CLIP_FRAC_MAX = 0.005   # >0.5% pixels at saturation => highlights blown
HEADROOM_EV_MAX = 1.0   # >1 EV below saturation at the 99.5th pct => underexposed


def raw_exposure_stats(path):
    """Return dict of true-exposure stats from a CR2's RAW green channel."""
    with rawpy.imread(path) as raw:
        img = raw.raw_image_visible.astype(np.float64)
        colors = raw.raw_colors_visible
        white = float(raw.white_level)
        blk = raw.black_level_per_channel
        # green CFA positions are colour indices 1 and 3
        gmask = (colors == 1) | (colors == 3)
        black_g = float(np.mean([blk[1], blk[3]]))
        g = img[gmask] - black_g
        g = np.clip(g, 0, None)
    span = max(white - black_g, 1.0)
    p90 = float(np.percentile(g, 90))
    p995 = float(np.percentile(g, 99.5))
    median = float(np.percentile(g, 50))
    clip_frac = float(np.mean(img[gmask] >= 0.99 * white))
    headroom_ev = float(np.log2(span / max(p995, 1.0)))
    # normalise the 90th percentile to 0..255 to compare with the logged LightLevel
    light_true = int(round(p90 / span * 255))
    if clip_frac > CLIP_FRAC_MAX:
        verdict = "CLIPPED"
    elif headroom_ev > HEADROOM_EV_MAX:
        verdict = "UNDER"
    else:
        verdict = "ETTR-OK"
    return dict(light_true=light_true, median=int(round(median / span * 255)),
                clip_pct=clip_frac * 100, headroom_ev=headroom_ev, verdict=verdict)


def parse_log(path):
    """Return list of dicts for records that carry FileNum."""
    recs = []
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    for blk in text.split("---"):
        d = {}
        for line in blk.strip().splitlines():
            if "=" in line and not line.startswith("Hist"):
                k, v = line.split("=", 1)
                d[k.strip()] = v.strip()
        if "FileNum" in d and "LightLevel" in d:
            recs.append(d)
    return recs


def cr2_number(path):
    m = re.search(r"(\d+)\.CR2$", os.path.basename(path), re.IGNORECASE)
    return int(m.group(1)) if m else None


def main(argv):
    if len(argv) != 3:
        print("usage: python tools/validate_exposure.py <unified_log.txt> <cr2_dir>")
        return 2
    log_path, cr2_dir = argv[1], argv[2]

    recs = parse_log(log_path)
    by_filenum = {}
    for r in recs:
        try:
            by_filenum.setdefault(int(r["FileNum"]), []).append(r)
        except ValueError:
            pass
    print(f"log records with FileNum: {len(recs)}")

    # de-dupe (case-insensitive filesystems return the same file for *.CR2/*.cr2)
    seen, cr2s = set(), []
    for p in glob.glob(os.path.join(cr2_dir, "*.CR2")) + glob.glob(os.path.join(cr2_dir, "*.cr2")):
        key = os.path.normcase(os.path.abspath(p))
        if key not in seen:
            seen.add(key)
            cr2s.append(p)
    cr2s.sort(key=lambda p: p.lower())
    if not cr2s:
        print("No CR2 files found in", cr2_dir)
        return 1

    logged, trues, verdicts, skipped = [], [], [], 0
    print(f"\n{'CR2':>13} {'log_LL':>7} {'raw_LL':>7} {'clip%':>7} {'hdr_EV':>7} {'verdict':>9}  pair")
    for path in cr2s:
        n = cr2_number(path)
        try:
            st = raw_exposure_stats(path)
        except Exception as e:
            skipped += 1
            print(f"{os.path.basename(path):>13} {'--':>7} {'--':>7}  SKIP (unsupported RAW: {e})")
            continue
        verdicts.append(st["verdict"])
        rec = None
        for cand in (n - 1, n):   # CR2 N pairs with the half-press logged at FileNum N-1
            if cand in by_filenum:
                rec = by_filenum[cand][-1]
                break
        log_ll = int(rec["LightLevel"]) if rec else None
        pair = f"FileNum={rec['FileNum']}" if rec else "(no log match)"
        if log_ll is not None:
            logged.append(log_ll); trues.append(st["light_true"])
        print(f"{os.path.basename(path):>13} {str(log_ll):>7} {st['light_true']:>7} "
              f"{st['clip_pct']:>6.2f} {st['headroom_ev']:>7.2f} {st['verdict']:>9}  {pair}")

    if skipped:
        print(f"({skipped} file(s) skipped -- likely mRAW/sRAW, not flat Bayer)")

    # Check A: input fidelity (rank correlation logged LightLevel vs RAW-true)
    if len(logged) >= 3:
        lr = np.argsort(np.argsort(logged)); tr = np.argsort(np.argsort(trues))
        m = len(logged)
        rho = 1 - 6 * np.sum((lr - tr) ** 2) / (m * (m * m - 1))
        print(f"\nCheck A (input fidelity): Spearman rho(logged LightLevel, RAW-true) = {rho:.2f}  "
              f"[1.0=perfect proxy, ~0=unrelated]  n={m}")
    else:
        print("\nCheck A: need >=3 FileNum-paired frames (shoot with the FileNum firmware).")

    # Check B: ETTR oracle summary
    from collections import Counter
    c = Counter(verdicts)
    total = sum(c.values())
    print(f"Check B (ETTR oracle, n={total}): " + ", ".join(f"{k}={v}" for k, v in c.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
