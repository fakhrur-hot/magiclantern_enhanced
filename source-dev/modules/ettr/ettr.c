/**
 * Auto ETTR (Expose To The Right).
 * 
 * Optimize the exposure for raw shooting (photo + video).
 */

#include <module.h>
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <menu.h>
#include <config.h>
#include <raw.h>
#include <lens.h>
#include <math.h>
#include <zebra.h>
#include <shoot.h>
#include <fps.h>
#include <focus.h>
#include <beep.h>
#include <histogram.h>
#include <console.h>
#include <picstyle.h>

/* interface with dual ISO */
#include "../dual_iso/dual_iso.h"

#include "ai_lut.h"   /* AI-LUT: learned per-scene ISO/WB/ALO/HTP for the optimizer */

/* Video gyro/MLV-metadata subsystem (external IMU over hot-shoe serial +
 * electronic-level + per-frame MLV blocks + mlc.gyro/level Lua bindings).
 * DISABLED 2026-07-23 after a real field regression: gyro_bridge_poll() runs
 * every LiveView frame from auto_ettr_vsync_cbr and reads the hot-shoe serial
 * pins; with a flash mounted on the hot shoe (field case: camera.flash=true)
 * that repeated probe is a plausible fault/hang source. Symptom on that card:
 * ML core + lua.mo loaded fine, but ettr's runtime never ran (no sidecar, no
 * unified_log, no ml_export update, shots stuck on Canon Auto WB) — consistent
 * with a crash that tripped ML's "disable module auto-load after a crash"
 * safety. The SAME binary had worked the day before, the only difference being
 * a flash now on the hot shoe. This subsystem is also the sole reason ettr.mo
 * hard-imports lua.mo's lua_* symbols, coupling core ETTR's load fate to
 * lua.mo. Compiling it out (a) removes the per-frame hot-shoe probe, (b) makes
 * ettr.mo self-contained again (no cross-module lua dependency), restoring the
 * known-good module shape. The feature was unvalidated and produced no MLV
 * output in practice. gyro_bridge.o / mlv_metadata.o are also dropped from the
 * Makefile link so their lua_* imports don't re-enter ettr.mo. Re-enable only
 * after the hot-shoe-vs-flash conflict is resolved and validated on hardware. */
#define ETTR_VIDEO_GYRO_METADATA 0

#if ETTR_VIDEO_GYRO_METADATA
#include "gyro_bridge.h"  /* External IMU + electronic level for video metadata */
#include "mlv_metadata.h" /* MLV chunk callback for gyro/level in .MLV recordings */
#endif

CONFIG_INT("auto.ettr", auto_ettr, 0);
static CONFIG_INT("auto.ettr.trigger", auto_ettr_trigger, 3);
static CONFIG_INT("auto.ettr.ignore", auto_ettr_ignore, 1);
/* Default ETTR exposure target: 0 => the aggressive -0.5 EV (firmware clamps the
 * target to MIN(level, -0.5); -0.5 is the closest-to-clip ML allows -- its
 * metering-safety margin -- so it's the practical "-1/3 EV, highlights just under
 * clip" aesthetic). Still user-adjustable: Expo > Auto ETTR > Exposure target
 * (-4/-3/-2/-1/-0.5 EV). Was -1 (a full stop below clip, too timid). */
static CONFIG_INT("auto.ettr.level", auto_ettr_target_level, 0);
static CONFIG_INT("auto.ettr.max.tv", auto_ettr_max_shutter, 88);
static CONFIG_INT("auto.ettr.clip", auto_ettr_clip, 0);
static CONFIG_INT("auto.ettr.mode", auto_ettr_adjust_mode, 0);
static CONFIG_INT("auto.ettr.midtone.snr", auto_ettr_midtone_snr_limit, 6);
static CONFIG_INT("auto.ettr.shadow.snr", auto_ettr_shadow_snr_limit, 2);
static CONFIG_INT("auto.ettr.dual.iso", auto_ettr_dual_iso_link, 1);
static CONFIG_INT("auto.ettr.allow.beeps", auto_ettr_allow_beeps, 1);
/* When ISO is set to Auto, take over with ETTR + HTP + ALO (default ON).
 * ETTR needs manual ISO, so we switch to manual at the floor and let ETTR
 * drive it. Floor is enforced by MIN_ISO: ISO 200 with HTP, ISO 100 without. */
static CONFIG_INT("auto.ettr.iso.optimizer", auto_iso_optimizer, 1);

/* AI data logging: append one record to ML/logs/unified_log.txt on each
 * half-shutter press (firmware-side; ML Lua has no histogram access). */
static CONFIG_INT("auto.ettr.ai.logging", ai_data_logging, 1);

/* AI white balance: neutralize WB from the brightest highlights (white-point).
 * Affects RAW metadata + JPEG. Default OFF. */
static CONFIG_INT("auto.ettr.ai.wb", ai_white_balance, 0);

/* AI picture tune: per-lens Canon contrast/saturation from lens_tune.tbl.
 * JPEG-only (picstyle does not touch RAW). Default OFF. */
static CONFIG_INT("auto.ettr.ai.pictune", ai_picture_tune_en, 0);

/* AI WB warm/cool bias as a menu INDEX 0..8 that maps to Canon's WB-Shift
 * Amber/Blue axis, signed step = index - 4 (so 0=B4 cool .. 4=Neutral ..
 * 8=A4 warm). 1 step ~= 5 mireds (Canon spec). Default 4 = Neutral (was A2
 * warm, which biased everything warm and fed the earlier too-warm/green
 * complaints). Key renamed .warmth -> .tone so old saved "warmth" values
 * (0=neutral in the old scheme) don't silently mis-map to a cool bias here. */
static CONFIG_INT("auto.ettr.ai.wb.tone", ai_wb_warmth, 4);

/* Which shooting modes the whole AI system acts in.
 *   0 = P, M        (default)
 *   1 = P, M, Av, Tv
 * In every covered mode the AI never touches the parameter the user owns:
 *   Av -> aperture is the user's (AI adjusts ISO/shutter via Canon Auto ISO)
 *   Tv -> shutter is the user's (AI adjusts ISO/aperture via Canon Auto ISO)
 *   M  -> AI (metered ETTR) may drive ISO+shutter fully
 *   P  -> Canon owns shutter+aperture; AI adds HTP/ALO + learned ISO floor
 * ETTR's metered highlight push runs only in M (ML limitation); Av/Tv/P use
 * Canon's native Auto ISO, which by construction never touches the locked one. */
static CONFIG_INT("auto.ettr.ai.modes", ai_modes, 0);

/* True if the AI system should act in the current shooting mode. */
static int ai_mode_covered(void)
{
    int m = shooting_mode;
    if (m == SHOOTMODE_M || m == SHOOTMODE_P) return 1;
    if (ai_modes == 1 && (m == SHOOTMODE_AV || m == SHOOTMODE_TV)) return 1;
    return 0;
}

static int debug_info = 0;
static int show_metered_areas = 0;

/* one AI log record per half-press; shared between the LiveView polling path
 * and the OVF image-review (QR) path so they never double-log a shot */
static int ai_logged_press = 0;

#define AUTO_ETTR_TRIGGER_ALWAYS_ON (auto_ettr_trigger == 0 || is_intervalometer_running())
#define AUTO_ETTR_TRIGGER_AUTO_SNAP (auto_ettr_trigger == 1)
#define AUTO_ETTR_TRIGGER_PHOTO (AUTO_ETTR_TRIGGER_ALWAYS_ON || AUTO_ETTR_TRIGGER_AUTO_SNAP)
#define AUTO_ETTR_TRIGGER_BY_SET (auto_ettr_trigger == 2)
#define AUTO_ETTR_TRIGGER_BY_HALFSHUTTER (auto_ettr_trigger == 3)

/* status codes */
#define ETTR_EXPO_PRECOND_TIMEOUT -2
#define ETTR_EXPO_LIMITS_REACHED -1
#define ETTR_NEED_MORE_SHOTS 0
#define ETTR_SETTLED 1

/** Some cameras do not have raw liveview **/
extern WEAK_FUNC(ret_0) void raw_lv_request();
extern WEAK_FUNC(ret_0) void raw_lv_release();
extern WEAK_FUNC(ret_0) int  raw_lv_is_enabled();
// allow compiling module if FEATURE_RAW_ZEBRAS is undefined
extern WEAK_FUNC(ret_0) void zebra_highlight_raw_advanced(struct raw_highlight_info * raw_highlight_info);

/* optional beeps */
static void ettr_beep()
{
    if (auto_ettr_allow_beeps)
    {
        beep();
    }
}

static void ettr_beep_times(int n)
{
    if (auto_ettr_allow_beeps)
    {
        beep_times(n);
    }
}

static int ettr_get_current_long_exposure_time()
{
    int seconds = menu_get_value_from_script("Bulb Timer", "Exposure duration");
    return seconds;
}

static int ettr_get_current_raw_shutter()
{
    if (is_bulb_mode())
    {
        return shutterf_to_raw(ettr_get_current_long_exposure_time());
    }
    else
    {
        return lens_info.raw_shutter;
    }
}

static int auto_ettr_get_long_exposure_time(int raw_shutter)
{
    /* full-stops will be rounded to minutes, otherwise we get funny times like 91 seconds */
    int seconds = (int)roundf(30.0 * powf(2.0, (16.0 - raw_shutter)/8.0));
    
    /* things like 21 or 19 get rounded */
    int s = (seconds % 60) % 10;
    if (s == 1 || s == 3) {
        seconds--;
    } else if (s == 7 || s == 9) {
        seconds++;
    }
    
    return seconds;
}

static const char * ettr_format_shutter(int raw_shutter)
{
    if (raw_shutter >= SHUTTER_30s)
    {
        return lens_format_shutter(raw_shutter);
    }
    else
    {
        int seconds = auto_ettr_get_long_exposure_time(raw_shutter);
        return format_time_hours_minutes_seconds(seconds);
    }
}


static char* get_current_exposure_settings()
{
    static char msg[50];
    int iso1 = lens_info.iso_analog_raw;
    snprintf(msg, sizeof(msg), "ISO %d", raw2iso(iso1));
    int iso2 = dual_iso_get_alternate_iso();
    if (iso2 && iso2 != iso1)
    {
        STR_APPEND(msg, "/%d", raw2iso(iso2));
    }
    
    if (is_bulb_mode())
    {
        /* note: using ettr_format_shutter here introduces roundoff errors of a few seconds */
        int seconds = ettr_get_current_long_exposure_time();
        STR_APPEND(msg, " %s", format_time_hours_minutes_seconds(seconds));
    }
    else
    {
        STR_APPEND(msg, " %s", lens_format_shutter(lens_info.raw_shutter));
    }
    return msg;
}

static int extra_snr_needed = 0;

/* metering on dual ISO images can be affected by black level delta */
/* ideally, ev_hi = ev_lo + ev_delta, so we'll try to find a black level delta that matches this */
/* => solve this: raw_to_ev(raw_value_hi - black_delta) = raw_to_ev(raw_value_lo + black_delta) + ev_delta */
static int guess_black_delta(int raw_value_lo, int raw_value_hi, float ev_delta)
{
    float best_err = 100000;
    int best_black_delta = 0;
    for (int black_delta = -40; black_delta <= 40; black_delta++)
    {
        float err = ABS(raw_to_ev(raw_value_hi - black_delta) - raw_to_ev(raw_value_lo + black_delta) - ev_delta);
        if (err < best_err)
        {
            best_err = err;
            best_black_delta = black_delta;
        }
    }
    return best_black_delta;
}

/* also used for display on histogram */
/* ── ML Extended Intelligence: ETTR metadata (spec cr2-intelligence-
 * integration, Component 7) ────────────────────────────────────────────────
 * Reuses the SAME raw_hist_get_percentile_levels() statistical sampler the
 * stock ETTR metering below already calls -- not a second full-frame pixel
 * scan (Requirement 7.5) -- plus three additional per-channel percentile
 * queries (GRAY_PROJECTION_RED/GREEN/BLUE) to approximate per-channel clip.
 * Note: raw_hist_get_percentile_levels() reports the raw LEVEL at a given
 * percentile rank, not a pixel count above a threshold, so channelClip is a
 * coarse proxy (the histogram-rank fraction whose level has reached
 * raw_info.white_level) rather than an exact clipped-pixel-count fraction --
 * documented limitation, refine against real captures during task 10. */
