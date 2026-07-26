#ifndef _ai_lut_h_
#define _ai_lut_h_

/* AI-LUT integration for the ETTR Auto ISO Optimizer.
 *
 * Reads ML/models/unified.tbl and returns the learned per-scene targets
 * (ISO / WB / ALO / HTP) that the offline training produced. The real metered
 * ETTR still owns the exposure push; this only supplies learned starting points
 * -- a strict superset of stock ETTR. See docs/ETTR_AI_INTEGRATION.md.
 *
 * Parsing is manual (no sscanf scansets). Light-level math is integer-only.
 */

#include <fio-ml.h>
#include <version.h>
#include <propvalues.h>
#include <math.h>

/* Forward declarations for symbols defined further down in ettr.c but needed
 * here -- this header is included near the TOP of ettr.c (before auto_ettr /
 * the ETTR metadata type are defined), so we declare just enough here rather
 * than reordering the whole file.
 *   auto_ettr           CONFIG_INT("auto.ettr", ...) -> plain global int.
 *   ettr_metadata_t /
 *   ettr_last_metadata  ETTR extended-metadata type/getter, defined in ettr.c;
 *                        the type is declared here so ettr.c's definition and
 *                        any header use agree on one canonical type. */
extern int auto_ettr;

typedef struct { int light_level; float scene_dr; float highlight_headroom; float clip_r, clip_g, clip_b; } ettr_metadata_t;
ettr_metadata_t ettr_last_metadata(void);

/* picstyle setters are declared core-only (FEATURE_PICSTYLE) in picstyle.h, but
 * the symbols ARE exported to modules (present in the core .sym) -- declare the
 * prototypes ourselves so we can drive Canon's picture style from the module. */
extern void picstyle_set_current_contrast(int value);
extern void picstyle_set_current_saturation(int value);
extern void picstyle_set_current_color_tone(int value);

#define AI_LUT_PATH    "ML/models/unified.tbl"
#define AI_LENS_TUNE_PATH "ML/models/lens_tune.tbl"
#define AI_WB_NEUTRAL  1024   /* lens_set_custom_wb_gains scale: 1024 = 1.0x */
/* daylight AsShotNeutral for this sensor family x1024 (src/chdk-dng.c) --
 * the fallback WB prior when a scene is too dark to measure */
#define AI_WB_DAY_R    485
#define AI_WB_DAY_B    639

/* Global green-cast compensation (2026-07-26). Field ground truth: Canon's own
 * WB_RGGBLevelsMeasured (well-calibrated AWB) vs ML's applied WB_RGGBLevelsAsShot
 * showed ML systematically UNDER-boosting blue (~+7-8%) and slightly red (~+3%)
 * -> persistent yellow-green cast on daylight/flat scenes (e.g. IMG_6397: ML
 * 1544/1419 vs Canon 2009/1820). Higher R/B gain = more red/blue = LESS green.
 * Applied to the final estimate so it corrects BOTH the white-point and the
 * daylight-prior paths. Tune against AIWB_DBG.TXT + the CR2's
 * WB_RGGBLevelsMeasured; raise if still green, lower if it goes magenta. */
#define AI_WB_GREENCOMP_R  103   /* x1.03 red  */
#define AI_WB_GREENCOMP_B  108   /* x1.08 blue */
#define AI_LUT_MAXROWS 128
#define AI_LUT_BUFSZ   8192   /* single FIO_ReadFile cap; keep the LUT < 8 KB */

struct ai_lut_row
{
    char scene[16];
    int  light;              /* 0..255 */
    int  iso;                /* 100..3200 */
    int  wb_r, wb_g, wb_b;   /* integer gains, G=100 reference */
    int  alo;                /* ALO_STD / ALO_LOW / ALO_HIGH */
    int  htp;                /* 0 / 1 */
};

static struct ai_lut_row ai_rows[AI_LUT_MAXROWS];
static int ai_nrows = 0;

static int ai_alo_from_kw(const char * kw)
{
    if (streq(kw, "shadow_boost")) return ALO_HIGH;   /* 2 */
    if (streq(kw, "shadow_lift"))  return ALO_LOW;    /* 1 */
    return ALO_STD;                                   /* 0: neutral */
}

/* ISO -> ML raw iso, rounded to the NEAREST full stop (not up):
 * 100->72, 200->80, ... 25600->136. e.g. 2000 -> 1600 (not 3200);
 * midpoint rule iso*2 >= v*3 goes to the higher stop. Integer-only. */
static int ai_iso_to_raw(int iso)
{
    int stops = 0, v = 100;
    while (v * 2 <= iso && stops < 8) { v *= 2; stops++; }
    if (stops < 8 && iso * 2 >= v * 3) stops++;
    return 72 + 8 * stops;
}

/* Split a NUL-terminated line in place on '|'. Returns field count. */
static int ai_split(char * line, char * fields[], int maxf)
{
    int n = 0;
    fields[n++] = line;
    for (char * c = line; *c && n < maxf; c++)
    {
        if (*c == '|') { *c = 0; fields[n++] = c + 1; }
    }
    return n;
}

