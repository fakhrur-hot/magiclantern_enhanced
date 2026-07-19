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

/* picstyle setters are declared core-only (FEATURE_PICSTYLE) in picstyle.h, but
 * the symbols ARE exported to modules (present in the core .sym) -- declare the
 * prototypes ourselves so we can drive Canon's picture style from the module. */
extern void picstyle_set_current_contrast(int value);
extern void picstyle_set_current_saturation(int value);
extern void picstyle_set_current_color_tone(int value);

#define AI_LUT_PATH    "ML/models/unified.tbl"
#define AI_LENS_TUNE_PATH "ML/models/lens_tune.tbl"
#define AI_WB_NEUTRAL  1024   /* lens_set_custom_wb_gains scale: 1024 = 1.0x */
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

    /* gray-world estimate at the medians; if the scene is too dark for a
     * meaningful median, fall back to "keep current" */
    int r_md = cur_r, b_md = cur_b;
    if (g[2] >= 8 && r[2] >= 1 && b[2] >= 1)
    {
        r_md = AI_WB_NEUTRAL * r[2] / g[2];
        b_md = AI_WB_NEUTRAL * b[2] / g[2];
    }

    /* confidence-clipped blend (scene-adaptive Shades-of-Gray) */
    int r_est = (r_hi * conf + r_md * (256 - conf)) / 256;
    int b_est = (b_hi * conf + b_md * (256 - conf)) / 256;

    /* per-lens color-cast trim (lens_tune.tbl); damping/clamps below still rule */
    r_est = r_est * ai_lens_wbr / 1024;
    b_est = b_est * ai_lens_wbb / 1024;

    /* global warmth bias (menu "AI WB Warmth"): pure white-point AWB is Canon's
     * "White priority" -- technically neutral, but it strips the ambience and
     * reads cold, especially on skin. Bias the target amber like Canon's
     * default "Ambience priority" + WB A-shift: ~2.5% per step (suppression
     * scale: lower R gain and higher B gain = warmer). */
    r_est = r_est * (1024 - 26 * warmth) / 1024;
    b_est = b_est * (1024 + 26 * warmth) / 1024;

    /* temporal damping: glide halfway toward the estimate, step-clipped */
    int new_r = cur_r + (r_est - cur_r) / 2;
    int new_b = cur_b + (b_est - cur_b) / 2;
    new_r = COERCE(new_r, cur_r - 160, cur_r + 160);
    new_b = COERCE(new_b, cur_b - 160, cur_b + 160);
    new_r = COERCE(new_r, 256, 1536);
    new_b = COERCE(new_b, 256, 1536);

    ai_wb_conf = conf;
    if (new_r != cur_r || new_b != cur_b)
        lens_set_custom_wb_gains(new_r, AI_WB_NEUTRAL, new_b);
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
    /* row = {contrast, saturation, tone, ev8, wbr, wbb} */
    int row[6], def[6], found = 0, have_def = 0;

    char * p = buf;
    while (*p && !found)
    {
        char * nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') nl++;
        char saved = *nl;
        *nl = 0;
        if (*p != '#' && strchr(p, '|'))
        {
            char * f[7];
            int nf = ai_split(p, f, 7);
            if (nf >= 4)
            {
                int v[6];
                v[0] = COERCE(atoi(f[1]), -4, 4);
                v[1] = COERCE(atoi(f[2]), -4, 4);
                v[2] = COERCE(atoi(f[3]), -4, 4);
                v[3] = nf >= 5 ? COERCE(atoi(f[4]), -24, 8)    : 0;
                v[4] = nf >= 6 ? COERCE(atoi(f[5]), 512, 2048) : 1024;
                v[5] = nf >= 7 ? COERCE(atoi(f[6]), 512, 2048) : 1024;
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

    char row[80];
    snprintf(row, sizeof(row), "%d|%d|%d|%d|%d|%d|%d  #user\n",
             cur, ai_lens_c, ai_lens_s, ai_lens_t,
             ai_lens_ev8, ai_lens_wbr, ai_lens_wbb);
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

    FILE * f = FIO_CreateFile(AI_LENS_TUNE_PATH);
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
        "\nShutter=%dms\nISO=%d\nWB=R%d,G%d,B%d\nLens=%d\nFileNum=%d\n---\n",
        scene, light, (ai_light_src ? "raw" : "disp"),
        ai_raw_med, ai_raw_p99, r_med, b_med, ai_wb_conf,
        shutter_ms, iso, wr, wg, wbb, (int) lens_info.lens_id, fnum);
    FIO_WriteFile(f, line, strlen(line));

    FIO_CloseFile(f);
    return 1;
}

#endif /* _ai_lut_h_ */