static ettr_metadata_t g_last_ettr_meta = { 0, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f };

static float ettr_channel_clip_fraction(int gray_proj_channel, int speed)
{
    /* Percentile ladder from coarse (near-max) to fine; find the highest
     * rank whose raw level still reaches saturation. */
    int percentiles[6] = {1000, 990, 950, 900, 800, 500};
    int raw_values[COUNT(percentiles)];
    if (raw_hist_get_percentile_levels(percentiles, raw_values, COUNT(percentiles),
            gray_proj_channel | GRAY_PROJECTION_DARK_ONLY, speed) != 1)
        return 0.0f;

    for (int i = 0; i < (int) COUNT(percentiles); i++)
    {
        if (raw_values[i] < raw_info.white_level)
        {
            /* everything from percentile[i-1] and up (the previous, higher
             * rank) reached saturation; rank/1000 approximates the clipped
             * fraction. i==0 means even the 100th percentile clips fully. */
            return i == 0 ? 1.0f : (1000.0f - (float) percentiles[i]) / 1000.0f;
        }
    }
    return 0.0f;   /* not even the darkest sampled rank reached saturation */
}

static void ettr_compute_extended_metadata(int raw_highlight_lo, int raw_shadow_lo, int gray_proj, int speed)
{
    if (raw_highlight_lo <= raw_info.black_level || raw_shadow_lo <= raw_info.black_level)
    {
        /* Requirement 7.4: fully clipped or fully dark readout. */
        g_last_ettr_meta = (ettr_metadata_t){ ai_light_level(), 4.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        return;
    }

    float scene_dr = log2f((float)(raw_info.white_level - raw_shadow_lo + 1) /
                            (float)(raw_highlight_lo - raw_info.black_level + 1));
    float highlight_headroom = log2f((float) raw_info.white_level / (float)(raw_highlight_lo + 1));

    float clip_r = ettr_channel_clip_fraction(GRAY_PROJECTION_RED, speed);
    float clip_g = ettr_channel_clip_fraction(GRAY_PROJECTION_GREEN, speed);
    float clip_b = ettr_channel_clip_fraction(GRAY_PROJECTION_BLUE, speed);

    g_last_ettr_meta = (ettr_metadata_t){
        ai_light_level(),
        COERCE(scene_dr, 4.0f, 14.0f),
        COERCE(highlight_headroom, 0.0f, 3.0f),
        COERCE(clip_r, 0.0f, 1.0f),
        COERCE(clip_g, 0.0f, 1.0f),
        COERCE(clip_b, 0.0f, 1.0f),
    };
}

ettr_metadata_t ettr_last_metadata(void)
{
    return g_last_ettr_meta;
}

static int auto_ettr_get_correction()
{
    static int last_value = INT_MIN;
    
    /* this is kinda slow, don't run it very often */
    static int aux = INT_MIN;
    if (!lv && !should_run_polling_action(100, &aux) && last_value != INT_MIN)
        return last_value;
    
    int gray_proj = 
        auto_ettr_clip == 0 ? GRAY_PROJECTION_MAX_RGB :
        auto_ettr_clip == 1 ? GRAY_PROJECTION_MAX_RB :
        auto_ettr_clip == 2 ? GRAY_PROJECTION_MEDIAN_RGB : -1;
    
    /* compute the raw levels for more percentile values; will help if the image is overexposed */
    /* if it's not, we'll use only the first value (the one from menu) */
    int percentiles[13] = {(1000 - auto_ettr_ignore), 950, 900, 800, 750, 700, 600, 500, 300, 200, 150, 100, 50};

    int raw_values[COUNT(percentiles)];
    static float diff_from_lower_percentiles[COUNT(percentiles)-1] = {0};

    int speed = 1; /* 1 = examine each LiveView pixel (720x480); 2 = downsample by 2 and so on */
    if (lv)
    {
        /* if highlight ignore is off, we have to look carefully */
        /* otherwise, the meter is not that sensitive and can be a little faster */
        speed = auto_ettr_ignore ? 4 : 2;
    }

    int ok = raw_hist_get_percentile_levels(percentiles, raw_values, COUNT(percentiles), gray_proj | GRAY_PROJECTION_DARK_ONLY, speed);
    
    if (ok != 1)
    {
        last_value = INT_MIN;
        return last_value;
    }
    
    float ev = raw_to_ev(raw_values[0]);
    int raw_median_lo = raw_values[7];  /* 50th percentile (median) */
    int raw_shadow_lo = raw_values[12]; /* 5th percentile */
    int raw_highlight_lo = raw_values[0]; /* "highlight ignore" percentile */
    float ev_median_lo = raw_to_ev(raw_median_lo);
    float ev_shadow_lo = raw_to_ev(raw_shadow_lo);

    ettr_compute_extended_metadata(raw_highlight_lo, raw_shadow_lo, gray_proj, speed);

    int dual_iso = auto_ettr_dual_iso_link && dual_iso_is_active();
    float ev_median_hi = ev_median_lo;
    float ev_shadow_hi = ev_shadow_lo; /* for dual ISO: for the bright exposure */
    
    if (dual_iso)
    {
        /* for dual ISO only:*/
        /* we have metered the dark exposure (since ETTR is pushing that to the right), now meter the bright one too */

        /* EV difference between the two ISOs (from settings) */
        float dual_iso_spacing = ABS(dual_iso_get_alternate_iso() - lens_info.iso_analog_raw) / 8.0;

        if (lv && !is_movie_mode())
        {
            /* photo LV (only one exposure) */
            /* estimate it from settings */
            int rec_iso = dual_iso_get_alternate_iso();
            if (rec_iso > (int)lens_info.iso_analog_raw) /* we are looking at the dark exposure */
            {
                ev_median_hi = MIN(ev_median_lo + dual_iso_spacing, 0); /* you can't get whiter than white */
                ev_shadow_hi = MIN(ev_shadow_lo + dual_iso_spacing, 0);
            }
            else /* we are looking at the bright exposure */
            {
                ev_median_hi = ev_median_lo - dual_iso_spacing;
                ev_shadow_hi = ev_shadow_lo - dual_iso_spacing;
                float aux = ev_median_hi; ev_median_hi = ev_median_lo; ev_median_lo = aux;
                aux = ev_shadow_hi; ev_shadow_hi = ev_shadow_lo; ev_shadow_lo = aux;
            }
        }
        else
        {
            /* photo non-LV and movie */
            int percentiles_hi[2] = {500, 50};
            int raw_values_hi[2];
            raw_hist_get_percentile_levels(percentiles_hi, raw_values_hi, COUNT(percentiles_hi), gray_proj | GRAY_PROJECTION_BRIGHT_ONLY, 4);
            int raw_median_hi = raw_values_hi[0];  /* 50th percentile (median) */
            int raw_shadow_hi = raw_values_hi[1]; /* 5th percentile */

            /* signal level for the higher exposure must be equal to signal level for the lower exposure plus dual ISO spacing (EV) */
            /* if it's not, it's very likely to be a large black level difference messing with our formulas */
            /* let's try to fight it! */

            /* compute it from shadow levels, because this is where black delta has the largest effect */
            /* if you compute it from median, shadow may be still wrong by 1-2 EV */
            /* if you compute it from shadow, median may be wrong by only 0.1 - 0.2 EV - much better! */
            int black_delta = guess_black_delta(raw_shadow_lo, raw_shadow_hi, dual_iso_spacing);

            ev_median_lo = raw_to_ev(raw_median_lo + black_delta);
            ev_shadow_lo = raw_to_ev(raw_shadow_lo + black_delta);

            ev_median_hi = raw_to_ev(raw_median_hi - black_delta);
            ev_shadow_hi = raw_to_ev(raw_shadow_hi - black_delta);

            if (debug_info)
            {
                int gap_med = (ev_median_hi - ev_median_lo) * 100;
                int gap_shad = (ev_shadow_hi - ev_shadow_lo) * 100;
                printf("Black delta  : %d (EV gap mid:%s%d.%02d shad:%s%d.%02d)\n", black_delta, FMT_FIXEDPOINT2(gap_med), FMT_FIXEDPOINT2(gap_shad));
            }
        }
    }

    if (show_metered_areas)
    {
        /* show where exactly are those percentiles */
        bmp_printf(FONT(FONT_SMALL, COLOR_WHITE, COLOR_BLUE),   0, 20, "Shadows    5%%   ");
        bmp_printf(FONT(FONT_SMALL, COLOR_WHITE, COLOR_ORANGE), 0, 32, "Midtones   50%%  ");
        int hp = (1000 - auto_ettr_ignore);
        bmp_printf(FONT(FONT_SMALL, COLOR_WHITE, COLOR_RED),    0, 44, "Highlights%3d.%d%%", hp/10, hp%10);
        zebra_highlight_raw_advanced(
            (struct raw_highlight_info [])
            {
                {
                    .raw_level_lo = 0,
                    .raw_level_hi = raw_shadow_lo,
                    .color = COLOR_BLUE,
                    .line_type = ZEBRA_LINE_SIMPLE,
                    .fill_type = ZEBRA_FILL_DIAG,
                    .gray_projection = gray_proj | GRAY_PROJECTION_DARK_ONLY,
                },
                {
                    .raw_level_lo = raw_median_lo,
                    .raw_level_hi = raw_median_lo,
                    .color = COLOR_ORANGE,
                    .line_type = ZEBRA_LINE_SIMPLE,
                    .gray_projection = gray_proj | GRAY_PROJECTION_DARK_ONLY,
                },
                {
                    .raw_level_lo = raw_highlight_lo,
                    .raw_level_hi = 16384,
                    .color = COLOR_RED,
                    .line_type = ZEBRA_LINE_SIMPLE,
                    .fill_type = ZEBRA_FILL_DIAG,
                    .gray_projection = gray_proj | GRAY_PROJECTION_DARK_ONLY,
                },
                RAW_HIGHLIGHT_END
            }
        );
    }

    //~ bmp_printf(FONT_MED, 50, 200, "%d ", MEMX(0xc0f08030));
    float target = MIN(auto_ettr_target_level, -0.5);
    /* per-lens exposure bias from lens_tune.tbl (0 unless a row asks for it --
     * e.g. a lens known to bloom its highlights meters 1 EV more protective) */
    target += ai_lens_ev8 / 8.0;
    float correction = target - ev;
    float overexposed_percentage = 0;
    if (ev < -0.1)
    {
        /* cool, we know exactly how much to correct, we'll return "correction" */
        
        /* save data for helping with future overexposed shots */
        for (int k = 0; k < COUNT(percentiles)-1; k++)
            diff_from_lower_percentiles[k] = ev - raw_to_ev(raw_values[k+1]);
        
        if (debug_info) printf("overexposure hints: %d %d %d\n", (int)(diff_from_lower_percentiles[0] * 100), (int)(diff_from_lower_percentiles[1] * 100), (int)(diff_from_lower_percentiles[2] * 100));
    }
    else
    {
        /* image is overexposed */
        /* and we don't know how much to go back in order to fix the overexposure */

        /* we can find out how many pixels are clipped, but this doesn't help much in knowing how many stops we should go back */
        overexposed_percentage = raw_hist_get_overexposure_percentage(GRAY_PROJECTION_AVERAGE_RGB | GRAY_PROJECTION_DARK_ONLY) / 100.0;
        if (debug_info) printf("overexposure area: %s%d.%d%%\n", FMT_FIXEDPOINT2((int)(overexposed_percentage * 100)));

        /* from the previous shot, we know where the highlights were, compared to some lower percentiles */
        /* let's assume this didn't change; meter at those percentiles and extrapolate the result */

        int num = 0;
        float sum = 0;
        float min = 100000;
        float max = -100000;
        for (int k = 0; k < COUNT(percentiles)-1; k++)
        {
            if (diff_from_lower_percentiles[k] > 0)
            {
                float lower_ev = raw_to_ev(raw_values[k+1]);
                if (lower_ev < -0.1)
                {
                    /* if the scene didn't change, we should be spot on */
                    /* don't update the correction hints, since we don't know exactly where we are */
                    ev = lower_ev + diff_from_lower_percentiles[k];
                    
                    /* we need to get a stronger correction than with the overexposed metering */
                    /* otherwise, the scene probably changed */
                    if (target - ev < correction)
                    {
                        float corr = target - ev;
                        min = MIN(min, corr);
                        max = MAX(max, corr);
                        
                        /* first estimations are more reliable, weight them a bit more */
                        sum += corr * (COUNT(percentiles) - k);
                        num += (COUNT(percentiles) - k);
                        //~ msleep(500);
                        printf("overexposure fix: k=%d diff=%d ev=%d corr=%d\n", k, (int)(diff_from_lower_percentiles[k] * 100), (int)(ev * 100), (int)(corr * 100));
                    }
                }
            }
        }

        /* use the average value for correction */
        correction = sum / num;
        
        if (num < 3 || max - correction > 1 || correction - min > 1 || correction > -1)
        {
            /* scene changed? measurements from previous shot not confirmed or vary too much?
             * 
             * we'll use a heuristic: for 1% of blown out image, go back 1EV, for 100% go back 13EV */
            printf("fail info: (%d %d %d %d) (%d %d %d)\n", raw_values[0], raw_values[1], raw_values[2], raw_values[3], (int)(diff_from_lower_percentiles[0] * 100), (int)(diff_from_lower_percentiles[1] * 100), (int)(diff_from_lower_percentiles[2] * 100));
            float corr = - log2f(1 + overexposed_percentage*overexposed_percentage);
            
            /* with dual ISO, the cost of underexposing is not that high, so prefer it to improve convergence */
            if (dual_iso)
                corr *= 3;
            
            correction = MIN(correction, corr);
            
            /* we can't really meter more than 10 EV */
            correction = MAX(correction, -10);
        }
    }

    int iso1 = lens_info.iso_analog_raw;
    int iso2 = iso1;
    if (dual_iso) iso2 = dual_iso_get_alternate_iso();
    int iso_hi = MAX(iso1, iso2);
    int iso_lo = MIN(iso1, iso2);
    float dr_lo = get_dxo_dynamic_range(iso_lo) / 100.0;
    float dr_hi = get_dxo_dynamic_range(iso_hi) / 100.0;

    if (debug_info)
    {
        if (dual_iso)
        {
            float midtone_snr_lo = dr_lo + ev_median_lo;
            float shadow_snr_lo = dr_lo + ev_shadow_lo;
            int mid_snr_lo = (int)roundf(midtone_snr_lo * 10);
            int shad_snr_lo = (int)roundf(shadow_snr_lo * 10);
            float midtone_snr_hi = dr_hi + ev_median_hi;
            float shadow_snr_hi = dr_hi + ev_shadow_hi;
            int mid_snr_hi = (int)roundf(midtone_snr_hi * 10);
            int shad_snr_hi = (int)roundf(shadow_snr_hi * 10);
            printf("Midtone SNR  : %s%d.%d / %s%d.%d EV\n", FMT_FIXEDPOINT1(mid_snr_lo), FMT_FIXEDPOINT1(mid_snr_hi));
            printf("Shadows SNR  : %s%d.%d / %s%d.%d EV\n", FMT_FIXEDPOINT1(shad_snr_lo), FMT_FIXEDPOINT1(shad_snr_hi));
        }
        else
        {
            float midtone_snr = dr_lo + ev_median_lo;
            float shadow_snr = dr_lo + ev_shadow_lo;
            int mid_snr = (int)roundf(midtone_snr * 10);
            int shad_snr = (int)roundf(shadow_snr * 10);
            printf("Midtone SNR  : %s%d.%d EV\n", FMT_FIXEDPOINT1(mid_snr));
            printf("Shadows SNR  : %s%d.%d EV\n", FMT_FIXEDPOINT1(shad_snr));
        }
        int clipped = raw_hist_get_overexposure_percentage(GRAY_PROJECTION_AVERAGE_RGB | GRAY_PROJECTION_DARK_ONLY);
        printf("Clipped highs: %s%d.%02d%%\n", FMT_FIXEDPOINT2(clipped));
    }

    if (overexposed_percentage > 0 && (auto_ettr_midtone_snr_limit || auto_ettr_shadow_snr_limit) && !dual_iso)
    {
        /* if the image is overexposed and we have SNR limits, we could meter for those instead */
        /* don't underexpose by more than 2 EV in one step though */
        correction -= 2;
    }

    /* are we underexposing too much? */
    float correction0 = correction;
    if (lens_info.raw_iso && (auto_ettr_midtone_snr_limit || auto_ettr_shadow_snr_limit))
    {
        float midtone_snr = dr_lo + ev_median_lo;
        float shadow_snr = dr_lo + ev_shadow_lo;

        if (auto_ettr_midtone_snr_limit)
        {
            float midtone_expected_snr = midtone_snr + correction0;
            int midtone_desired_snr = auto_ettr_midtone_snr_limit;

            if (midtone_expected_snr < midtone_desired_snr)
            {
                correction = MAX(correction, correction0 + midtone_desired_snr - midtone_expected_snr);
            }
        }

        if (auto_ettr_shadow_snr_limit)
        {
            float shadow_expected_snr = shadow_snr + correction0;
            int shadow_desired_snr = auto_ettr_shadow_snr_limit;

            if (shadow_expected_snr < shadow_desired_snr)
            {
                correction = MAX(correction, correction0 + shadow_desired_snr - shadow_expected_snr);
            }
        }
    }
    
    /* exposure difference with and without SNR limits */
    int expo_delta_snr = (correction - correction0) * 100.0;
    
    if (debug_info)
    {
        int expo_hi = correction0 * 100.0;
        int expo_snr = correction * 100.0;
        printf("Expo highlight: %s%d.%02d EV\n", FMT_FIXEDPOINT2S(expo_hi));
        printf("Expo SNR limit: %s%d.%02d EV\n", FMT_FIXEDPOINT2S(expo_snr));
        printf("Expo delta SNR: %s%d.%02d EV\n", FMT_FIXEDPOINT2S(expo_delta_snr));
    }

    /* exposure correction so it doesn't clip anything more than allowed by highlight ignore */
    int corr_without_clipping = (int)(correction * 100) - expo_delta_snr;

    if (dual_iso)
    {
        /* with dual ISO: expose without clipping */
        /* auto_ettr_work will have to do something and recover the SNR */
        last_value = corr_without_clipping;
        extra_snr_needed = expo_delta_snr;
    }
    else
    {
        /* without dual ISO: expose with clipping in order to meet the SNR */
        /* no more SNR correction needed */
        last_value = corr_without_clipping + expo_delta_snr;
        extra_snr_needed = 0;
    }
    
    if (debug_info)
    {
        printf("Expo correction: %s%d.%02d EV\n", FMT_FIXEDPOINT2S(last_value));
    }
    return last_value;
}

int auto_ettr_export_correction(int* out)
{
    int value = auto_ettr_get_correction();
    if (value == INT_MIN) return -1;
    if (out) *out = value;
    return 1;
}

static char prev_exposure_settings[50];

/* returns: 0 = nothing changed, 1 = OK, -1 = exposure limits reached */
static int auto_ettr_work(int corr)
{
    if (debug_info) printf("\nauto_ettr_work(%d)\n", corr);
    /* wait until shutter speed is reported by Canon firmware */
    int iter = 0;
    while (lens_info.raw_shutter == 0)
    {
        if (iter > 100)
        {
            return ETTR_EXPO_PRECOND_TIMEOUT;
        }
        msleep(50);
        iter += 50;
    }
    
    /* save initial exposure settings so we can print them */
    char* expo_settings = get_current_exposure_settings();
    snprintf(prev_exposure_settings, sizeof(prev_exposure_settings), "%s", expo_settings);
    
    int tv = ettr_get_current_raw_shutter();
    int iso = lens_info.raw_iso;
    
    /* to detect whether it settled or not */
    int tv_before = tv;
    int iso_before = iso;
    int iso2_before = dual_iso_get_alternate_iso();
    
    if (!tv || !iso) return 0;
    //~ int old_expo = tv - iso;

    /* note: expo compensation will not clip with dual ISO, but will clip highlights without it */
    int dual_iso = auto_ettr_dual_iso_link && dual_iso_is_active();
    int delta = -corr * 8 / 100;
    
    int expected_expo = tv - iso + delta;               /* will clip without dual ISO */

    static int prev_tv = 0;
    if (auto_ettr_adjust_mode == 1)
    {
        if (prev_tv != tv)
        {
            auto_ettr_max_shutter = tv;
            if (lv)
            {
                NotifyBox(2000, "ETTR: Tv <= %s ", lens_format_shutter(tv));
                prev_tv = tv;
                return 0; /* wait for next iteration */
            }
            else
            {
                msleep(1000);
                bmp_printf(FONT_MED, 0, os.y0, "ETTR: Tv <= %s ", lens_format_shutter(tv));
            }
        }
    }
    else
    {
        if (lv && prev_tv != tv && AUTO_ETTR_TRIGGER_ALWAYS_ON)
        {
            prev_tv = tv;
            return 0; /* small pause when you change exposure manually */
        }
    }

    int shutter_lim = auto_ettr_max_shutter;

    /* if intervalometer is enabled, limit longest exposures
     * to interval time minus 2 seconds */
    if (is_intervalometer_running())
    {
        int intervalometer_lim = MAX(200, 1000 * (get_interval_time() - 2));
        shutter_lim = MAX(shutter_lim, shutter_ms_to_raw(intervalometer_lim));
    }

    /* can't go slower than 1/fps in movie mode */
    if (is_movie_mode())
    {
        shutter_lim = MAX(shutter_lim, shutter_ms_to_raw(1000 / video_mode_fps));
        if (!expo_override_active())
        {
            /* without expo override, in movie mode we can't set exposures longer than 1/30 */
            shutter_lim = MAX(shutter_lim, SHUTTER_1_30);
        }
    }

    /* apply exposure correction */
    tv += delta;

    if (debug_info) printf("expo after comp: %d\n", tv - iso);

    /* use the lowest ISO for which we can get shutter = shutter_lim or higher */
    int offset = MIN(tv - shutter_lim, iso - MIN_ISO);
    tv -= offset;
    iso -= offset;

    /* some shutter values are not accepted by Canon firmware */
    int tvr = (MIN(tv, shutter_lim) >= SHUTTER_30s)
        ? round_shutter(tv, shutter_lim)
        : MAX(tv, shutter_lim);
    
    iso += tvr - tv;

    if (debug_info) printf("tv rounding: %d -> %d limit=%d\n", tv, tvr, shutter_lim);
    
    /* analog iso can be only in 1 EV increments */
    /* prefer rounding towards lower ISOs */
    int max_auto_iso = auto_iso_range & 0xFF;
    int isor = COERCE(iso / 8 * 8, MIN_ISO, max_auto_iso);
    if (debug_info) printf("iso rounding: %d -> %d (expo %d -> %d)\n", iso, isor, tvr - iso, tvr - isor);
    
    /* can we use dual ISO to recover the highlights? (HR = highlight recovery) */
    if (dual_iso)
    {
        int base_iso = isor;
        int recovery_iso = base_iso;

        /* bring back the SNR */
        int snr_delta = -extra_snr_needed;
        while (snr_delta < 0 && recovery_iso < max_auto_iso)
        {
            int old_rec_iso = recovery_iso;
            recovery_iso += 8;
            int dr_gained = dual_iso_calc_dr_improvement(old_rec_iso, recovery_iso);
            snr_delta += dr_gained;
        }
        
        if (snr_delta + extra_snr_needed < 100) /* snr_delta + extra_snr_needed = SUM(dr_gained) */
        {
            /* too little gain? just shoot at base ISO */
            recovery_iso = base_iso;
            snr_delta = -extra_snr_needed;
        }
        else if (base_iso > MIN_ISO)
        {
            /* shooting at high ISO? go back one stop to protect some more highlights, because the cost is next to none */
            base_iso -= 8;
            expected_expo += 8;
        }

        /* apply dual ISO settings */
        isor = base_iso;
        dual_iso_set_alternate_iso(recovery_iso);
        extra_snr_needed = -snr_delta;
    }

    /* apply the new settings */
    int oki = 0, oks = 0;
    if (tvr < SHUTTER_30s)
    {
        /* use BULB for long exposures */
        ensure_bulb_mode();
        int seconds = auto_ettr_get_long_exposure_time(tvr);
        
        if (is_intervalometer_running())
        {
            /* in BULB mode, limit longest exposures to interval time minus 3 seconds */
            int intervalometer_lim = MAX(1, get_interval_time() - 3);
            seconds = MIN(seconds, intervalometer_lim);
        }
        
        /* configure bulb timer with the new exposure */
        menu_set_value_from_script("Bulb Timer", "Exposure duration", seconds);
        oks = 1;

        /* set ISO */
        oki = hdr_set_rawiso(isor);
    }
    else
    {
        if (is_bulb_mode())
        {
            /* back from BULB */
            set_shooting_mode(SHOOTMODE_M);
        }
        
        oki = lens_set_rawiso(isor);    /* for expo overide */
        oks = lens_set_rawshutter(tvr);
        if (!expo_override_active())
        {
            oks = hdr_set_rawshutter(tvr);  /* for confirmation and retrying if needed */
            oki = hdr_set_rawiso(isor);
        }
    }

    /* don't let expo lock undo our changes */
    expo_lock_update_value();

    if (debug_info)
    {
        printf("Adjusted expo: %s (SNR lost: %s%d.%02d)\n", get_current_exposure_settings(), FMT_FIXEDPOINT2(extra_snr_needed));
    }

    /* to know when the user changed shutter speed */
    prev_tv = ettr_get_current_raw_shutter();
    
    /* did it converge or not? */
    int tv_after = prev_tv;
    int iso_after = lens_info.raw_iso;
    int new_expo = tv_after - iso_after;

    if (dual_iso)
    {
        int iso2_after = dual_iso_get_alternate_iso();
        int dr2_before = dual_iso_calc_dr_improvement(iso_before, iso2_before);
        int dr2_after = dual_iso_calc_dr_improvement(iso_after, iso2_after);

        if (debug_info)
        {
            printf( 
                "iso2 %d->%d dr %d->%d\n",
                raw2iso(iso2_before), raw2iso(iso2_after), dr2_before, dr2_after
            );
        }

        if (ABS(dr2_after - dr2_before) >= 40)
            return ETTR_NEED_MORE_SHOTS;
        
        //~ if (highlight_headroom_needed > 50)
            //~ return ETTR_EXPO_LIMITS_REACHED;
    }

    if (debug_info)
    {
        printf(
            "iso %d->%d %s\ntv %d->%d %s\nexpo expected %d got %d\n",
            raw2iso(iso_before), raw2iso(iso_after), oki ? "OK" : "err",
            tv_before, tv_after, oks ? "OK" : "err",
            expected_expo, new_expo
        );
    }
    
    /* anything changed? consider it OK, better than nothing */
    if (ABS(tv_before - tv_after) >= 4)
        return ETTR_NEED_MORE_SHOTS;

    if (ABS(iso_before - iso_after) >= 4)
        return ETTR_NEED_MORE_SHOTS;

    /* did we fully correct the exposure? */
    if (ABS(new_expo - expected_expo) > 8)
        return ETTR_EXPO_LIMITS_REACHED;

    return oks && oki ? ETTR_SETTLED : ETTR_EXPO_LIMITS_REACHED;
}

static volatile int auto_ettr_running = 0;
static volatile int ettr_pics_took = 0;

static void auto_ettr_step_task(int corr)
{
    lens_wait_readytotakepic(64);
    int status = auto_ettr_work(corr);
    
    if (status == ETTR_SETTLED)
    {
        /* cool, we got the ideal exposure */
        ettr_beep();
        ettr_pics_took = 0;
        
        msleep(1000);
        bmp_printf(FONT_MED, 0, os.y0, "ETTR: settled at %s", get_current_exposure_settings());

        //~ int blown_highlights = (highlight_headroom_needed - highlight_headroom_recovered) / 10;
        //~ if (blown_highlights > 2)
        //~ {
            //~ msleep(1000);
            //~ bmp_printf(FONT_MED, 0, os.y0, "ETTR: clipped %s%d.%d EV of highlights", FMT_FIXEDPOINT1(blown_highlights));
        //~ }
    }
    else if (ettr_pics_took >= 3)
    {
        /* I give up */
        ettr_beep_times(3);
        ettr_pics_took = 0;
        msleep(1000);
        bmp_printf(FONT_MED, 0, os.y0, "ETTR: giving up\n%s", get_current_exposure_settings());
    }
    else if (status == ETTR_EXPO_LIMITS_REACHED)
    {
        ettr_beep_times(3);
        ettr_pics_took = 0;
        msleep(1000);
        bmp_printf(FONT_MED, 0, os.y0, "ETTR: expo limits reached\n%s", get_current_exposure_settings());
    }
    else if (status == ETTR_EXPO_PRECOND_TIMEOUT)
    {
        ettr_beep_times(3);
        ettr_pics_took = 0;
        msleep(1000);
        bmp_printf(FONT_MED, 0, os.y0, "ETTR: timeout while waiting for preconditions\n");
    }
    else if (AUTO_ETTR_TRIGGER_AUTO_SNAP)
    {
        /* take another pic */
        auto_ettr_running = 0;
        schedule_remote_shot();
        ettr_pics_took++;
    }
    else if (AUTO_ETTR_TRIGGER_ALWAYS_ON)
    {
        ettr_beep_times(2);
        msleep(1000);
        bmp_printf(FONT_MED, 0, os.y0, "ETTR: next %s (was %s)", get_current_exposure_settings(), prev_exposure_settings);

        //~ int blown_highlights = (highlight_headroom_needed - highlight_headroom_recovered) / 10;
        //~ if (blown_highlights > 2)
        //~ {
            //~ bmp_printf(FONT_MED, 0, os.y0+20, "Clipped %s%d.%d EV of highlights", FMT_FIXEDPOINT1(blown_highlights));
        //~ }
    }
    auto_ettr_running = 0;
}

/* Photo/QR metering with retry. The intermittent OVF ETTR miss was here: at QR
 * the just-captured raw buffer can lag the review opening by a frame or two, so
 * the old one-shot raw_update_params() failed ("Raw error") and the shot was
 * skipped -- leaving it underexposed. Retry for ~0.5 s before giving up. */

static void auto_ettr_photo_task(int unused)
{
    int ok = 0;
    for (int i = 0; i < 10 && !ok; i++)
    {
        ok = raw_update_params();
        if (!ok) msleep(50);
    }
    if (ok)
    {
        int corr = auto_ettr_get_correction();
        if (corr != INT_MIN)
        {
            auto_ettr_step_task(corr);   /* applies the correction + clears the flag */
            return;
        }
    }
    auto_ettr_running = 0;
}

/* photo mode only, no LV */
static void auto_ettr_step()
{
    if (!auto_ettr) return;
    if (shooting_mode != SHOOTMODE_M && !is_movie_mode() && !is_bulb_mode()) return;
    if (lens_info.raw_iso == 0) return;
    if (auto_ettr_running) return;
    if (is_hdr_bracketing_enabled() && !AUTO_ETTR_TRIGGER_BY_SET) return;

    /* meter in a task so it can wait for the raw buffer (see above) */
    auto_ettr_running = 1;
    task_create("ettr_task", 0x1c, 0x1000, auto_ettr_photo_task, (void*) 0);
}

static int auto_ettr_check_pre_lv()
{
    if (!auto_ettr) return 0;
    if (shooting_mode != SHOOTMODE_M && !is_movie_mode()) return 0;
    if (lens_info.raw_iso == 0) return 0;
    if (lens_info.raw_shutter == 0) return 0;
    if (is_hdr_bracketing_enabled() && !AUTO_ETTR_TRIGGER_BY_SET) return 0;
    int raw = is_movie_mode() ? raw_lv_is_enabled() : pic_quality & 0x60000;
    return raw;
}

static int auto_ettr_check_in_lv()
{
    if (AUTO_ETTR_TRIGGER_ALWAYS_ON && !get_expsim()) return 0;
    if (AUTO_ETTR_TRIGGER_ALWAYS_ON && lv_dispsize != 1) return 0;
    if (LV_PAUSED) return 0;
    if (!liveview_display_idle()) return 0;
    return 1;
}

static int auto_ettr_check_lv()
{
    if (!auto_ettr_check_pre_lv()) return 0;
    if (!auto_ettr_check_in_lv()) return 0;
    return 1;
}

static volatile int auto_ettr_vsync_active = 0;
static volatile int auto_ettr_vsync_delta = 0;
static volatile int auto_ettr_vsync_counter = 0;

/* instead of changing settings via properties, we can override them very quickly */
static unsigned int auto_ettr_vsync_cbr(unsigned int ctx)
{
    auto_ettr_vsync_counter++;
#if ETTR_VIDEO_GYRO_METADATA
    gyro_bridge_poll();  /* Poll external IMU at frame cadence */
#endif

    if (auto_ettr_vsync_active)
    {
        int delta = auto_ettr_vsync_delta;
        int current_iso = get_frame_iso();
        int current_shutter = get_frame_shutter_timer();
        int altered_iso = current_iso;
        int altered_shutter = current_shutter;

        int max_shutter = get_max_shutter_timer();
        if (current_shutter > max_shutter) max_shutter = current_shutter;

        /* first increase shutter speed, since it gives the cleanest signal */
        while (delta > 0 && altered_shutter * 2 <= max_shutter)
        {
            altered_shutter *= 2;
            delta -= 8;
        }

        int max_iso = get_max_analog_iso();

        /* then try to increase ISO if we need more */
        while (delta > 0 && altered_iso + 8 <= max_iso)
        {
            altered_iso += 8;
            delta -= 8;
        }

        /* then try to decrease ISO until ISO 100, raw 72 (even with HTP, FRAME_ISO goes to 100) */
        while (delta < -8 && altered_iso - 8 >= 72)
        {
            altered_iso -= 8;
            delta += 8;
        }

        /* commit iso */
        set_frame_iso(altered_iso);

        /* adjust shutter with the remaining delta */
        altered_shutter = COERCE((int)roundf(powf(2, delta/8.0) * (float)altered_shutter), 2, max_shutter);
        
        /* commit shutter */
        set_frame_shutter_timer(altered_shutter);
        
        //~ bmp_printf(FONT_MED, 50, 70, "delta %d iso %d->%d shutter %d->%d max %d ",  auto_ettr_vsync_delta, current_iso, altered_iso, current_shutter, altered_shutter, get_max_shutter_timer());
        return 1;
    }
    
    return 0;
}

static int auto_ettr_wait_lv_frames(int num_frames)
{
    auto_ettr_vsync_counter = 0;
    int count = 0;
    int frame_duration = 1000000 / fps_get_current_x1000();
    while (auto_ettr_vsync_counter < num_frames)
    {
        frame_duration = MAX(frame_duration, 1000000 / fps_get_current_x1000());
        msleep(20);
        count++;
        if (count > num_frames * frame_duration * 2 / 20)
        {
            /* timeout */
            if (debug_info) printf("wait_lv_frames: timeout\n");
            return 0;
        }
        if (!lv)
        {
            /* outside lv */
            if (debug_info) printf("wait_lv_frames: LV closed\n");
            return 0;
        }
    }
    return 1;
}

/* wait until LiveView exposure changes from the old values to something else (with timeout on number of frames) */
static int auto_ettr_wait_lv_expo_change(int max_frames, int old_iso, int old_shutter)
{
    /* todo: also look at aperture changes */
    for (int i = 0; i < max_frames; i++)
    {
        int current_iso = get_frame_iso();
        int current_shutter = get_frame_shutter_timer();
        if (debug_info) printf("wait lv expo change: %x %x\n", current_iso, current_shutter);
        if (current_iso != old_iso || current_shutter != old_shutter)
        {
            if (debug_info) printf("exposure changed to: %x %x\n", current_iso, current_shutter);
            /* exposure changed */
            return 1;
        }
        if (!auto_ettr_wait_lv_frames(1))
        {
            /* whoops */
            return 0;
        }
    }

    /* timeout */
    if (debug_info) printf("lv expo change timeout\n");
    return 0;
}

/* wait until LiveView exposure settles (identical on two consecutive frames) */
static int auto_ettr_wait_lv_expo_settle(int max_frames)
{
    /* todo: also look at aperture changes */
    int old_iso = -1;
    int old_shutter = -1;
    for (int i = 0; i < max_frames; i++)
    {
        int current_iso = get_frame_iso();
        int current_shutter = get_frame_shutter_timer();
        if (debug_info) printf("wait lv expo settle: %x %x\n", current_iso, current_shutter);
        if (current_iso == old_iso && current_shutter == old_shutter)
        {
            /* looks like it settled */
            if (debug_info) printf("lv expo maybe settled at: %x %x\n", current_iso, current_shutter);
            
            /* wait one more frame, just in case */
            if (auto_ettr_wait_lv_frames(2) == 0)
            {
                return 0;
            }
            
            current_iso = get_frame_iso();
            current_shutter = get_frame_shutter_timer();
            if (current_iso == old_iso && current_shutter == old_shutter)
            {
                if (debug_info) printf("lv expo settled at: %x %x\n", current_iso, current_shutter);
                /* looks like it did settle */
                return 1;
            }
        }
        if (!auto_ettr_wait_lv_frames(1))
        {
            /* whoops */
            return 0;
        }
        old_iso = current_iso;
        old_shutter = current_shutter;
    }
    
    /* timeout */
    if (debug_info) printf("lv expo settle timeout\n");
    return 0;
}

static int auto_ettr_prepare_lv(int reset, int force_expsim_and_zoom)
{
    static int was_in_lv = 1;
    static int old_expsim = -1;
    static int old_zoom = -1;
    static int should_clear_bv = 0;
    
    if (!reset)
    {
        was_in_lv = lv;
        old_expsim = -1;

        if (!lv) force_liveview();
        if (!lv) return 0; /* fail */

        /* force 1x zoom */
        if (force_expsim_and_zoom && lv_dispsize != 1)
        {
            old_zoom = lv_dispsize;
            set_lv_zoom(1);
            if (!auto_ettr_wait_lv_frames(10)) return 0;
        }

        /* temporarily enable get_expsim() while metering */
        if (force_expsim_and_zoom)
        {
            if (shooting_mode == SHOOTMODE_M && !lens_info.lens_exists)
            {
                /* workaround for Canon's manual lens underexposure bug */
                /* use expo override instead of ExpSim */
                extern int bv_auto;
                if (!bv_auto)
                {
                    should_clear_bv = 1;
                    bv_toggle(0, 1);
                    if (!auto_ettr_wait_lv_frames(10)) return 0;
                }
            }
            else if (!get_expsim())
            {
                /* ExpSim should work well */
                old_expsim = get_expsim();
                set_expsim(1);
                if (!auto_ettr_wait_lv_frames(10)) return 0;
            }
        }
    }
    else /* undo all that stuff */
    {
        if (should_clear_bv)
        {
            extern int bv_auto;
            if (bv_auto)
            {
                bv_toggle(0, -1);
                auto_ettr_wait_lv_frames(5);
            }
            should_clear_bv = 0;
        }
        
        if (old_expsim >= 0)
        {
            set_expsim(old_expsim);
            old_expsim = -1;
        }
        
        if (old_zoom > 0)
        {
            set_lv_zoom(old_zoom);
            old_zoom = -1;
        }
        
        if (lv && !was_in_lv)
        {
            msleep(200);
            close_liveview();
            was_in_lv = 1;
        }
    }
    return 1; /* ok */
}

static void auto_ettr_on_request_task_fast()
{
    ettr_beep();
    int raw_requested = 0;
    
    char* err_msg = "ETTR failed";
    
    /* requires LiveView and ExpSim */
    if (!auto_ettr_prepare_lv(0, 1)) goto err;
    if (!auto_ettr_check_lv()) goto err;
    
    if (get_halfshutter_pressed())
    {
        msleep(500);
        if (get_halfshutter_pressed()) goto err;
    }

#undef AUTO_ETTR_DEBUG
#ifdef AUTO_ETTR_DEBUG
    auto_ettr_vsync_active = 1;
    int raw0 = raw_hist_get_percentile_level(500, GRAY_PROJECTION_GREEN, 2);
    float ev0 = raw_to_ev(raw0);
    int y0 = 100 - ev0 * 20;
    for (int i = 0; i < 100; i++)
    {
        int delta = rand() % 160 - 80;
        auto_ettr_vsync_delta = delta;
        if (!auto_ettr_wait_lv_frames(2)) break;
        
        int raw = raw_hist_get_percentile_level(500, GRAY_PROJECTION_GREEN, 2);
        float ev = raw_to_ev(raw);
        int x = 360 + delta * 3;
        int y = 100 - ev * 24; /* multiplier must be 8 x the one from delta */
        draw_circle(x, y, 2, COLOR_BLUE);
        draw_angled_line(360, y0, 300, 1800-450, COLOR_RED);
        draw_angled_line(360, y0, 300, -450, COLOR_RED);
        draw_angled_line(0, 100, 720, 0, COLOR_RED);
    }
    auto_ettr_vsync_delta = 0;
    auto_ettr_wait_lv_frames(100);
#endif


    NotifyBox(100000, "ETTR...");
    raw_lv_request(); raw_requested = 1;

    if (!raw_update_params())
    {
        err_msg = "Raw error";
        goto err;
    }

    for (int i = 0; i < 5; i++)
    {
        NotifyBox(100000, "ETTR (%d)...", i+1);

        /* make sure the LiveView exposure is settled before reading */
        if (!auto_ettr_wait_lv_expo_settle(30)) break;

        if (fps_get_shutter_speed_shift(160) == 0)
        {
            auto_ettr_vsync_active = 1;
            auto_ettr_vsync_delta = 0;
            for (int k = 0; k < 5; k++)
            {
                if (debug_info) printf("ETTR (%d.%d)\n", i+1, k+1);
                
                /* see how far we are from the ideal exposure */
                int corr = auto_ettr_get_correction();
                if (corr == INT_MIN) break;
                
                /* override the liveview parameters via auto_ettr_vsync_cbr (much faster than via properties) */
                auto_ettr_vsync_delta += corr * 8 / 100;

                /* I'm confident the last iteration was accurate */
                if (corr >= -20 && corr <= 200)
                    break;
                
                /* wait for 2 frames before trying again */
                if (!auto_ettr_wait_lv_frames(2)) goto err;
            }
            auto_ettr_vsync_active = 0;
        }
        else /* FPS override is messing up our plans? fall back to the slow method */
        {
            int corr = auto_ettr_get_correction();
            if (corr == INT_MIN) break;
            auto_ettr_vsync_delta = corr * 8 / 100;
        }

        /* apply the correction via properties */
        int corr = auto_ettr_vsync_delta * 100 / 8;
        int old_iso = get_frame_iso();
        int old_shutter = get_frame_shutter_timer();
        int status = auto_ettr_work(corr);
    
        if (status == ETTR_SETTLED)
        {
            /* looks like it settled */
            break;
        }
        else
        {
            if (i < 4 && status != ETTR_EXPO_LIMITS_REACHED)
            {
                /* here we go again... */
                if (!auto_ettr_wait_lv_expo_change(30, old_iso, old_shutter)) goto err;
            }
            else
            {
                /* or... not? */
                err_msg = status == ETTR_EXPO_LIMITS_REACHED ? "Expo limits reached" : "Whoops";
                goto err;
            }
        }
    }

/* ok: */
    ettr_beep();
    NotifyBoxHide();
    goto cleanup;

err:
    ettr_beep();
    ettr_beep();
    NotifyBox(2000, err_msg);
    goto cleanup;

cleanup:
    auto_ettr_running = 0;
    auto_ettr_vsync_active = 0;
    if (raw_requested) raw_lv_release();
    auto_ettr_prepare_lv(1, 1);
}

static void auto_ettr_step_lv_fast()
{
    if (!auto_ettr || !AUTO_ETTR_TRIGGER_ALWAYS_ON)
        return;
    
    if (!auto_ettr_prepare_lv(0, 0))
        goto end;
    
    if (!auto_ettr_check_lv())
        goto end;
    
    if (get_halfshutter_pressed())
        goto end;

    /* only poll exposure once per second */
    static int aux = INT_MIN;
    if (!should_run_polling_action(1000, &aux))
        goto end;
    
    raw_lv_request();

    if (!raw_update_params())
    {
        NotifyBox(5000, "Raw error");
        goto skip;
    }

    /* make sure the LiveView exposure is settled before reading */
    if (!auto_ettr_wait_lv_expo_settle(30)) goto skip;

    /* get exposure correction */
    int corr = auto_ettr_get_correction();
    
    /* only correct if the image is overexposed by more than 0.2 EV or underexposed by more than 1 EV */
    if (corr != INT_MIN && (corr < -20 || corr > 100))
    {
        if (fps_get_shutter_speed_shift(160) == 0)
        {
            auto_ettr_vsync_active = 1;
            auto_ettr_vsync_delta = 0;
            int k;
            for (k = 0; k < 5; k++)
            {
                /* see how far we are from the ideal exposure */
                if (k > 0) corr = auto_ettr_get_correction();
                if (corr == INT_MIN) break;
                
                /* override the liveview parameters via auto_ettr_vsync_cbr (much faster than via properties) */
                auto_ettr_vsync_delta += corr * 8 / 100;

                /* I'm confident the last iteration was accurate */
                if (corr >= -20 && corr <= 200)
                    break;
                
                /* wait for 3 frames before trying again */
                if (!auto_ettr_wait_lv_frames(2)) break;
            }
            auto_ettr_vsync_active = 0;
        }
        else /* FPS override is messing up our plans? fall back to the slow method */
        {
            auto_ettr_vsync_delta = corr * 8 / 100;
        }

        /* apply the final correction via properties */
        int old_iso = get_frame_iso();
        int old_shutter = get_frame_shutter_timer();

        auto_ettr_work(auto_ettr_vsync_delta * 100 / 8);

        auto_ettr_wait_lv_expo_change(30, old_iso, old_shutter);
    }

skip:
    raw_lv_release();
    
end:
    auto_ettr_prepare_lv(1, 0);
}

static void auto_ettr_on_request_task_slow()
{
    ettr_beep();
    char* err_msg = "ETTR failed";
    
    /* requires LiveView and ExpSim */
    if (!auto_ettr_prepare_lv(0, 1)) goto err;
    if (!auto_ettr_check_lv()) goto err;

    if (get_halfshutter_pressed())
    {
        msleep(500);
        if (get_halfshutter_pressed()) goto err;
    }

    NotifyBox(100000, "ETTR...");
    for (int k = 0; k < 5; k++)
    {
        msleep(500);
        
        raw_lv_request();

        if (!raw_update_params())
        {
            err_msg = "Raw error";
            goto err;
        }

        int corr = auto_ettr_get_correction();
        raw_lv_release();

        if (corr == INT_MIN)
            break;
        
        int status = auto_ettr_work(corr);
        msleep(1000);
        
        if (status == ETTR_SETTLED)
            break;
        
        if (k == 4 || status == ETTR_EXPO_LIMITS_REACHED)
        {
            err_msg = status == ETTR_EXPO_LIMITS_REACHED ? "Expo limits reached" : "Whoops";
            goto err;
        }
    }

/* ok: */
    ettr_beep();
    NotifyBoxHide();
    goto cleanup;

err:
    ettr_beep();
    ettr_beep();
    NotifyBox(2000, err_msg);
    goto cleanup;

cleanup:
    auto_ettr_prepare_lv(1, 1);
    auto_ettr_running = 0;
}

static void auto_ettr_step_lv_slow()
{
    if (!auto_ettr || !AUTO_ETTR_TRIGGER_ALWAYS_ON)
        return;
    
    if (!auto_ettr_check_lv())
        return;
    
    if (get_halfshutter_pressed())
        return;

    /* only update once per 1.5 seconds, so the exposure has a chance to be updated on the LCD */
    static int aux = INT_MIN;
    if (!should_run_polling_action(1500, &aux))
        return;
    
    int corr = INT_MIN;
    raw_lv_request();
    
    if (raw_update_params())
    {
        corr = auto_ettr_get_correction();
    }
    
    raw_lv_release();
    
    if (corr == INT_MIN)
        return;
    
    /* only correct if the image is overexposed by more than 0.2 EV or underexposed by more than 1 EV */
    if (corr >= -20 && corr < 100)
        return;

    int status = auto_ettr_work(corr);

    if (status == ETTR_SETTLED)
    {
        ettr_beep();
    }
}

static void auto_ettr_step_lv()
{
    if (can_set_frame_iso() && can_set_frame_shutter_timer())
        auto_ettr_step_lv_fast();
    else
        auto_ettr_step_lv_slow();
}

static void auto_ettr_on_request_task(int unused)
{
    if (can_set_frame_iso() && can_set_frame_shutter_timer())
        auto_ettr_on_request_task_fast();
    else
        auto_ettr_on_request_task_slow();
}

static unsigned int auto_ettr_keypress_cbr(unsigned int key)
{
    if (!auto_ettr) return 1;
    if (AUTO_ETTR_TRIGGER_PHOTO) return 1;
    if (!display_idle()) return 1;
    if (!auto_ettr_check_pre_lv()) return 1;
    if (lv && !auto_ettr_check_in_lv()) return 1;
    
    if (
            (AUTO_ETTR_TRIGGER_BY_SET && key == MODULE_KEY_PRESS_SET) ||
            /* Half-shutter trigger meters ONLY when already in LiveView. In OVF
             * this task would force_liveview() to meter -- which fired on the
             * finger-release after every shot (release passes back through the
             * half-press detent -> new PRESS_HALFSHUTTER event) and popped the
             * camera into LV. OVF instead meters from the QR image (see
             * PROP_GUI_STATE), with no LiveView interruption. */
            (AUTO_ETTR_TRIGGER_BY_HALFSHUTTER && key == MODULE_KEY_PRESS_HALFSHUTTER && lv) ||
       0)
    {
        if (!auto_ettr_running)
        {
            auto_ettr_running = 1;
            task_create("ettr_task", 0x1c, 0x1000, auto_ettr_on_request_task, (void*) 0);
        }
        if (AUTO_ETTR_TRIGGER_BY_SET) return 0;
    }
    return 1;
}

static MENU_UPDATE_FUNC(auto_ettr_update)
{
    if (lv && ((void*)&raw_lv_request == (void*)&ret_0))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Auto ETTR Does not work in LV on this camera.");

    if (shooting_mode != SHOOTMODE_M && !is_movie_mode() && !is_bulb_mode())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Auto ETTR only works in M, BULB and RAW MOVIE modes.");

    if (lens_info.raw_iso == 0)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Auto ETTR requires manual ISO.");

    if (!lv && !can_use_raw_overlays_photo() && AUTO_ETTR_TRIGGER_PHOTO)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Photo RAW data not available, try in LiveView.");

    menu_checkdep_raw(entry, info);

    if (image_review_time == 0 && AUTO_ETTR_TRIGGER_PHOTO)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Enable image review from Canon menu.");

    if (is_hdr_bracketing_enabled() && !AUTO_ETTR_TRIGGER_BY_SET)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Not compatible with HDR bracketing. Use trigger mode SET.");

    if (lv && AUTO_ETTR_TRIGGER_ALWAYS_ON && !get_expsim())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "In LiveView, this requires ExpSim enabled.");
    
    if (is_continuous_drive() && AUTO_ETTR_TRIGGER_PHOTO)
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Not fully compatible with continuous drive.");

    if (auto_ettr)
    {
        MENU_SET_VALUE(
            AUTO_ETTR_TRIGGER_ALWAYS_ON ? "Always ON" : 
            AUTO_ETTR_TRIGGER_AUTO_SNAP ? "Auto Snap" : 
            AUTO_ETTR_TRIGGER_BY_SET ? "Press SET" : 
            AUTO_ETTR_TRIGGER_BY_HALFSHUTTER ? "HalfShutter" : "err"
        );
    }

    if (!AUTO_ETTR_TRIGGER_PHOTO)
    {
        MENU_SET_HELP("Press the shortcut key to optimize the exposure (ETTR).");
    }
    else if (AUTO_ETTR_TRIGGER_ALWAYS_ON)
    {
        if (lv) MENU_SET_HELP("In LiveView, just wait for exposure to settle, then shoot.");
        else MENU_SET_HELP("Take a test picture (underexposed). Next pic will be ETTR.");
    }
    else if (AUTO_ETTR_TRIGGER_AUTO_SNAP)
    {
        MENU_SET_HELP("Press shutter once. ML will take a pic and retry if needed.");
    }
    
    /* recommended: move AF to back button */
    if (auto_ettr && AUTO_ETTR_TRIGGER_BY_HALFSHUTTER && !is_manual_focus())
        entry->works_best_in = DEP_CFN_AF_BACK_BUTTON;
    else
        entry->works_best_in = 0;
}