/* Parse "R<r>,G<g>,B<b>" -> ints. Returns 1 on success. */
static int ai_parse_wb(const char * s, int * r, int * g, int * b)
{
    const char * c;
    if (*s != 'R') return 0;
    s++;
    *r = atoi(s);
    c = strchr(s, ',');
    if (!c) return 0;
    s = c + 1;
    if (*s != 'G') return 0;
    s++;
    *g = atoi(s);
    c = strchr(s, ',');
    if (!c) return 0;
    s = c + 1;
    if (*s != 'B') return 0;
    s++;
    *b = atoi(s);
    return 1;
}

/* Load + parse unified.tbl. Returns row count (0 if absent/empty/unreadable). */
static int ai_lut_load(void)
{
    static char buf[AI_LUT_BUFSZ + 1];
    ai_nrows = 0;

    int rc = read_file(AI_LUT_PATH, buf, AI_LUT_BUFSZ);
    if (rc <= 0) return 0;
    if (rc > AI_LUT_BUFSZ) rc = AI_LUT_BUFSZ;
    buf[rc] = 0;

    char * p = buf;
    while (*p && ai_nrows < AI_LUT_MAXROWS)
    {
        char * nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') nl++;
        char saved = *nl;
        *nl = 0;

        if (*p != '#' && strchr(p, '|'))
        {
            char * f[8];
            int nf = ai_split(p, f, 8);
            int r, g, b;
            if (nf >= 7 && ai_parse_wb(f[6], &r, &g, &b))
            {
                int light = atoi(f[1]);
                int iso   = atoi(f[5]);
                /* reject rows with insane values (atoi returns 0 on garbage;
                 * WB gains of 0 must never reach lens_set_custom_wb_gains) */
                int sane = (light >= 0 && light <= 255)
                        && (iso >= 50 && iso <= 25600)
                        && (r >= 10 && r <= 400)
                        && (g >= 10 && g <= 400)
                        && (b >= 10 && b <= 400);
                if (sane)
                {
                    struct ai_lut_row * row = &ai_rows[ai_nrows];
                    snprintf(row->scene, sizeof(row->scene), "%s", f[0]);
                    row->light = light;
                    row->alo   = ai_alo_from_kw(f[3]);
                    row->htp   = streq(f[4], "priority_on") ? 1 : 0;
                    row->iso   = iso;
                    row->wb_r = r; row->wb_g = g; row->wb_b = b;
                    ai_nrows++;
                }
            }
        }

        *nl = saved;
        p = nl;
        while (*p == '\n' || *p == '\r') p++;
    }
    return ai_nrows;
}

/* Light-level source of the last ai_light_level() call: 1 = RAW histogram
 * (true exposure), 0 = LiveView display histogram (preview brightness). */
static int ai_light_src = 0;

/* Extra raw stats from the last ai_light_level() call (0..255, -1 = n/a):
 * green median + 99th percentile (clip proximity). Free -- same metering pass. */
static int ai_raw_med = -1;
static int ai_raw_p99 = -1;

/* Normalize a raw sensor level to 0..255 using black/white points. -1 on n/a. */
static int ai_raw_to_255(int rawv)
{
    if (rawv < 0) return -1;
    int black = raw_info.black_level;
    int span = raw_info.white_level - black;
    if (span <= 0) return -1;
    int v = rawv - black;
    if (v < 0) v = 0;
    int light = v * 255 / span;
    return (light > 255) ? 255 : light;
}

/* 90th-percentile green light level, 0..255.
 *
 * Prefer the RAW histogram (raw_hist_get_percentile_levels, GREEN + DARK_ONLY):
 * it reflects true sensor exposure, unlike the display histogram which is
 * post-gamma and ExpSim-brightened (measured uncorrelated with real exposure,
 * Spearman ~0). Needs raw LV active -- requested in the polling CBR while
 * logging is on. Falls back to the display histogram when raw is unavailable.
 * One metering pass also yields the median and P99 (stored in ai_raw_med/p99).
 */
static int ai_light_level(void)
{
    int pcts[3] = {990, 900, 500};  /* x10: P99, P90, P50 (ETTR convention) */
    int out[3]  = {-1, -1, -1};
    raw_hist_get_percentile_levels(pcts, out, 3,
        GRAY_PROJECTION_GREEN | GRAY_PROJECTION_DARK_ONLY, 4 /* subsampled */);
    int light = ai_raw_to_255(out[1]);
    if (light >= 0)
    {
        ai_raw_p99 = ai_raw_to_255(out[0]);
        ai_raw_med = ai_raw_to_255(out[2]);
        ai_light_src = 1;
        return light;
    }

    /* Fallback: LiveView display histogram (brightness proxy, not exposure). */
    ai_light_src = 0;
    ai_raw_med = ai_raw_p99 = -1;
    uint32_t * h = histogram.is_rgb ? histogram.hist_g : histogram.hist;
    uint32_t total = 0;
    for (int i = 0; i < HIST_WIDTH; i++) total += h[i];
    if (total == 0) return 128;
    uint32_t thr = total * 9 / 10;
    uint32_t cum = 0;
    for (int i = 0; i < HIST_WIDTH; i++)
    {
        cum += h[i];
        if (cum >= thr) return (i * 255) / (HIST_WIDTH - 1);
    }
    return 255;
}

