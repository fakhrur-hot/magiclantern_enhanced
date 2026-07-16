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

#define AI_LUT_PATH    "ML/models/unified.tbl"
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

/* ISO -> ML raw iso: 100->72, 200->80, 400->88, 800->96, 1600->104, 3200->112 */
static int ai_iso_to_raw(int iso)
{
    int stops = 0, v = 100;
    while (v < iso && stops < 10) { v *= 2; stops++; }
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
                struct ai_lut_row * row = &ai_rows[ai_nrows];
                snprintf(row->scene, sizeof(row->scene), "%s", f[0]);
                row->light = atoi(f[1]);
                row->alo   = ai_alo_from_kw(f[3]);
                row->htp   = streq(f[4], "priority_on") ? 1 : 0;
                row->iso   = atoi(f[5]);
                row->wb_r = r; row->wb_g = g; row->wb_b = b;
                ai_nrows++;
            }
        }

        *nl = saved;
        p = nl;
        while (*p == '\n' || *p == '\r') p++;
    }
    return ai_nrows;
}

/* Integer 90th-percentile bin of the green channel, scaled to 0..255
 * (REQ-001 definition). 128 fallback when the histogram has no data. */
static int ai_light_level(void)
{
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
    if (!streq(scene, "unknown"))
        lens_set_custom_wb_gains(row->wb_r, row->wb_g, row->wb_b);
    return row->iso;
}

#endif /* _ai_lut_h_ */