static MENU_UPDATE_FUNC(auto_ettr_max_shutter_update)
{
    MENU_SET_VALUE("%s", ettr_format_shutter(auto_ettr_max_shutter));
    
    if (auto_ettr_max_shutter < SHUTTER_30s)
    {
        MENU_SET_RINFO("BULB");
        MENU_SET_WARNING(MENU_WARN_INFO, "For long exposures, enable bulb timer (maybe also intervalometer).");
    }
    
    if (is_intervalometer_running())
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Slowest shutter will be limited by interval time minus 2 seconds.");
    }
    
    if (auto_ettr_adjust_mode == 1)
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Adjust shutter speed from top scrollwheel, outside menu.");
    }
}

static MENU_SELECT_FUNC(auto_ettr_max_shutter_toggle)
{
    if (auto_ettr_adjust_mode == 0)
    {
        /* adjust in 0.5 EV steps, from 1/4096 to 4096 seconds */
        const int tv_max = SHUTTER_1_4000;
        const int tv_min = SHUTTER_1s - EXPO_FULL_STOP * 12;
        auto_ettr_max_shutter = MOD(auto_ettr_max_shutter/4*4 - tv_min + delta * 4, tv_max - tv_min + 4) + tv_min;
    }
}

/* ETTR for P/Av/Tv (auto-exposure) modes -- iteration 1, OVF path.
 *
 * The M-mode path (auto_ettr_step/auto_ettr_work) writes shutter/ISO directly,
 * which Canon overrides in P/Av/Tv. So in the creative auto modes we instead
 * bias Canon's OWN metering with exposure compensation (lens_info.ae, 1/8 EV;
 * lens_set_ae clamps to the body's valid EC range). The correction is metered
 * from the raw highlights exactly like M mode (auto_ettr_get_correction,
 * 1/100 EV), so highlight protection is identical -- only the actuator differs
 * (EC vs shutter/ISO).
 *
 * Feedback loop: each frame is metered as exposed at the current EC, so
 * target_ec = cur_ec + needed_shift; the next frame re-meters the result and
 * converges. Half-damped and step-clamped (max +/-1 EV/shot) to avoid
 * oscillation; one-shot lag in OVF (meter shot N at review -> EC for N+1),
 * same as AI WB. Logs each decision to ML/logs/ettr_ec.txt.
 *
 * SCOPE: iteration 1 is OVF only (metered from the QR/review frame) -- the
 * confirmed field use case. LiveView auto-mode ETTR is a deliberate follow-up.
 * UNVALIDATED on hardware: biasing Canon's auto-exposure can interact with its
 * own metering; verify on-camera before trusting. */