/* Raw R/B channel medians (0..255; -1 = n/a). Logged so offline training can
 * LEARN white balance and scene from channel ratios (R/G, B/G) instead of the
 * hardcoded WB table -- the data the WB/scene collection sessions need.
 * Only called from the logging path (2 extra subsampled passes). */
static void ai_sample_rb(int * r_med, int * b_med)
{
    int pct = 500, rv = -1;
    raw_hist_get_percentile_levels(&pct, &rv, 1,
        GRAY_PROJECTION_RED | GRAY_PROJECTION_DARK_ONLY, 4);
    *r_med = ai_raw_to_255(rv);
    pct = 500; rv = -1;
    raw_hist_get_percentile_levels(&pct, &rv, 1,
        GRAY_PROJECTION_BLUE | GRAY_PROJECTION_DARK_ONLY, 4);
    *b_med = ai_raw_to_255(rv);
}

/* Best-effort scene from the camera's WB Kelvin; "unknown" otherwise (we then
 * leave WB untouched rather than guess). */
static const char * ai_scene(void)
{
    int k = (int) lens_info.kelvin;
    if (lens_info.wb_mode != WB_KELVIN || k <= 0) return "unknown";
    if (k <= 3500) return "tungsten";
    if (k <= 4800) return "lowlight";
    if (k <= 6000) return "daylight";
    return "shade";
}

/* Exact match, then integer nearest-neighbor within scene, then "unknown". */
static int ai_find(const char * scene, int light)
{
    int best = -1, best_d = 1 << 30;

    for (int i = 0; i < ai_nrows; i++)
        if (ai_rows[i].light == light && streq(ai_rows[i].scene, scene))
            return i;

    for (int i = 0; i < ai_nrows; i++)
        if (streq(ai_rows[i].scene, scene))
        {
            int d = ai_rows[i].light - light; if (d < 0) d = -d;
            if (d < best_d) { best_d = d; best = i; }
        }
    if (best >= 0) return best;

    for (int i = 0; i < ai_nrows; i++)
        if (streq(ai_rows[i].scene, "unknown"))
        {
            int d = ai_rows[i].light - light; if (d < 0) d = -d;
            if (d < best_d) { best_d = d; best = i; }
        }
    return best;
}

/* Apply learned HTP/ALO/WB from the LUT. Returns the learned ISO (>0), or -1
 * when no LUT row applies (caller then uses its hardcoded fallback). WB is
 * applied only when the scene is confidently known. */
static int ai_lut_apply(void)
{
    if (!ai_lut_load()) return -1;

    int light = ai_light_level();
    const char * scene = ai_scene();
    int idx = ai_find(scene, light);
    if (idx < 0) return -1;

    struct ai_lut_row * row = &ai_rows[idx];
    set_htp(row->htp);
    set_alo(row->alo);
    /* LUT WB is green-referenced; normalize by the row's own green (usually
     * 100, but don't assume -- a retrained row with G!=100 would otherwise be
     * silently wrong) and convert to the 1024=neutral gain scale.
     * (Only fires for a known scene; white-point WB owns WB otherwise.) */
    if (!streq(scene, "unknown"))
        lens_set_custom_wb_gains(row->wb_r * AI_WB_NEUTRAL / row->wb_g,
                                 AI_WB_NEUTRAL,
                                 row->wb_b * AI_WB_NEUTRAL / row->wb_g);
    return row->iso;
}

/* --------------------------------------------------------------------------
 * Per-lens tune state, loaded from lens_tune.tbl on lens change. Optional
 * columns 5-7 extend the picstyle row with exposure/WB lens corrections:
 *   lens_id|contrast|saturation|color_tone[|ev_bias_8ths|wb_r_trim|wb_b_trim]
 * ev_bias: added to the ETTR exposure target, in 1/8 EV (negative = protect
 * highlights; e.g. -8 = expose 1 EV lower on this lens). wb trims: multiply
 * the AI white-balance gains, 1024 = 1.0x (suppression scale: r<1024 warms,
 * b>1024 warms). 4-column rows keep the defaults (no exposure/WB change).
 * -------------------------------------------------------------------------- */
static int ai_lens_ev8 = 0;      /* ETTR target bias for this lens, 1/8 EV */
static int ai_lens_wbr = 1024;   /* AI WB red-gain trim, 1024 = 1.0x */
static int ai_lens_wbb = 1024;   /* AI WB blue-gain trim */
static int ai_lens_ca = 0;       /* chromatic aberration correction strength, 0..100 */
static int ai_lens_fr = 0;       /* purple/green fringe reduction strength, 0..100 */

/* where the current values came from: for the AI Lens Tune menu header */
#define AI_LENS_SRC_NONE   0     /* no row -> neutral defaults */
#define AI_LENS_SRC_TABLE  1     /* row from the table (AI-trained/shipped) */
#define AI_LENS_SRC_USER   2     /* row saved from the menu (#user marker) */
static int ai_lens_src = AI_LENS_SRC_NONE;

static void ai_lens_tune_reset(void)
{
    ai_lens_ev8 = 0;
    ai_lens_wbr = 1024;
    ai_lens_wbb = 1024;
    ai_lens_ca = 0;
    ai_lens_fr = 0;
    ai_lens_src = AI_LENS_SRC_NONE;
}


