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
    /* LUT WB is G=100 reference; convert to the 1024=neutral gain scale.
     * (Only fires for a known scene; white-point WB owns WB otherwise.) */
    if (!streq(scene, "unknown"))
        lens_set_custom_wb_gains(row->wb_r * AI_WB_NEUTRAL / 100,
                                 AI_WB_NEUTRAL,
                                 row->wb_b * AI_WB_NEUTRAL / 100);
    return row->iso;
}

/* --------------------------------------------------------------------------
 * Item 1: White-point white balance. Measure the brightest highlights per RAW
 * channel (95th percentile, black-subtracted) and set custom WB gains so a
 * neutral highlight renders R=G=B -- "pure white at the whitest highlights".
 * Integer-only; the classical white-patch method (mirrors Fixed16bit's auto-WB
 * per-channel white-point step). Affects RAW metadata + JPEG. Needs raw LV.
 * Returns 1 if applied, 0 if raw not ready / no usable highlights.
 * -------------------------------------------------------------------------- */
static int ai_channel_hi(int gray_proj)
{
    int pct = 950, v = -1;   /* 95th percentile */
    raw_hist_get_percentile_levels(&pct, &v, 1,
        gray_proj | GRAY_PROJECTION_DARK_ONLY, 4);
    return v;   /* raw level, or -1 if unavailable */
}

static int ai_white_point_wb(void)
{
    int r_hi = ai_channel_hi(GRAY_PROJECTION_RED);
    int g_hi = ai_channel_hi(GRAY_PROJECTION_GREEN);
    int b_hi = ai_channel_hi(GRAY_PROJECTION_BLUE);
    if (r_hi < 0 || g_hi < 0 || b_hi < 0) return 0;   /* raw not ready */

    int black = raw_info.black_level;
    int span = raw_info.white_level - black;
    r_hi -= black; g_hi -= black; b_hi -= black;
    if (r_hi < 1 || g_hi < 1 || b_hi < 1) return 0;
    if (span <= 0 || g_hi < span / 8) return 0;       /* no real highlights */

    /* Canon WBGain is AsShotNeutral-style (1024 = neutral): gain = raw_channel /
     * raw_green at the white point, and the pipeline DIVIDES by it. So R and B
     * gains are the channel-to-green RATIO (typically < 1024 -- daylight R~0.47,
     * B~0.62). A neutral highlight then renders R=G=B. (Getting this inverted
     * suppressed R/B and gave a green cast.) Clamp to a sane WB range. */
    int r_gain = AI_WB_NEUTRAL * r_hi / g_hi;
    int b_gain = AI_WB_NEUTRAL * b_hi / g_hi;
    r_gain = COERCE(r_gain, 256, 1536);   /* ~0.25x .. 1.5x */
    b_gain = COERCE(b_gain, 256, 1536);

    lens_set_custom_wb_gains(r_gain, AI_WB_NEUTRAL, b_gain);
    return 1;
}

/* --------------------------------------------------------------------------
 * Item 2: Per-lens picture tune. Read ML/models/lens_tune.tbl and set Canon
 * picture-style contrast/saturation/color-tone for the current lens, so
 * different lenses render with uniform contrast/saturation.
 * Format: lens_id|contrast|saturation|color_tone   (each -4..4; lens_id 0 =
 * default fallback). JPEG-only (picstyle does not affect RAW).
 * -------------------------------------------------------------------------- */
static void ai_picture_tune(void)
{
    static char buf[2048];
    int rc = read_file(AI_LENS_TUNE_PATH, buf, (int) sizeof(buf) - 1);
    if (rc <= 0) return;
    if (rc > (int) sizeof(buf) - 1) rc = sizeof(buf) - 1;
    buf[rc] = 0;

    int cur = (int) lens_info.lens_id;
    int c = 0, s = 0, t = 0, found = 0;
    int dc = 0, ds = 0, dt = 0, have_def = 0;

    char * p = buf;
    while (*p && !found)
    {
        char * nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') nl++;
        char saved = *nl;
        *nl = 0;
        if (*p != '#' && strchr(p, '|'))
        {
            char * f[4];
            if (ai_split(p, f, 4) >= 4)
            {
                int id = atoi(f[0]);
                int cc = COERCE(atoi(f[1]), -4, 4);
                int ss = COERCE(atoi(f[2]), -4, 4);
                int tt = COERCE(atoi(f[3]), -4, 4);
                if (id == cur)   { c = cc; s = ss; t = tt; found = 1; }
                else if (id == 0){ dc = cc; ds = ss; dt = tt; have_def = 1; }
            }
        }
        *nl = saved;
        p = nl;
        while (*p == '\n' || *p == '\r') p++;
    }
    if (!found && have_def) { c = dc; s = ds; t = dt; found = 1; }
    if (!found) return;

    picstyle_set_current_contrast(c);
    picstyle_set_current_saturation(s);
    picstyle_set_current_color_tone(t);
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
        "\nRawMed=%d\nRawP99=%d\nRawR=%d\nRawB=%d"
        "\nShutter=%dms\nISO=%d\nWB=R%d,G%d,B%d\nFileNum=%d\n---\n",
        scene, light, (ai_light_src ? "raw" : "disp"),
        ai_raw_med, ai_raw_p99, r_med, b_med,
        shutter_ms, iso, wr, wg, wbb, fnum);
    FIO_WriteFile(f, line, strlen(line));

    FIO_CloseFile(f);
    return 1;
}

#endif /* _ai_lut_h_ */