static void auto_ettr_ec_step(void)
{
    if (!auto_ettr) return;
    /* M / movie / bulb are owned by the direct shutter+ISO path, not EC */
    if (shooting_mode == SHOOTMODE_M || is_movie_mode() || is_bulb_mode()) return;
    if (!ai_mode_covered()) return;   /* respects the AI Modes menu (P / +Av/Tv) */

    /* Meter the highlights DIRECTLY with raw_hist_get_percentile_levels at
     * speed 4 -- the exact call/speed the AI-WB pass uses successfully in this
     * QR/review context. We do NOT call auto_ettr_get_correction (it forces a
     * full-res speed=1 scan in non-LV that returns "not ready" here) and do
     * NOT re-run raw_update_params (the 2nd call in the review window fails);
     * we read the buffer WB just read. p999 = brightest highlights (MAX of
     * R/G/B, so any channel near clip counts). */
    int black = raw_info.black_level;
    int white = raw_info.white_level;
    int span  = white - black;
    int pcts[2] = {999, 950};
    int lvls[2] = {-1, -1};
    int meter_ok = (span >= 8) &&
        (raw_hist_get_percentile_levels(pcts, lvls, 2,
            GRAY_PROJECTION_MAX_RGB | GRAY_PROJECTION_DARK_ONLY, 4) == 1) &&
        (lvls[0] >= 0);

    /* TARGET = headroom UNDER clip (not at clip): aim the brightest highlights
     * 0.5..0.70 EV below saturation, more headroom the closer they are to
     * clipping. This is a FIXED POINT -> converges and settles, so it can't
     * run away / over-darken (the property the removed exact-exposure guard
     * was meant to give; that guard never fired anyway with Auto ISO). */
    int cur_ec = lens_info.ae;                /* 1/8 EV, signed */
    int clipp = 0, hl_e2 = 0, headroom = 0, shift_e2 = 0, delta8 = 0, target = cur_ec;
    if (meter_ok)
    {
        int hl = lvls[0] - black; if (hl < 1) hl = 1;
        clipp = hl * 1000 / span;             /* highlight vs saturation, permille (0..1000) */
        headroom = (clipp >= 980) ? 70 : (clipp >= 900) ? 60 : 50;  /* 1/100 EV, by severity */
        hl_e2 = (int)(raw_to_ev(lvls[0]) * 100);  /* highlight EV rel. to clip, x100 (<=0) */
        shift_e2 = (-headroom) - hl_e2;        /* 1/100 EV to move highlight to -headroom */
        delta8 = COERCE((shift_e2 * 8 / 100) / 2, -8, 8);  /* ->1/8 EV, half-damped, <=1EV/shot */
        target = COERCE(cur_ec + delta8, -40, 16);         /* -5..+2 EV; lens_set_ae re-clamps */
    }

    /* log EVERY call to ML/logs (writable), before any early-out */
    FIO_CreateDirectory("ML/logs");
    FILE * _f = FIO_CreateFileOrAppend("ML/logs/ettr_ec.txt");
    if (_f)
    {
        char _l[176];
        int _n = meter_ok
            ? snprintf(_l, sizeof(_l),
                "mode=%d clip=%d hlEV=%d headroom=%d shift=%d curEC=%d d8=%d targetEC=%d\n",
                shooting_mode, clipp, hl_e2, headroom, shift_e2, cur_ec, delta8, target)
            : snprintf(_l, sizeof(_l),
                "mode=%d meter=NA span=%d lvl=%d curEC=%d\n",
                shooting_mode, span, lvls[0], cur_ec);
        if (_n > 0) FIO_WriteFile(_f, _l, _n);
        FIO_CloseFile(_f);
    }

    if (!meter_ok) return;
    if (target != cur_ec) lens_set_ae(target);
}