/* --------------------------------------------------------------------------
 * Item 1: White-point white balance. Measure the brightest highlights per RAW
 * channel (95th percentile, black-subtracted) and set custom WB gains so a
 * neutral highlight renders R=G=B -- "pure white at the whitest highlights".
 * Integer-only; the classical white-patch method (mirrors Fixed16bit's auto-WB
 * per-channel white-point step). Affects RAW metadata + JPEG. Needs raw LV.
 * Returns 1 if applied, 0 if raw not ready / no usable highlights.
 * -------------------------------------------------------------------------- */
/* Per-channel raw percentiles: out[0]=P99, out[1]=P95, out[2]=P50 (raw levels,
 * -1 = unavailable). One subsampled metering pass per channel. */
static int ai_channel_stats(int gray_proj, int out[3])
{
    int pcts[3] = {990, 950, 500};
    out[0] = out[1] = out[2] = -1;
    raw_hist_get_percentile_levels(pcts, out, 3,
        gray_proj | GRAY_PROJECTION_DARK_ONLY, 4);
    return (out[0] >= 0 && out[1] >= 0 && out[2] >= 0);
}

/* Confidence (0..256) of the last WB estimate; -1 = none yet. Logged. */
static int ai_wb_conf = -1;

/* Directional-consistency streak for the per-press step clamp (see damping
 * block in ai_white_point_wb). Remembers the SIGN of the last applied gain
 * change per channel and how many consecutive presses pushed the same way.
 * A genuine persistent cast points the same direction every press, so the
 * streak grows and the clamp widens -> it converges in a few presses instead
 * of the geometric half-steps that, in a dim scene, never reach the target
 * within realistic 1-2 half-press usage (field case: FileNum 5503, dim green
 * room, gains stuck near neutral at WbConf 82). A noisy/random read flips
 * direction, resetting the streak to the conservative base step -- preserving
 * the noise safety this file's history was built around. */
static int ai_wb_r_sign = 0, ai_wb_r_streak = 0;
static int ai_wb_b_sign = 0, ai_wb_b_streak = 0;

/* White-point WB v2 -- confidence-clipped bright-pixels estimator.
 *
 * Literature-backed design (Shades-of-Gray, Finlayson/Trezzi; Bright Pixels,
 * Joze/Drew CIC'12; near-white AWB; temporally damped camera AWB):
 *  - White anchor = P95 per channel (a bright POPULATION, not the max --
 *    robust to specular/clipped pixels, unlike classic white-patch).
 *  - Gray anchor = P50 (gray-world on medians) as the robust fallback.
 *  - CLAHE-like clip on estimator influence: trust in the white anchor scales
 *    with how bright the highlight really is. A dim "brightest highlight"
 *    (the reported failure case) gets low confidence and is NOT forced to
 *    pure white; the estimate leans on the gray anchor / current WB instead.
 *  - Clipped highlights (P99 at saturation) halve confidence: clamped
 *    channels lie about the ratio.
 *  - Temporal damping + per-press step clip: gains glide toward the estimate
 *    (halfway per half-press, max +/-160/press) instead of jumping.
 * Integer-only. Gains are AsShotNeutral-style (1024 = channel/green ratio). */
static int ai_white_point_wb(int warmth)   /* warmth: 0=neutral .. 4=warmest */
{
    int r[3], g[3], b[3];
    if (!ai_channel_stats(GRAY_PROJECTION_RED, r))   return 0;
    if (!ai_channel_stats(GRAY_PROJECTION_GREEN, g)) return 0;
    if (!ai_channel_stats(GRAY_PROJECTION_BLUE, b))  return 0;

    int black = raw_info.black_level;
    int span = raw_info.white_level - black;
    if (span < 8) return 0;   /* also guards span/2 - span/8 == 0 below */
    for (int i = 0; i < 3; i++)
    {
        r[i] -= black; g[i] -= black; b[i] -= black;
        if (r[i] < 0) r[i] = 0;
        if (g[i] < 0) g[i] = 0;
        if (b[i] < 0) b[i] = 0;
    }
    if (g[1] < 1) return 0;   /* no usable signal at all */

    /* current gains = the "do nothing" anchor */
    int cur_r = AI_WB_NEUTRAL, cur_b = AI_WB_NEUTRAL;
    if (lens_info.wb_mode == WB_CUSTOM && lens_info.WBGain_R && lens_info.WBGain_B)
    {
        cur_r = (int) lens_info.WBGain_R;
        cur_b = (int) lens_info.WBGain_B;
    }

    /* confidence in the bright anchor: 0 at <= span/8, full at >= span/2 */
    int conf = (g[1] - span / 8) * 256 / (span / 2 - span / 8);
    conf = COERCE(conf, 0, 256);
    /* clipped highlights lie about channel ratios -> halve the trust */
    if (g[0] >= span * 97 / 100 || r[0] >= span * 97 / 100 || b[0] >= span * 97 / 100)
        conf = conf / 2;

    /* white-patch estimate at the bright anchor (P95) */
    int r_hi = AI_WB_NEUTRAL * r[1] / g[1];
    int b_hi = AI_WB_NEUTRAL * b[1] / g[1];

    /* Gray-world estimate at the medians -- CLAHE-style clip on shadow
     * influence. Channel ratios computed near the noise floor are pure noise
     * (a real field failure: RawMed=6, RawR=3, RawB=3 -> r/g=0.5 -> gains
     * slammed blue). Weight the gray anchor by how far the green median sits
     * above the noise floor: 0 at <= span/64 (dark: the anchor is DISABLED,
     * WB cannot drift), full only at >= span/8 (solid midtones). */
    int wsh = (g[2] - span / 64) * 256 / (span / 8 - span / 64);
    wsh = COERCE(wsh, 0, 256);
    if (r[2] < 1 || b[2] < 1) wsh = 0;   /* channel medians unusable */

    /* Daylight prior (this sensor's daylight AsShotNeutral x1024, chdk-dng.c:
     * r 0.4736 b 0.624). As the shadows clip the gray anchor to zero, fade to
     * DAYLIGHT -- not to "keep current": keeping the current gains preserved
     * whatever cast they carried (field case: gains cooler than daylight,
     * frozen by dark scenes -> permanent blue in the dark-mid JPEG bands).
     * Total black now glides WB to sane daylight instead. */
    int r_md = AI_WB_DAY_R, b_md = AI_WB_DAY_B;
    if (wsh > 0)
    {
        int r_gw = AI_WB_NEUTRAL * r[2] / g[2];
        int b_gw = AI_WB_NEUTRAL * b[2] / g[2];
        r_md = (r_gw * wsh + AI_WB_DAY_R * (256 - wsh)) / 256;
        b_md = (b_gw * wsh + AI_WB_DAY_B * (256 - wsh)) / 256;
    }

    /* confidence-clipped blend (scene-adaptive Shades-of-Gray) */
    int r_est = (r_hi * conf + r_md * (256 - conf)) / 256;
    int b_est = (b_hi * conf + b_md * (256 - conf)) / 256;

    /* per-lens color-cast trim (lens_tune.tbl); damping/clamps below still rule */
    r_est = r_est * ai_lens_wbr / 1024;
    b_est = b_est * ai_lens_wbb / 1024;

    /* global green-cast compensation -- boost R/B toward Canon's measured
     * neutral (see AI_WB_GREENCOMP_* above) to kill the systematic yellow-green */
    r_est = r_est * AI_WB_GREENCOMP_R / 100;
    b_est = b_est * AI_WB_GREENCOMP_B / 100;

    /* temporal damping: glide halfway toward the estimate, step-clipped.
     * The clamp widens when successive presses keep pointing the SAME way
     * (a genuine, persistent cast) and stays at the conservative base when
     * the direction flips (noise). base 160, +160 per consecutive
     * same-direction press, capped at 480 (~3 presses to full authority).
     * NOTE: the confidence-scaled "converge fast" variant was reverted
     * 2026-07-25 -- field samples showed the estimate itself drifting green,
     * so converging to it faster is the wrong fix. This build is instrumented
     * (see below) to capture WHY the estimate is wrong before changing it. */
    int r_sign = (r_est > cur_r) - (r_est < cur_r);
    int b_sign = (b_est > cur_b) - (b_est < cur_b);
    if (r_sign != 0 && r_sign == ai_wb_r_sign) ai_wb_r_streak++;
    else { ai_wb_r_sign = r_sign; ai_wb_r_streak = 1; }
    if (b_sign != 0 && b_sign == ai_wb_b_sign) ai_wb_b_streak++;
    else { ai_wb_b_sign = b_sign; ai_wb_b_streak = 1; }
    int r_step = COERCE(160 * ai_wb_r_streak, 160, 480);
    int b_step = COERCE(160 * ai_wb_b_streak, 160, 480);

    int new_r = cur_r + (r_est - cur_r) / 2;
    int new_b = cur_b + (b_est - cur_b) / 2;
    new_r = COERCE(new_r, cur_r - r_step, cur_r + r_step);
    new_b = COERCE(new_b, cur_b - b_step, cur_b + b_step);
    new_r = COERCE(new_r, 256, 1536);
    new_b = COERCE(new_b, 256, 1536);

    ai_wb_conf = conf;

    /* --- WB estimator instrumentation (TEMPORARY diagnostic, 2026-07-25) ---
     * Dumps the full internal estimator state every time AI WB runs, from ANY
     * path (LiveView poll OR OVF/QR review), independent of ai_lut_log()'s
     * gating (which wasn't firing for OVF shots). One appended line per run to
     * ML/logs/aiwb_dbg.txt. This is how we find out WHY the estimate lands
     * green/high on real shots (green kitchen etc.). Remove once diagnosed. */
    {
        FIO_CreateDirectory("ML/logs");
        /* FIO_CreateFileOrAppend: the correct ML idiom (FIO_OpenFile with
         * O_CREAT does NOT create a missing file in ML's FIO -- that silently
         * produced no log the first time). Matches ai_lut_log() below. */
        FILE * _df = FIO_CreateFileOrAppend("ML/logs/aiwb_dbg.txt");
        if (_df)
        {
            char _dl[256];
            int _dn = snprintf(_dl, sizeof(_dl),
                "P95 r=%d g=%d b=%d|P50 r=%d g=%d b=%d|conf=%d wsh=%d|"
                "rhi=%d bhi=%d rmd=%d bmd=%d|rest=%d best=%d|cur=%d/%d new=%d/%d\n",
                r[1], g[1], b[1], r[2], g[2], b[2], conf, wsh,
                r_hi, b_hi, r_md, b_md, r_est, b_est, cur_r, cur_b, new_r, new_b);
            if (_dn > 0) FIO_WriteFile(_df, _dl, _dn);
            FIO_CloseFile(_df);
        }
    }

    if (new_r != cur_r || new_b != cur_b)
        lens_set_custom_wb_gains(new_r, AI_WB_NEUTRAL, new_b);

    /* Warmth rides Canon's own WB SHIFT (B/A axis), not the gains: the gains
     * above are the neutral measurement ("white priority" science); the shift
     * is the ambience ("taste"), exactly how Canon separates the two.
     *   base:  the "AI WB Warm/Cool" menu (user's master). It is a menu INDEX
     *          0..8 that maps to a SIGNED Canon Amber/Blue step, base = index-4
     *          (0=B4 cool .. 4=Neutral .. 8=A4 warm). 1 step ~= 5 mireds.
     *   keep:  a SMALL amber nudge (<=+1) only for genuinely warm light, so
     *          golden light keeps a little character -- applied on top of the
     *          user's base, so it never drags a cool choice warm by more than
     *          one step, and never fights a neutral choice hard.
     *
     * FIELD FIX 2026-07-25: the old auto-amber added up to +4 whenever the
     * highlight was merely warmer than daylight -- tripping on all warm indoor
     * light the user wants neutralized (WB Shift AB pinned +4, visibly too
     * warm). Capped to +1 and gated to clearly warm light (warm_ratio > 1100).
     * 2026-07-26: warmth control made bidirectional (cool B-steps added) and
     * the base defaulted to Neutral. Daylight ref r_hi*1024/b_hi ~777
     * (chdk-dng.c AsShotNeutral r/b 0.4736/0.624). */
    int wbs = (int) warmth - 4;   /* menu index 0..8 -> signed A/B step -4..+4 */
    if (conf >= 64 && b_hi > 0)
    {
        int warm_ratio = r_hi * 1024 / b_hi;
        int auto_amber = COERCE((warm_ratio - 1100) * 4 / 777, 0, 1);
        wbs += auto_amber;        /* golden-light keep: at most +1 amber */
    }
    wbs = COERCE(wbs, -9, 9);     /* full Canon WB Shift A/B range */
    if (wbs != lens_info.wbs_ba)
        lens_set_wbs_ba(wbs);

    /* Green-Magenta cast correction (Canon WBS_GM axis) -- REVERTED.
     *
     * Tried: driving lens_set_wbs_gm() from the product r_hi*b_hi, on the
     * theory that log(r_hi/g)+log(b_hi/g) (~ log(r_hi*b_hi)) is roughly
     * invariant along the temperature axis, so testing it against a single
     * daylight-anchored reference product would isolate a genuine green/
     * magenta cast from ordinary color-temperature variation.
     *
     * FIELD-DISPROVEN 2026-07-21: real test shot IMG_5495.CR2, scene at
     * ColorTemperature 4100K (warm), came back with WB Shift GM = +4
     * (Canon: positive = toward Green) and the JPEG is severely,
     * overwhelmingly green -- this code pushed the WRONG direction on an
     * already-green scene. Root cause: the single-anchor "product stays
     * constant" assumption only holds near daylight; at 4100K the true
     * r_hi*b_hi product is legitimately far from the daylight reference for
     * ordinary color-temperature reasons having nothing to do with a real
     * G/M cast, so the heuristic misread normal warm-light behavior as
     * "magenta cast" and pushed a compensating (harmful) green shift.
     *
     * Do NOT re-enable this shape of fix without either (a) multiple
     * calibration anchors spanning tungsten..daylight..shade (not just one),
     * or (b) a fundamentally different signal that doesn't conflate
     * temperature with tint. See [[ml6d-ai-wb-picture-tune]] memory for the
     * history of every other WB fix in this file being field-log-driven --
     * this one skipped that step and broke on the first real photo.
     *
     * NOTE: this may have left lens_info.wbs_gm sitting at a nonzero value
     * on-camera (Canon's own property, independent of this firmware) --
     * check/reset WB Shift in Canon's menu if photos still look off after
     * this revert. */

    return 1;
}