/* AI WB + data logging for OVF shooters: there is no LiveView raw to meter,
 * so meter the picture just taken during Canon image review -- the same
 * reactive pattern as ETTR's photo path. The captured photo is actually the
 * best WB source there is (it IS the scene). Runs in a task because the raw
 * buffer can lag the QR event (retry, like auto_ettr_photo_task). */
static volatile int ai_qr_running = 0;
static void ai_qr_task(int unused)
{
    int ok = 0;
    for (int i = 0; i < 10 && !ok; i++)
    {
        ok = raw_update_params();
        if (!ok) msleep(50);
    }
    if (ok)
    {
        if (ai_white_balance)
            ai_white_point_wb(ai_wb_warmth);
        if (ai_data_logging && ai_lut_log())
            ai_logged_press = 1;   /* the LV path won't log this press again */
        /* P/Av/Tv exposure via EC (M mode is handled by auto_ettr_step). Meters
         * the just-taken frame and biases the NEXT shot's exposure comp. */
        auto_ettr_ec_step();
    }
    ai_qr_running = 0;
}

PROP_HANDLER(PROP_GUI_STATE)
{
    if (buf[0] == GUISTATE_QR)
    {
        /* Half-shutter trigger too: in OVF, meter from the picture just taken
         * (photo path, no LV) and silently refine exposure for the next shot.
         * Requires Canon image review enabled (same as the other photo triggers). */
        if (AUTO_ETTR_TRIGGER_PHOTO || AUTO_ETTR_TRIGGER_BY_HALFSHUTTER)
            auto_ettr_step();

        /* AI WB + logging from the just-taken photo (OVF path) */
        if (!lv && ai_mode_covered() && (ai_white_balance || ai_data_logging || auto_ettr)
            && !ai_qr_running)
        {
            ai_qr_running = 1;
            task_create("ai_qr_task", 0x1c, 0x1000, ai_qr_task, (void*) 0);
        }
    }
}

MENU_UPDATE_FUNC(auto_ettr_intervalometer_warning)
{
    if (auto_ettr && image_review_time == 0 && AUTO_ETTR_TRIGGER_PHOTO)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Auto ETTR: enable image review from Canon menu.");
}

void auto_ettr_intervalometer_wait()
{
    if (auto_ettr && image_review_time)
    {
        /* make sure auto ETTR has a chance to run (it's triggered by prop handler on QR mode) */
        /* timeout: a bit more than exposure time, to handle long expo noise reduction */
        for (int i = 0; i < raw2shutter_ms(ettr_get_current_raw_shutter())/100; i++)
        {
            if (gui_state == GUISTATE_PLAYMENU || gui_state == GUISTATE_QR) break;
            msleep(150);
        }
        msleep(500);
    }
}

/* Auto ISO Optimizer:
 * When the user selects Canon "Auto ISO" (lens_info.raw_iso == 0) in P/Av/Tv/M,
 * take over at highest priority: enable ETTR, HTP and ALO, and switch to manual
 * ISO at the floor so ETTR becomes the auto-exposure engine.
 *
 * Edge-triggered on purpose: we act once when Auto ISO is engaged, then respect
 * the user afterwards. So if you later turn HTP off (Canon Q menu or ML), the
 * ETTR floor naturally drops from ISO 200 back to ISO 100 (via MIN_ISO), exactly
 * as requested. The 6D has no native ISO 50 for RAW, so 100 is the real floor. */