/* --------------------------------------------------------------------------
 * Item 2: Per-lens picture tune. Read ML/models/lens_tune.tbl and set Canon
 * picture-style contrast/saturation/color-tone for the current lens, so
 * different lenses render with uniform contrast/saturation.
 * Format: lens_id|contrast|saturation|color_tone[|ev_bias|wb_r|wb_b]
 * (picstyle values -4..4; lens_id 0 = default fallback; optional columns
 * feed ai_lens_ev8 / ai_lens_wbr / ai_lens_wbb -- see above). Picstyle is
 * JPEG-only; ev_bias steers the metered ETTR target (RAW exposure); wb trims
 * steer the AI white balance (RAW as-shot + JPEG).
 * -------------------------------------------------------------------------- */
static int ai_lens_c = 0, ai_lens_s = 0, ai_lens_t = 0;

/* Parse lens_tune.tbl for the mounted lens (exact id, else default row 0).
 * Always resets to neutral first, so a missing file/row means "no change".
 * Returns 1 if a row matched. */
static int ai_lens_tune_load(void)
{
    static char buf[2048];
    ai_lens_tune_reset();
    ai_lens_c = ai_lens_s = ai_lens_t = 0;

    int rc = read_file(AI_LENS_TUNE_PATH, buf, (int) sizeof(buf) - 1);
    if (rc <= 0) return 0;
    if (rc > (int) sizeof(buf) - 1) rc = sizeof(buf) - 1;
    buf[rc] = 0;

    int cur = (int) lens_info.lens_id;
    /* row = {contrast, saturation, tone, ev8, wbr, wbb, ca_strength, fringe_reduce} */
    int row[8], def[8], found = 0, have_def = 0;

    char * p = buf;
    while (*p && !found)
    {
        char * nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') nl++;
        char saved = *nl;
        *nl = 0;
        if (*p != '#' && strchr(p, '|'))
        {
            char * f[9];
            int nf = ai_split(p, f, 9);
            if (nf >= 4)
            {
                int v[8];
                v[0] = COERCE(atoi(f[1]), -4, 4);
                v[1] = COERCE(atoi(f[2]), -4, 4);
                v[2] = COERCE(atoi(f[3]), -4, 4);
                v[3] = nf >= 5 ? COERCE(atoi(f[4]), -24, 8)    : 0;
                v[4] = nf >= 6 ? COERCE(atoi(f[5]), 512, 2048) : 1024;
                v[5] = nf >= 7 ? COERCE(atoi(f[6]), 512, 2048) : 1024;
                v[6] = nf >= 8 ? COERCE(atoi(f[7]), 0, 100)    : 0;
                v[7] = nf >= 9 ? COERCE(atoi(f[8]), 0, 100)    : 0;
                int id = atoi(f[0]);
                if (id == cur)
                {
                    memcpy(row, v, sizeof(row)); found = 1;
                    /* menu-saved rows carry a "#user" marker after the fields */
                    ai_lens_src = strstr(f[nf-1], "#user") ? AI_LENS_SRC_USER
                                                           : AI_LENS_SRC_TABLE;
                }
                else if (id == 0){ memcpy(def, v, sizeof(def)); have_def = 1; }
            }
        }
        *nl = saved;
        p = nl;
        while (*p == '\n' || *p == '\r') p++;
    }
    if (!found && have_def)
    {
        memcpy(row, def, sizeof(row));
        found = 1;
        ai_lens_src = AI_LENS_SRC_TABLE;   /* generic default row */
    }
    if (!found) return 0;

    ai_lens_c = row[0]; ai_lens_s = row[1]; ai_lens_t = row[2];
    ai_lens_ev8 = row[3]; ai_lens_wbr = row[4]; ai_lens_wbb = row[5];
    ai_lens_ca = row[6]; ai_lens_fr = row[7];
    return 1;
}

/* Write the current tune values back to lens_tune.tbl as the row for the
 * mounted lens (replacing any existing row for that id), marked "#user" so
 * offline training knows to preserve it. Returns 1 on success. */
static int ai_lens_tune_save(void)
{
    static char buf[2048];
    static char out[2048];
    int cur = (int) lens_info.lens_id;

    char row[96];
    snprintf(row, sizeof(row), "%d|%d|%d|%d|%d|%d|%d|%d|%d  #user\n",
             cur, ai_lens_c, ai_lens_s, ai_lens_t,
             ai_lens_ev8, ai_lens_wbr, ai_lens_wbb,
             ai_lens_ca, ai_lens_fr);
    int rl = strlen(row);

    int rc = read_file(AI_LENS_TUNE_PATH, buf, (int) sizeof(buf) - 1);
    if (rc < 0) rc = 0;                    /* no file yet -> create fresh */
    if (rc > (int) sizeof(buf) - 1) rc = sizeof(buf) - 1;
    buf[rc] = 0;

    /* copy every line except an existing row for this lens id */
    int on = 0;
    char * p = buf;
    while (*p)
    {
        char * nl = p;
        while (*nl && *nl != '\n') nl++;
        int ll = nl - p + (*nl == '\n' ? 1 : 0);
        int skip = 0;
        int has_pipe = 0;
        for (char * q = p; q < nl; q++) if (*q == '|') { has_pipe = 1; break; }
        if (*p != '#' && has_pipe && atoi(p) == cur)
            skip = 1;                      /* replaced by the new row below */
        if (!skip)
        {
            if (on + ll >= (int) sizeof(out) - rl - 1) return 0;  /* too big */
            memcpy(out + on, p, ll);
            on += ll;
        }
        p = nl + (*nl == '\n' ? 1 : 0);
    }
    if (on > 0 && out[on-1] != '\n') out[on++] = '\n';
    memcpy(out + on, row, rl);
    on += rl;

    /* FIO_CreateFile returns NULL on this firmware/card; use the working
     * FIO_CreateFileOrAppend after RemoveFile so the on-camera "Save for this
     * lens" writes a fresh table instead of silently failing (or appending
     * onto the old one). Note: lens_tune.tbl lives in ML/models (writable),
     * NOT ML/DATA -- so this save works, unlike the removed ML/DATA writers. */
    FIO_RemoveFile(AI_LENS_TUNE_PATH);
    FILE * f = FIO_CreateFileOrAppend(AI_LENS_TUNE_PATH);
    if (!f) return 0;
    FIO_WriteFile(f, out, on);
    FIO_CloseFile(f);

    ai_lens_src = AI_LENS_SRC_USER;
    return 1;
}