static void auto_iso_optimizer_step()
{
    if (!auto_iso_optimizer) return;

    static int was_auto_iso = 0;
    int is_auto_iso = (lens_info.raw_iso == 0);
    int mode_ok = ai_mode_covered();

    if (is_auto_iso && mode_ok && !was_auto_iso)
    {
        /* AI-LUT (single source of adjustment): apply the learned per-scene
         * HTP/ALO/WB via ML's OWN setters and get the learned starting ISO.
         * Falls back to the previous hardcoded HTP+ALO when no LUT row applies,
         * so nothing regresses. The metered ETTR still owns the exposure push.
         * Wrap with raw_lv_request so the lookup uses the RAW light level even
         * when AI Data Logging (which otherwise holds raw LV) is off. */
        int ai_raw_wrap = lv && ((void*)&raw_lv_request != (void*)&ret_0);
        if (ai_raw_wrap) raw_lv_request();
        int ai_iso = ai_lut_apply();
        if (ai_raw_wrap) raw_lv_release();
        if (ai_iso <= 0)
        {
            /* No LUT / no match: enable HTP + ALO as before. HTP also raises the
             * camera's OWN Auto-ISO floor to 200 (Canon disables ISO 100 under
             * HTP); with HTP off the floor is 100. */
            set_htp(1);
            set_alo(ALO_STD);
        }

        if (shooting_mode == SHOOTMODE_M)
        {
            /* Only in M can ETTR actually drive exposure -- it needs control
             * of ISO+shutter, which Canon owns in P/Av/Tv (ETTR bails out
             * there, see auto_ettr_step / auto_ettr_check_pre_lv). So only in M
             * do we hand over: arm ETTR, half-shutter meters, ISO at the learned
             * (or MIN_ISO) floor. MIN_ISO = get_htp() ? 80 (ISO200) : 72 (ISO100). */
            auto_ettr = 1;
            auto_ettr_trigger = 3;   /* Half-Shutter */
            lens_set_rawiso(ai_iso > 0 ? ai_iso_to_raw(ai_iso) : MIN_ISO);
        }
        /* P/Av/Tv: deliberately DO NOT force a manual ISO. Canon's native Auto
         * ISO is already dynamic; we just let it run, now floored at 200 (HTP)
         * or 100. Forcing manual ISO here only pins a fixed value, because ETTR
         * cannot run outside M -- that was the "stuck at fixed ISO" bug. */
    }

    /* update edge state */
    if (!is_auto_iso) was_auto_iso = 0;
    else if (mode_ok) was_auto_iso = 1;
}

static unsigned int auto_ettr_polling_cbr()
{
    auto_iso_optimizer_step();

    /* The whole AI system only acts in the user-selected shooting modes. */
    int ai_on = ai_mode_covered();

    /* Keep raw LV available while logging OR white-balancing so the RAW
     * histogram (true exposure / per-channel white point) is readable.
     * Reference-counted; balanced request/release. */
    static int ai_raw_req = 0;
    int ai_want_raw = ai_on && (ai_data_logging || ai_white_balance) && lv
                      && ((void*)&raw_lv_request != (void*)&ret_0);
    if (ai_want_raw && !ai_raw_req) { raw_lv_request(); ai_raw_req = 1; }
    else if (!ai_want_raw && ai_raw_req) { raw_lv_release(); ai_raw_req = 0; }

    /* Item 1: white-point WB, once per half-press (retry until raw is ready). */
    static int ai_wb_done = 0;
    int ai_metered = 0;   /* did a raw-metering action run this poll? */
    if (!get_halfshutter_pressed()) ai_wb_done = 0;
    else if (ai_on && ai_white_balance && !ai_wb_done && ai_white_point_wb(ai_wb_warmth))
    {
        ai_wb_done = 1;
        ai_metered = 1;
    }

    /* Item 2: per-lens tune. The ev-bias/WB-trim columns load on every lens
     * change while the AI is active (they steer ETTR/AI-WB, no toggle of their
     * own); the picstyle push additionally honors the AI Picture Tune toggle. */
    static int ai_tune_lens = -1;   /* lens the tbl was loaded for */
    static int ai_pt_lens = -1;     /* lens the picstyle was applied for */
    int ai_lid = (int) lens_info.lens_id;
    if (!ai_on)
    {
        if (ai_tune_lens != -1) ai_lens_tune_reset();   /* AI off -> no bias */
        ai_tune_lens = ai_pt_lens = -1;
    }
    else
    {
        if (ai_lid != ai_tune_lens)
        {
            ai_lens_tune_load();
            ai_tune_lens = ai_lid;
        }
        if (ai_picture_tune_en && ai_lid != ai_pt_lens)
        {
            ai_picture_tune();
            ai_pt_lens = ai_lid;
        }
        if (!ai_picture_tune_en)
            ai_pt_lens = -1;   /* re-apply after the user toggles it back on */
    }

    /* AI data logging: log ONCE per half-press, but only once the histogram is
     * built AND ISO is resolved -- both lag the rising edge during metering, so
     * logging on the edge caught inconsistent snapshots (ISO=0 or LightLevel=128
     * fallback). Independent of the optimizer, so AI-off ground-truth passes log
     * native metering. Histogram is only meaningful in LiveView. */
    if (!get_halfshutter_pressed())
    {
        ai_logged_press = 0;
    }
    else if (ai_on && ai_data_logging && !ai_logged_press)
    {
        int riso = lens_info.raw_iso ? lens_info.raw_iso : lens_info.raw_iso_auto;
        /* ai_lut_log() returns 0 until a light source (raw metering preferred,
         * display histogram fallback) is ready -- retry next poll. This no
         * longer requires the ML histogram overlay to be enabled. */
        if (riso > 0 && ai_lut_log())
        {
            ai_logged_press = 1;
            ai_metered = 1;
        }
    }

    if (lv && NOT_RECORDING && ((void*)&raw_lv_request != (void*)&ret_0))
        auto_ettr_step_lv();

    /* Keep the ML console off LiveView: raw metering (raw.c buffer messages) and
     * ETTR debug prints go to the console and otherwise pile up over the screen.
     * Hide it ONCE, only right after a metering action actually ran (that's what
     * prints) -- NOT every poll: console_hide() does msleep(100)+redraw(), so
     * calling it every cycle forced a constant full redraw that flickered the
     * GUI/menu and stalled this task. Never fight the menu while it's open. */
    if (ai_metered && !gui_menu_shown())
        console_hide();

    /* NOTE: the RaZStudio data-export writers (.ml6d per-shot sidecar +
     * ml_export.json session file + dual-ISO MakerNote patch) were REMOVED
     * 2026-07-26. StudioRoom's sidecar consumer is disabled in its own source
     * ("MLExtendedIntelligence disabled -- sidecar-driven corrections caused
     * issues"); the confirmed-working path reads standard Canon EXIF only
     * (LensID -> lens tune, ISO -> NR, ColorTemperature -> WB). So these
     * writers had no consumer -- and were failing to write to ML/DATA anyway
     * (FIO_CreateFile/CreateFileOrAppend returning NULL for that directory).
     * The firmware's on-camera WB/exposure/lens adjustments already land in
     * the standard CR2 EXIF StudioRoom reads, so nothing extra is needed. */

    return 0;
}

static MENU_SELECT_FUNC(debug_info_toggle)
{
    debug_info = !debug_info;
    /* fixme: kinda ugly */
    if (debug_info) console_show();
    else console_hide();
}

/* --- AI Lens Tune: on-camera editor for ML/models/lens_tune.tbl ------------
 * Values shown/edited are the LIVE tune for the mounted lens (auto-loaded on
 * every lens swap). "Save for this lens" persists them as that lens's row,
 * marked #user so offline training preserves hand-tuned rows. */

static MENU_UPDATE_FUNC(ai_lens_tune_menu_update)
{
    if (lens_info.name[0])
        MENU_SET_VALUE("%s", lens_info.name);
    else
        MENU_SET_VALUE("(no lens)");
    MENU_SET_RINFO("%s",
        ai_lens_src == AI_LENS_SRC_USER  ? "custom" :
        ai_lens_src == AI_LENS_SRC_TABLE ? "table"  : "neutral");
    if (!lens_info.lens_id)
        MENU_SET_WARNING(MENU_WARN_INFO,
            "No electronic lens ID: edits use the default row (id 0).");
}

static MENU_UPDATE_FUNC(ai_lens_ev_update)
{
    int mev = ai_lens_ev8 * 125;   /* 1/8 EV -> milli-EV, exact */
    MENU_SET_VALUE("%s%d.%03d EV", mev < 0 ? "-" : "+", ABS(mev)/1000, ABS(mev)%1000);
    if (ai_lens_ev8)
        MENU_SET_RINFO("ETTR");
}

static MENU_UPDATE_FUNC(ai_lens_wbr_update)
{
    int v = ai_lens_wbr * 100 / 1024;
    MENU_SET_VALUE("x%d.%02d", v/100, v%100);
    if (ai_lens_wbr != 1024)
        MENU_SET_RINFO(ai_lens_wbr < 1024 ? "warmer" : "cooler");
}

static MENU_UPDATE_FUNC(ai_lens_wbb_update)
{
    int v = ai_lens_wbb * 100 / 1024;
    MENU_SET_VALUE("x%d.%02d", v/100, v%100);
    if (ai_lens_wbb != 1024)
        MENU_SET_RINFO(ai_lens_wbb > 1024 ? "warmer" : "cooler");
}

/* WB trims step in ~1.6% increments; a click per unit would take forever */
static MENU_SELECT_FUNC(ai_lens_wb_toggle)
{
    menu_numeric_toggle(priv, delta * 16, 512, 2048);
}

static MENU_SELECT_FUNC(ai_lens_tune_save_select)
{
    if (ai_lens_tune_save())
        NotifyBox(2000, "Saved: lens id %d", (int) lens_info.lens_id);
    else
        NotifyBox(2000, "Save FAILED (card / table too big?)");
    if (ai_picture_tune_en) ai_picture_tune();   /* make edits visible now */
}

static MENU_SELECT_FUNC(ai_lens_tune_reload_select)
{
    if (ai_lens_tune_load())
        NotifyBox(2000, "Reloaded from lens_tune.tbl");
    else
        NotifyBox(2000, "No row for this lens: neutral");
    if (ai_picture_tune_en) ai_picture_tune();
}