static void ai_picture_tune(void)
{
    if (!ai_lens_tune_load()) return;
    picstyle_set_current_contrast(ai_lens_c);
    picstyle_set_current_saturation(ai_lens_s);
    picstyle_set_current_color_tone(ai_lens_t);
}

/* --------------------------------------------------------------------------
 * Firmware-side data logging (replaces the non-viable Lua logger: ML Lua has no
 * histogram access). Appends one key=value record to ML/logs/unified_log.txt on
 * each half-shutter press. LightLevel comes from the real ML histogram; camera
 * state from lens_info. Format matches the offline trainer's parser.
 * -------------------------------------------------------------------------- */

#define AI_LOG_PATH "ML/logs/unified_log.txt"

/* Append one record. Returns 1 if logged, 0 if no light source was ready yet
 * (caller retries next poll -- raw metering and the display histogram can both
 * lag the half-press rising edge). */
static int ai_lut_log(void)
{
    int light = ai_light_level();

    /* display-histogram total (fallback source readiness + optional Hist dump) */
    uint32_t * h = histogram.is_rgb ? histogram.hist_g : histogram.hist;
    uint32_t disp_total = 0;
    for (int i = 0; i < HIST_WIDTH; i++) disp_total += h[i];

    /* nothing metered yet -> don't write a junk record */
    if (!ai_light_src && disp_total == 0) return 0;

    const char * scene = ai_scene();
    /* In Auto ISO, raw_iso is 0; use the resolved auto value so ISO (the
     * training target) is real, not 0. */
    int riso = lens_info.raw_iso;
    if (riso == 0) riso = lens_info.raw_iso_auto;
    int iso = raw2iso(riso);
    int shutter_ms = raw2shutter_ms(lens_info.raw_shutter);
    int ts = get_seconds_clock();

    /* raw R/B medians for offline WB/scene learning (raw path only) */
    int r_med = -1, b_med = -1;
    if (ai_light_src) ai_sample_rb(&r_med, &b_med);

    int wr = 100, wg = 100, wbb = 100;
    if (lens_info.wb_mode == WB_CUSTOM)
    {
        wr  = (int) lens_info.WBGain_R;
        wg  = (int) lens_info.WBGain_G;
        wbb = (int) lens_info.WBGain_B;
    }

    /* Current file number via the core-exported card accessor (the raw
     * file_number global is not exported to modules). */
    struct card_info * card = get_shooting_card();
    int fnum = card ? card->file_number : -1;

    FILE * f = FIO_CreateFileOrAppend(AI_LOG_PATH);
    if (!f) return 0;

    char line[320];
    snprintf(line, sizeof(line), "Timestamp=%d\nHist=", ts);
    FIO_WriteFile(f, line, strlen(line));

    /* display-histogram bins (labeled by HistSrc=disp; LightLevel itself comes
     * from LightSrc). Static: keep 1 KB off the shoot-task stack. */
    static char hbuf[1024];
    int hn = 0;
    if (disp_total > 0)
    {
        for (int i = 0; i < HIST_WIDTH; i++)
        {
            char tmp[12];
            /* ML's snprintf has no %u; use %d (bin counts fit in int). */
            snprintf(tmp, sizeof(tmp), i ? ",%d" : "%d", (int) h[i]);
            int tl = strlen(tmp);
            if (hn + tl < (int) sizeof(hbuf) - 1)
            {
                memcpy(hbuf + hn, tmp, tl);
                hn += tl;
            }
        }
    }
    if (hn > 0) FIO_WriteFile(f, hbuf, hn);
    else        FIO_WriteFile(f, "nil", 3);

    /* FileNum = current camera file number. The CR2 produced by fully pressing
     * after this half-press is FileNum+1, so validation can pair each log record
     * to its exact RAW (see tools/validate_exposure.py).
     * RawMed/RawP99 = green median / clip proximity; RawR/RawB = channel medians
     * (all 0..255, -1 = n/a) -- features for offline WB/scene/ISO learning. */
    snprintf(line, sizeof(line),
        "\nHistSrc=disp\nScene=%s\nLightLevel=%d\nLightSrc=%s"
        "\nRawMed=%d\nRawP99=%d\nRawR=%d\nRawB=%d\nWbConf=%d"
        "\nShutter=%dms\nISO=%d\nWB=R%d,G%d,B%d\nWbs=%d\nWbsGm=%d\nLens=%d\nFileNum=%d\n---\n",
        scene, light, (ai_light_src ? "raw" : "disp"),
        ai_raw_med, ai_raw_p99, r_med, b_med, ai_wb_conf,
        shutter_ms, iso, wr, wg, wbb, (int) lens_info.wbs_ba, (int) lens_info.wbs_gm,
        (int) lens_info.lens_id, fnum);
    FIO_WriteFile(f, line, strlen(line));

    FIO_CloseFile(f);
    return 1;
}

#endif /* _ai_lut_h_ */