static struct menu_entry ettr_menu[] =
{
    {
        .name = "Auto ETTR",
        .priv = &auto_ettr,
        .update = auto_ettr_update,
        .max = 1,
        .help  = "Auto expose to the right when you shoot RAW.",
        .submenu_width = 710,
        .children =  (struct menu_entry[]) {
            {
                .name = "Trigger mode",
                .priv = &auto_ettr_trigger,
                .max = 3, // NOTE: Modifed by the module init task to disable ETTR in LV if not supported
                .choices = CHOICES("Always ON", "Auto Snap", "Press SET", "Half-Shutter"),
                .help  = "When should the exposure be adjusted for ETTR:",
                .help2 = "Always ON: when you take a pic, or continuously in LiveView\n"
                         "Auto Snap: after u take a pic,trigger another pic if needed\n"
                         "Press SET: meter for ETTR when you press SET (LiveView)\n"
                         "Half-Shutter: meter for ETTR on every half-shutter press\n"
            },
            {
                .name = "Slowest shutter",
                .priv = &auto_ettr_max_shutter,
                .select = auto_ettr_max_shutter_toggle,
                .update = auto_ettr_max_shutter_update,
                .min = 16,
                .max = 152,
                .icon_type = IT_PERCENT,
                .help = "Slowest shutter speed for ETTR (longest exposure time)."
            },
            {
                .name = "Exposure target",
                .priv = &auto_ettr_target_level,
                .min = -4,
                .max = 0,
                .choices = CHOICES("-4 EV", "-3 EV", "-2 EV", "-1 EV", "-0.5 EV"),
                .help = "Exposure target for ETTR. Recommended: -0.5 or -1 EV.",
                .advanced = 1,
            },
            {
                .name = "Highlight ignore",
                .priv = &auto_ettr_ignore,
                .min = 0,
                .max = 500,
                .unit = UNIT_PERCENT_x10,
                .icon_type = IT_PERCENT,
                .help  = "How many bright pixels are allowed above the target level.",
                .help2 = "Use this to allow spec(ta)cular highlights to be clipped.",
            },
            {
                .name = "Allow clipping",
                .priv = &auto_ettr_clip,
                .max = 2,
                .choices = CHOICES("OFF", "Green channel", "Any channel"),
                .help = "Choose what color channels are allowed to be clipped.",
                .advanced = 1,
            },
            {
                .name = "Midtone SNR limit",
                .priv = &auto_ettr_midtone_snr_limit,
                .min = 0,
                .max = 8,
                .choices = CHOICES("OFF", "1 EV", "2 EV", "3 EV", "4 EV", "5 EV", "6 EV", "7 EV", "8 EV"),
                .help  = "Stop underexposing when at least half of the image gets",
                .help2 = "noisier than selected SNR => will clip more highlights.",
                .depends_on = DEP_MANUAL_ISO,
            },
            {
                .name = "Shadow SNR limit",
                .priv = &auto_ettr_shadow_snr_limit,
                .min = 0,
                .max = 6,
                .choices = CHOICES("OFF", "1 EV", "2 EV", "3 EV", "4 EV", "5 EV", "6 EV"),
                .help  = "Stop underexposing when at least 5% of the image gets",
                .help2 = "noisier than selected SNR => will clip more highlights.",
                .depends_on = DEP_MANUAL_ISO,
            },
            {
                .name = "Link to Canon shutter",
                .priv = &auto_ettr_adjust_mode,
                .max = 1,
                .help = "Hack to adjust slowest shutter from main dial.",
                .advanced = 1,
            },
            {
                .name = "Link to Dual ISO",
                .priv = &auto_ettr_dual_iso_link,
                .max = 1,
                .help  = "Let ETTR change DualISO settings so you get the SNR values",
                .help2 = "in mids & shadows. It will disable dual ISO if not needed.",
            },
            {
                .name = "Show metered areas",
                .priv = &show_metered_areas,
                .max = 1,
                .help =  "Show where the white point and the SNR levels are metered",
                .help2 = "(what exactly is considered highlight, midtone and shadow).",
                .advanced = 1,
            },
            {
                .name = "Allow beeps",
                .priv = &auto_ettr_allow_beeps,
                .max = 1,
                .help =  "Make status beeps (1 = OK, 2 = need more pictures, 3 = error).",
                .advanced = 1,
            },
            {
                .name = "AI Modes",
                .priv = &ai_modes,
                .max = 1,
                .choices = CHOICES("P, M", "P, M, Av, Tv"),
                .help  = "Which shooting modes the whole AI system acts in.",
                .help2 = "Av keeps your aperture; Tv keeps your shutter; M full control.",
            },
            {
                .name = "Auto ISO Optimizer",
                .priv = &auto_iso_optimizer,
                .max = 1,
                .help  = "ISO=Auto+HTP+ALO. M: ETTR (half-shutter). P/Av/Tv: Canon AutoISO.",
                .help2 = "Floor ISO 200 w/HTP, 100 w/o. ETTR push only in M mode.",
            },
            {
                .name = "AI Data Logging",
                .priv = &ai_data_logging,
                .max = 1,
                .help  = "Log sensor data to ML/logs/unified_log.txt on half-press.",
                .help2 = "For AI-LUT training. Use in LiveView. Turn off for normal use.",
            },
            {
                .name = "AI White Balance",
                .priv = &ai_white_balance,
                .max = 1,
                .help  = "Adaptive WB: bright-pixels + gray-world, confidence blended.",
                .help2 = "Dim/clipped highlights are NOT forced white. Glides per press.",
            },
            {
                .name = "AI WB Warm/Cool",
                .priv = &ai_wb_warmth,
                .max = 8,
                .choices = CHOICES("B4 cool", "B3 cool (shade)", "B2 cool", "B1 cool", "Neutral", "A1 warm", "A2 warm (skin)", "A3 warm", "A4 warm"),
                .help  = "WB ambience via Canon's Amber/Blue shift (~5 mireds/step); WB gains stay neutral.",
                .help2 = "Cool (B) <- Neutral -> Warm (A). Golden light auto-adds +1 amber so sun stays warm.",
            },
            {
                .name = "AI Picture Tune",
                .priv = &ai_picture_tune_en,
                .max = 1,
                .help  = "Per-lens contrast/saturation from ML/models/lens_tune.tbl.",
                .help2 = "JPEG only (picstyle does not affect RAW). Off by default.",
            },
            {
                .name = "AI Lens Tune",
                .update = ai_lens_tune_menu_update,
                .select = menu_open_submenu,
                .icon_type = IT_SUBMENU,
                .help  = "View/edit this lens's tune row (auto-loaded on lens swap).",
                .help2 = "Normalize exposure/WB/look across lenses. Save writes the table.",
                .children = (struct menu_entry[]) {
                    {
                        .name = "Lens contrast",
                        .priv = &ai_lens_c,
                        .min = -4, .max = 4,
                        .help = "Picstyle contrast offset for this lens (JPEG only).",
                    },
                    {
                        .name = "Lens saturation",
                        .priv = &ai_lens_s,
                        .min = -4, .max = 4,
                        .help = "Picstyle saturation offset for this lens (JPEG only).",
                    },
                    {
                        .name = "Lens color tone",
                        .priv = &ai_lens_t,
                        .min = -4, .max = 4,
                        .help = "Picstyle color tone for this lens (JPEG only).",
                    },
                    {
                        .name = "Exposure bias",
                        .priv = &ai_lens_ev8,
                        .update = ai_lens_ev_update,
                        .min = -24, .max = 8,
                        .help  = "Shifts the ETTR exposure target on this lens, 1/8 EV steps.",
                        .help2 = "Negative = protect highlights (for lenses that bloom them).",
                    },
                    {
                        .name = "WB red trim",
                        .priv = &ai_lens_wbr,
                        .update = ai_lens_wbr_update,
                        .select = ai_lens_wb_toggle,
                        .min = 512, .max = 2048,
                        .help  = "Trims AI White Balance red gain for this lens's color cast.",
                        .help2 = "Below 1.00 = warmer. Only acts while AI White Balance is ON.",
                    },
                    {
                        .name = "WB blue trim",
                        .priv = &ai_lens_wbb,
                        .update = ai_lens_wbb_update,
                        .select = ai_lens_wb_toggle,
                        .min = 512, .max = 2048,
                        .help  = "Trims AI White Balance blue gain for this lens's color cast.",
                        .help2 = "Above 1.00 = warmer. Only acts while AI White Balance is ON.",
                    },
                    {
                        .name = "Save for this lens",
                        .select = ai_lens_tune_save_select,
                        .help  = "Write these values to lens_tune.tbl as this lens's row.",
                        .help2 = "Marked #user: offline training keeps your hand-tuned rows.",
                    },
                    {
                        .name = "Reload from table",
                        .select = ai_lens_tune_reload_select,
                        .help = "Discard edits; reload this lens's saved row (or neutral).",
                    },
                    MENU_EOL,
                },
            },
            {
                .name = "Show debug info",
                .priv = &debug_info,
                .select = debug_info_toggle,
                .max = 1,
                .help = "For camera nerds.",
                .advanced = 1,
            },
            MENU_ADVANCED_TOGGLE,
            MENU_EOL,
        },
    },
};

static unsigned int ettr_init()
{
    if ((void*)&raw_lv_request == (void*)&ret_0)
    {
        auto_ettr_trigger  = auto_ettr_trigger > 1 ? 0 : auto_ettr_trigger;
        ettr_menu[0].children[0].max = 1;
    }
    menu_add("Expo", ettr_menu, COUNT(ettr_menu));
    /* ml_export.json session writer removed 2026-07-26 (no StudioRoom consumer;
     * see the note in auto_ettr_polling_cbr). */
#if ETTR_VIDEO_GYRO_METADATA
    gyro_bridge_init();          /* Initialize gyro bridge (probes for external IMU) */
    mlv_metadata_init();         /* Register MLV chunk CBR (no-op if mlv_rec not loaded) */
#endif
    return 0;
}

static unsigned int ettr_deinit()
{
    return 0;
}

#if ETTR_VIDEO_GYRO_METADATA
/* Electronic level property handler — forwards data to gyro bridge.
 * This captures Canon's accelerometer pitch/roll data at ~5 Hz for
 * the mlc.level() Lua binding (Gyroflow orientation metadata). */
PROP_HANDLER(PROP_ROLLING_PITCHING_LEVEL)
{
    extern void gyro_level_prop_handler(unsigned int property, void *buf, unsigned int len);
    gyro_level_prop_handler(property, (void*)buf, len);
}
#endif

MODULE_INFO_START()
    MODULE_INIT(ettr_init)
    MODULE_DEINIT(ettr_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_VSYNC_SETPARAM, auto_ettr_vsync_cbr, 0)
    MODULE_CBR(CBR_KEYPRESS, auto_ettr_keypress_cbr, 0)
    MODULE_CBR(CBR_SHOOT_TASK, auto_ettr_polling_cbr, 0)
MODULE_CBRS_END()

MODULE_PROPHANDLERS_START()
    MODULE_PROPHANDLER(PROP_GUI_STATE)
#if ETTR_VIDEO_GYRO_METADATA
    MODULE_PROPHANDLER(PROP_ROLLING_PITCHING_LEVEL)
#endif
MODULE_PROPHANDLERS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(auto_ettr)
    MODULE_CONFIG(auto_iso_optimizer)
    MODULE_CONFIG(ai_data_logging)
    MODULE_CONFIG(ai_white_balance)
    MODULE_CONFIG(ai_wb_warmth)
    MODULE_CONFIG(ai_picture_tune_en)
    MODULE_CONFIG(ai_modes)
    MODULE_CONFIG(auto_ettr_trigger)
    MODULE_CONFIG(auto_ettr_ignore)
    MODULE_CONFIG(auto_ettr_target_level)
    MODULE_CONFIG(auto_ettr_max_shutter)
    MODULE_CONFIG(auto_ettr_clip)
    MODULE_CONFIG(auto_ettr_adjust_mode)
    MODULE_CONFIG(auto_ettr_midtone_snr_limit)
    MODULE_CONFIG(auto_ettr_shadow_snr_limit)
    MODULE_CONFIG(auto_ettr_dual_iso_link)
    MODULE_CONFIG(auto_ettr_allow_beeps)
MODULE_CONFIGS_END()
