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

/* Floor on how far auto_ettr_ec_step() (P/Av/Tv ETTR) may darken via exposure
 * compensation, relative to Canon's own auto-exposure metering (lens_info.ae
 * == 0). Stored in HALF-EV steps (value 3 = 3 * 0.5 = "-1.5 EV limit") so the
 * default can land on a half-stop; whole-EV-only storage couldn't express
 * that. Default 3 = -1.5 EV, per field preference.
 *
 * FIELD BUG 2026-07-27: unlike the original M-mode ETTR (auto_ettr_work),
 * which already limits its correction against a midtone/shadow noise floor
 * (auto_ettr_midtone_snr_limit / auto_ettr_shadow_snr_limit below), the P/Av/Tv
 * EC corrector only ever meters the brightest 0.1% of pixels (p99.9) with NO
 * such floor. A persistent small bright element in frame (a window, a light
 * fixture, sky in a doorway) that can never be brought under headroom without
 * crushing the rest of the scene will make it darken shot after shot with
 * nothing to stop it short of the hardware EC limit (-5 EV) -- reported in the
 * field as exposure marching past -2 EV on Canon's own meter and continuing.
 * This is a blunt safety cap (not a real midtone-SNR model like the M-mode
 * path has); it stops the runaway but doesn't replace a proper fix. */
static CONFIG_INT("auto.ettr.ai.ec.floor", ai_ec_floor_halfstop, 3);

/* AI Shutter Control: auto-derive ETTR's "Slowest shutter" limit from the
 * mounted lens's LIVE focal length (lens_info.focal_len, mm -- reported by
 * the lens over the electronic mount, so this tracks zooming in real time),
 * using the classic handholding reciprocal rule: slowest safe shutter =
 * 1/focal_length seconds (e.g. 50mm -> never slower than 1/50s). When this
 * is on, auto_ettr_max_shutter (the "Slowest shutter" menu value) is
 * overwritten every poll instead of using the fixed value the user set
 * there -- same override relationship AI Picture Tune has with the base
 * picstyle controls. Falls back to leaving auto_ettr_max_shutter alone
 * (whatever the user set) if the lens reports no focal length (manual/
 * adapted glass with no electronic contacts). Full-frame body (6D, crop
 * factor 1.0, see AI_SENSOR_CROP_FACTOR) -- no crop multiplier needed. Does
 * not account for image stabilization; the plain rule is intentionally
 * conservative. */
static CONFIG_INT("auto.ettr.ai.shutter.reciprocal", ai_shutter_reciprocal, 0);

/* Manual Lens Profiles: up to 10 user-registered prime-lens focal lengths
 * (+ max aperture) for lenses with no electronic mount -- manual/adapted
 * glass reports lens_info.focal_len == 0, so AI Shutter Control above has
 * nothing to read from it. Slot data persists the same way as any other ML
 * setting (CONFIG_ARRAY_ELEMENT -- same mechanism as HDR's extended-bracket
 * table in src/hdr.c). "Active" selects which slot, if any, stands in for
 * the missing electronic focal-length report; 0 = none (AI Shutter Control
 * leaves auto_ettr_max_shutter untouched on a lens with no CPU contacts and
 * no active profile, same behavior as before this feature existed).
 *
 * Max aperture is stored per slot but NOT currently consumed by any
 * calculation -- AI Shutter Control only needs focal length. Recorded
 * because it was explicitly requested and is useful documentation of the
 * mounted lens; a real use (e.g. gating a wide-open metering assumption)
 * can be wired in later if a clear need shows up.
 *
 * NOTE: listing Canon's own internally-registered lens-correction database
 * (peripheral illumination / aberration correction) was also requested, but
 * is NOT implemented -- there is no exposed API for that data in this
 * Magic Lantern/DryOS codebase (only ML's own unrelated LiveView
 * vignetting-correction preview feature exists, src/lv-img-engio.c). */
#define AI_ML_SLOTS 10
#define AI_ML_APERTURES 11
/* f/1.0 .. f/6.3 *10 -- index matches the "Max aperture" menu CHOICES */
static const int ai_ml_aperture_x10[AI_ML_APERTURES] =
    { 10, 12, 14, 18, 20, 28, 35, 40, 45, 56, 63 };

static int ai_ml_focal[AI_ML_SLOTS];    /* mm, 0 = empty slot */
static int ai_ml_ap_idx[AI_ML_SLOTS];   /* index into ai_ml_aperture_x10 */

CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.0", ai_ml_focal, 0, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.1", ai_ml_focal, 1, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.2", ai_ml_focal, 2, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.3", ai_ml_focal, 3, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.4", ai_ml_focal, 4, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.5", ai_ml_focal, 5, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.6", ai_ml_focal, 6, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.7", ai_ml_focal, 7, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.8", ai_ml_focal, 8, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.focal.9", ai_ml_focal, 9, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.0", ai_ml_ap_idx, 0, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.1", ai_ml_ap_idx, 1, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.2", ai_ml_ap_idx, 2, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.3", ai_ml_ap_idx, 3, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.4", ai_ml_ap_idx, 4, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.5", ai_ml_ap_idx, 5, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.6", ai_ml_ap_idx, 6, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.7", ai_ml_ap_idx, 7, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.8", ai_ml_ap_idx, 8, 0);
CONFIG_ARRAY_ELEMENT("auto.ettr.ai.ml.ap.9", ai_ml_ap_idx, 9, 0);

/* Which slot the "Focal length" / "Max aperture" editors below point at. */
static CONFIG_INT("auto.ettr.ai.ml.slot", ai_ml_slot, 1);

/* Active profile for AI Shutter Control's manual-lens fallback: 0 = none. */
static CONFIG_INT("auto.ettr.ai.ml.active", ai_ml_active, 0);

/* AI Flash Mode: 0 = Off, 1 = E-TTL, 2 = Manual. A pure workflow toggle --
 * it does NOT program the flash itself (E-TTL vs. Manual power stays on the
 * speedlite/camera flash menu, exactly as always); it only tells this module
 * which system is handling the subject's light today, so it knows to force
 * Canon's own METERING MODE to Partial (see ai_flash_metering_step). With
 * flash doing the fill, the ambient meter should read the subject/background
 * in isolation for a correct base exposure -- evaluative/center-weighted
 * metering gets skewed by the flash's own preflash and by whatever's outside
 * the subject, and the user is going to dial the flash's OWN power to fill
 * the subject/center regardless. Off (default) -> this feature never touches
 * the metering mode; whatever Canon (or the user) has set is left alone. */
static CONFIG_INT("auto.ettr.ai.flash.mode", ai_flash_mode, 0);

/* Which shooting modes the whole AI system acts in.
 *   0 = P, M        (default)
 *   1 = P, M, Av, Tv
 *
 * Per-mode control matrix (2026-07-27 redesign). Every covered mode gets the
 * SAME base package -- ALO-level-or-HTP auto-select (ai_apply_alo_htp, one or
 * the other, never both -- see there), and AI Picture Tune's per-lens
 * contrast/saturation -- plus a mode-specific slice of exposure control:
 *   M  : AI owns ISO+shutter outright (the metered ETTR path, auto_ettr_work).
 *   Av : base package, PLUS shutter -- aperture stays the user's; AI reaches
 *        shutter through auto_ettr_ec_step's exposure-compensation bias,
 *        which in Av Canon's own AE resolves by moving the shutter.
 *   Tv : base package, PLUS aperture -- shutter stays the user's; same EC
 *        bias, which in Tv Canon resolves by moving the aperture.
 *   P  : base package, and EVERYTHING -- Canon's Program AE resolves the EC
 *        bias by moving shutter and/or aperture along the program line.
 * In every mode the AI never touches the parameter the user explicitly owns
 * (Av's aperture, Tv's shutter) -- it only ever biases the METERED target,
 * never writes shutter/aperture directly outside M. */
static CONFIG_INT("auto.ettr.ai.modes", ai_modes, 0);

/* True if the AI system should act in the current shooting mode. */
static int ai_mode_covered(void)
{
    int m = shooting_mode;
    if (m == SHOOTMODE_M || m == SHOOTMODE_P) return 1;
    if (ai_modes == 1 && (m == SHOOTMODE_AV || m == SHOOTMODE_TV)) return 1;
    return 0;
}

/* --------------------------------------------------------------------------
 * ALO/HTP Scene Optimizer (2026-08-08, .kiro/specs/alo-htp-optimizer)
 *
 * Replaces the float scene_dr/highlight_headroom chooser that lived here
 * (2026-07-27) with an INTEGER-ONLY classifier driven straight off the raw
 * histogram percentiles the AI path already meters for free:
 *
 *   P90 green (ai_light_level() return value, 0..255) -- overall brightness
 *   P99 green (ai_raw_p99, 0..255)                    -- highlight peak
 *
 * Three zones, each with a fixed ALO level, HTP state, and a MINIMAL single
 * Canon step (8 raw units = 1/3 EV) of exposure -- deliberately far less
 * aggressive than the metered ETTR path:
 *
 *   P99 >= 240            -> Bright: HTP on,  ALO Low, ISO -1/3 EV
 *                            (at the ISO 100 floor, P mode nudges the shutter
 *                             1/3 EV faster instead; Av/Tv/M leave it alone)
 *   else P90 < 80         -> Dark:   HTP off, ALO Low/Std/High by how dark,
 *                            ISO +1/3 EV
 *   else                  -> Normal: HTP on,  ALO Std, exposure untouched
 *
 * Bright is tested FIRST: a scene can be dark in the midtones and still have
 * clipping speculars, and blown highlights are unrecoverable while shadows
 * are not. HTP stays OFF in the Dark zone on purpose -- its ISO 200 floor
 * costs exactly the shadow SNR that zone needs.
 *
 * Mutual exclusion is hardware, not just policy: Canon's PROP_ALO reports
 * "actual ALO setting, maybe disabled by HTP" (see GENERIC_GET_ALO in
 * src/cfn-generic.h), so set_htp() is always written BEFORE set_alo() -- a
 * Bright/Normal -> Dark transition must clear HTP before the ALO level it
 * was suppressing can take effect.
 *
 * Aperture is NEVER touched, in any mode. Shutter is only ever touched in P,
 * and only as the Bright-zone-at-ISO-floor fallback. Every write is skipped
 * when the value already matches, so a static scene stops writing Canon
 * properties after the first press.
 *
 * THRESHOLD CALIBRATION 2026-08-08 (field data, NOT the spec's opening values).
 * Measured against the 40 raw-metered half-presses in ML/logs/unified_log.txt
 * (the only records where LightSrc=raw, i.e. where these percentiles are real
 * -- the other 189 fell back to the display histogram and carry P99 = -1).
 *
 * The spec's DARK_THRESHOLD 80 was written against a 0..255 scale assumed to
 * behave like a JPEG histogram. It does not: ai_light_level() meters the
 * LiveView RAW frame with GRAY_PROJECTION_DARK_ONLY, whose P90 ran a MEDIAN of
 * 36 across the session (p75 = 60). So "P90 < 80" fired on 77.5% of presses --
 * the Dark zone, which is the one and only place this function ADDS exposure
 * (ISO +1/3 EV) and the one place it turns HTP OFF. A systematic +1/3 EV push
 * with highlight protection disabled, on three presses out of four, is a
 * standing overexposure bias, and it showed: IMG_8222 came back with 10.5% of
 * its raw pixels within a stop of saturation and 16.9% of the JPEG blown.
 *
 * DARK_THRESHOLD 80 -> 36 (the measured median) cuts Dark-zone firing 77.5% ->
 * 40.0%, a 48% reduction -- deliberately HALF the correction, not all of it,
 * so the shadow-lift behaviour is damped rather than removed. It buys two
 * anti-overexposure effects from one value: the +1/3 EV nudge stops on 37.5%
 * of presses, and HTP (highlight protection) coverage rises 22.5% -> 60.0%.
 *
 * DARK_SEVERE / DARK_MODERATE are rescaled with it, keeping the spec's 0.31 /
 * 0.63 band ratios. This is REQUIRED, not cosmetic: DARK_MODERATE must stay
 * below DARK_THRESHOLD or `p90 < DARK_MODERATE` is always true inside the Dark
 * zone and resolve_alo()'s ALO_LOW branch becomes unreachable. All three bands
 * verified still populated on the field data (High 7 / Std 5 / Low 4).
 *
 * BRIGHT_THRESHOLD 240 -> 220 is free margin: the measured P99 distribution is
 * bimodal with an empty gap between 153 and 244, so every value from 160 to
 * 240 selects the exact same 12.5% of presses. 220 sits in that gap -- no
 * behaviour change on this data, a little more headroom on future scenes.
 *
 * ISO_NUDGE_STEP / SHUTTER_NUDGE_STEP stay at 8. They are NOT free tuning
 * knobs: 8 raw units is Canon's 1/3-EV quantum, and prop_set_rawiso rounds to
 * it anyway, so a "gentler" 4 would be silently rounded back or rejected.
 *
 * Re-tune against field logs as the sample grows -- n=40 is thin, and it is
 * one session's lighting. */
#define ISO_RAW_FLOOR       72   /* ISO 100 in Canon raw units (non-HTP floor) */
#define ISO_NUDGE_STEP       8   /* 1/3 EV in raw ISO units (Canon's quantum) */
#define SHUTTER_NUDGE_STEP   8   /* 1/3 EV in raw shutter units (+ = faster) */
#define DARK_THRESHOLD      36   /* P90 below this -> Dark zone   (spec: 80) */
#define BRIGHT_THRESHOLD   220   /* P99 at/above this -> Bright   (spec: 240) */
#define DARK_SEVERE         11   /* P90 below this -> ALO High    (spec: 25) */
#define DARK_MODERATE       23   /* P90 below this -> ALO Std     (spec: 50) */

typedef enum { ZONE_DARK, ZONE_NORMAL, ZONE_BRIGHT } scene_zone_t;

/* Highlight clipping wins over midtone darkness -- see the block comment. */
static scene_zone_t classify_zone(int p90, int p99)
{
    if (p99 >= BRIGHT_THRESHOLD) return ZONE_BRIGHT;
    if (p90 < DARK_THRESHOLD)    return ZONE_DARK;
    return ZONE_NORMAL;
}

static int resolve_alo(scene_zone_t zone, int p90)
{
    switch (zone)
    {
        case ZONE_DARK:
            if (p90 < DARK_SEVERE)   return ALO_HIGH;
            if (p90 < DARK_MODERATE) return ALO_STD;
            return ALO_LOW;                     /* P90 in [50, 79] */
        case ZONE_BRIGHT:
            return ALO_LOW;
        case ZONE_NORMAL:
        default:
            return ALO_STD;
    }
}

/* Dark -> OFF (HTP's ISO 200 floor costs shadow SNR); Normal/Bright -> ON. */
static int resolve_htp(scene_zone_t zone)
{
    return (zone != ZONE_DARK);
}

/* One step of exposure, at most, and never both knobs. Returns 1 if the
 * shutter fallback was taken instead of the ISO nudge.
 * cur_iso == 0 means Canon Auto ISO owns it -- 0 +/- 8 is meaningless, so the
 * nudge is skipped entirely (ALO/HTP still apply). */
static int compute_nudge(scene_zone_t zone, int cur_iso, int cur_shutter,
                         int mode, int * out_iso, int * out_shutter)
{
    *out_iso = cur_iso;
    *out_shutter = cur_shutter;

    if (cur_iso == 0) return 0;         /* Auto ISO: leave exposure to Canon */

    switch (zone)
    {
        case ZONE_DARK:
            *out_iso = cur_iso + ISO_NUDGE_STEP;            /* +1/3 EV */
            return 0;

        case ZONE_BRIGHT:
            if (cur_iso > ISO_RAW_FLOOR)
            {
                *out_iso = cur_iso - ISO_NUDGE_STEP;        /* -1/3 EV */
            }
            else if (mode == SHOOTMODE_P)
            {
                *out_shutter = cur_shutter + SHUTTER_NUDGE_STEP;  /* 1/3 EV faster */
                return 1;
            }
            /* Av/Tv/M at the floor: the user owns shutter -- no change possible */
            return 0;

        case ZONE_NORMAL:
        default:
            return 0;                   /* no exposure change */
    }
}

/* Returns 1 when a scene analysis actually ran (so the caller can burn its
 * once-per-half-press guard), 0 when no light level was available yet and the
 * next poll should retry -- same retry shape as the ai_lut_log() path below. */
static int ai_apply_alo_htp(void)
{
    if (!auto_iso_optimizer) return 0;              /* feature gate */

    /* One metering pass yields both percentiles (ai_raw_p99 is a side effect).
     * ai_light_level() falls back to the LiveView display histogram when the
     * raw histogram is unavailable; ai_raw_p99 is then -1, which can never
     * reach BRIGHT_THRESHOLD, so an unknown P99 simply cannot force Bright. */
    int p90 = ai_light_level();
    if (p90 < 0) return 0;
    int p99 = ai_raw_p99;

    scene_zone_t zone = classify_zone(p90, p99);

    int want_htp = resolve_htp(zone);
    int want_alo = resolve_alo(zone, p90);

    int want_iso, want_shutter;
    compute_nudge(zone, lens_info.raw_iso, lens_info.raw_shutter,
                  shooting_mode, &want_iso, &want_shutter);

    /* HTP before ALO (Canon suppresses ALO while HTP is on), then exposure.
     * Each setter is skipped when the value already matches. */
    if (want_htp != get_htp()) set_htp(want_htp);
    if (want_alo != get_alo()) set_alo(want_alo);
    if (want_iso != lens_info.raw_iso) lens_set_rawiso(want_iso);
    if (want_shutter != lens_info.raw_shutter) lens_set_rawshutter(want_shutter);

    return 1;
}

static int debug_info = 0;
static int show_metered_areas = 0;

/* one AI log record per half-press; shared between the LiveView polling path
 * and the OVF image-review (QR) path so they never double-log a shot */
static int ai_logged_press = 0;

/* same once-per-half-press guard for the ALO/HTP Scene Optimizer. Separate
 * from ai_logged_press on purpose: data logging is an independent feature and
 * either one consuming the other's flag would silently disable it. */
static int ai_scene_opt_press = 0;

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

    if (ai_shutter_reciprocal)
    {
        if (lens_info.focal_len <= 0 && ai_ml_active >= 1 && ai_ml_active <= AI_ML_SLOTS
            && ai_ml_focal[ai_ml_active - 1] > 0)
        {
            MENU_SET_WARNING(MENU_WARN_INFO,
                "AI Shutter Control is ON -- following Manual Lens Profile slot %d (%d mm), no CPU lens detected.",
                ai_ml_active, ai_ml_focal[ai_ml_active - 1]);
        }
        else
        {
            MENU_SET_WARNING(MENU_WARN_INFO,
                "AI Shutter Control is ON -- this value follows the lens zoom (1/focal_length), not your manual setting.");
        }
    }

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
 * same as AI WB.
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
    /* [2]=50 (5.0th percentile, shadow) fed the old float ALO/HTP chooser's
     * scene_dr; the Scene Optimizer is percentile/integer-based and runs off
     * its own half-press pass now (see ai_apply_alo_htp), so only lvls[0] is
     * still consumed here. Kept in the same request -- it is free. */
    int pcts[3] = {999, 950, 50};
    int lvls[3] = {-1, -1, -1};
    int meter_ok = (span >= 8) &&
        (raw_hist_get_percentile_levels(pcts, lvls, 3,
            GRAY_PROJECTION_MAX_RGB | GRAY_PROJECTION_DARK_ONLY, 4) == 1) &&
        (lvls[0] >= 0);

    /* TARGET = headroom UNDER clip (not at clip): aim the brightest highlights
     * 0.5..0.70 EV below saturation, more headroom the closer they are to
     * clipping. This IS a fixed point for a STATIC highlight -- it converges
     * and settles rather than oscillating -- but it is NOT bounded against
     * the rest of the scene: it only ever looks at the brightest 0.1% of
     * pixels. FIELD BUG 2026-07-27: a persistent small bright element (a
     * window, a light fixture) that can't be brought under headroom without
     * crushing the midtones will make this "converge" at an increasingly
     * dark exposure shot after shot, with nothing to stop it short of the
     * hardware EC limit -- reported as exposure marching past -2 EV and
     * continuing. ai_ec_floor_halfstop below is a blunt cap for this; the original
     * M-mode ETTR (auto_ettr_work) avoids the whole problem with a real
     * midtone/shadow noise-floor limit (auto_ettr_midtone_snr_limit /
     * auto_ettr_shadow_snr_limit) that this experimental path doesn't have. */
    int cur_ec = lens_info.ae;                /* 1/8 EV, signed */
    int target = cur_ec;
    if (meter_ok)
    {
        int hl = lvls[0] - black; if (hl < 1) hl = 1;
        int clipp = hl * 1000 / span;         /* highlight vs saturation, permille (0..1000) */
        int headroom = (clipp >= 980) ? 70 : (clipp >= 900) ? 60 : 50;  /* 1/100 EV, by severity */
        int hl_e2 = (int)(raw_to_ev(lvls[0]) * 100);  /* highlight EV rel. to clip, x100 (<=0) */
        int shift_e2 = (-headroom) - hl_e2;   /* 1/100 EV to move highlight to -headroom */
        int delta8 = COERCE((shift_e2 * 8 / 100) / 2, -8, 8);  /* ->1/8 EV, half-damped, <=1EV/shot */
        /* darken floor: never push more negative than -ai_ec_floor_halfstop/2
         * EV relative to Canon's own metering (default -1.5 EV). Brightening
         * (positive delta8) is never capped here -- only the runaway-darkening
         * direction needed the safety net. 1 half-EV step = 4 eighths. */
        int floor_8 = -ai_ec_floor_halfstop * 4;
        target = COERCE(cur_ec + delta8, floor_8, 16);     /* darken floor .. +2 EV */
        target = COERCE(target, -40, 16);                  /* hardware EC range safety */
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
 * take over at highest priority: enable ETTR and switch to manual ISO at the
 * floor so ETTR becomes the auto-exposure engine.
 *
 * ISO-floor takeover is still edge-triggered on purpose: we act once when Auto
 * ISO is engaged, then respect the user afterwards. So if you later turn HTP
 * off (Canon Q menu or ML), the ETTR floor naturally drops from ISO 200 back
 * to ISO 100 (via MIN_ISO), exactly as requested. The 6D has no native ISO 50
 * for RAW, so 100 is the real floor.
 *
 * The Auto-ETTR/EC arming below is NOT edge-triggered -- it runs every poll in
 * every AI-covered mode, per the 2026-07-27 per-mode redesign (see the "AI
 * Modes" comment above). The ALO/HTP Scene Optimizer fires once per
 * half-shutter press instead -- see ai_apply_alo_htp. */
static void auto_iso_optimizer_step()
{
    if (!auto_iso_optimizer) return;

    int mode_ok = ai_mode_covered();

    /* Keep Auto ETTR (and its half-shutter trigger) armed in every AI-covered
     * mode, not just M -- this is what lets auto_ettr_ec_step (the P/Av/Tv
     * exposure-compensation corrector) actually run without the user
     * separately toggling the base "Auto ETTR" feature: in Av, Canon's own AE
     * resolves the EC bias by moving the shutter (aperture is the user's); in
     * Tv, by moving the aperture (shutter is the user's); in P, by moving the
     * whole Program line (both). */
    if (mode_ok)
    {
        auto_ettr = 1;
        auto_ettr_trigger = 3;   /* Half-Shutter */
    }

    /* ALO/HTP Scene Optimizer: one analysis + apply per half-shutter press, in
     * EVERY shooting mode (P/Av/Tv/M -- the per-mode differences live inside
     * compute_nudge, not here). Same guard shape as the ai_data_logging path
     * in the polling CBR: clear on release, set once the analysis has actually
     * run. ai_apply_alo_htp returns 0 while no light level is available yet,
     * so a press that starts before the histogram is built retries next poll
     * instead of silently burning its one shot. */
    if (!get_halfshutter_pressed())
        ai_scene_opt_press = 0;
    else if (!ai_scene_opt_press && ai_apply_alo_htp())
        ai_scene_opt_press = 1;

    static int was_auto_iso = 0;
    int is_auto_iso = (lens_info.raw_iso == 0);

    if (is_auto_iso && mode_ok && !was_auto_iso)
    {
        /* AI-LUT (single source of adjustment for ISO/WB): get the learned
         * starting ISO. Wrap with raw_lv_request so the lookup uses the RAW
         * light level even when AI Data Logging (which otherwise holds raw
         * LV) is off. ALO/HTP is no longer this function's concern -- see
         * ai_apply_alo_htp above. */
        int ai_raw_wrap = lv && ((void*)&raw_lv_request != (void*)&ret_0);
        if (ai_raw_wrap) raw_lv_request();
        int ai_iso = ai_lut_apply();
        if (ai_raw_wrap) raw_lv_release();

        if (shooting_mode == SHOOTMODE_M)
        {
            /* Only in M can ETTR actually drive exposure -- it needs control
             * of ISO+shutter, which Canon owns in P/Av/Tv (ETTR bails out
             * there, see auto_ettr_step / auto_ettr_check_pre_lv). So only in M
             * do we hand over: half-shutter meters, ISO at the learned
             * (or MIN_ISO) floor. MIN_ISO = get_htp() ? 80 (ISO200) : 72 (ISO100). */
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

/* AI Flash Mode: force Canon's own metering mode to Partial while a flash
 * workflow is selected (ai_flash_mode above), and restore whatever metering
 * mode was active the moment it goes back to Off. Unconditional -- runs
 * regardless of "AI Modes" coverage or auto_iso_optimizer, since a flash
 * workflow is orthogonal to which shooting modes the rest of the AI covers
 * and to whether the AI is even on. */
static int ai_flash_saved_meter = -1;   /* -1 = nothing saved (flash mode is Off) */

static void ai_flash_metering_step(void)
{
    if (ai_flash_mode)
    {
        if (ai_flash_saved_meter < 0)
            ai_flash_saved_meter = metering_mode;   /* capture once, on the Off->on edge */
        if (metering_mode != PARTIAL_METER)
        {
            int m = PARTIAL_METER;
            prop_request_change(PROP_METERING_MODE, &m, 4);
        }
    }
    else if (ai_flash_saved_meter >= 0)
    {
        if (metering_mode != ai_flash_saved_meter)
        {
            int m = ai_flash_saved_meter;
            prop_request_change(PROP_METERING_MODE, &m, 4);
        }
        ai_flash_saved_meter = -1;
    }
}

static unsigned int auto_ettr_polling_cbr()
{
    auto_iso_optimizer_step();
    ai_flash_metering_step();

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

    /* AI Shutter Control: reciprocal rule (1/focal_length s) from the lens's
     * LIVE reported focal length, tracking zoom continuously -- not gated to
     * half-press, since the whole point is the shutter floor stays correct
     * the instant you zoom, before you ever press anything. Applies in any
     * AI-covered mode (M included: it only tightens auto_ettr_work's
     * shutter_lim, the actual exposure push is unaffected). No electronic
     * focal-length report (manual/adapted lens, focal_len==0) -> fall back to
     * the active Manual Lens Profile's registered focal length, if the user
     * set one; with neither, leave auto_ettr_max_shutter exactly as the user
     * set it in the menu. */
    if (ai_on && ai_shutter_reciprocal)
    {
        int ai_ml_focal_mm = (int) lens_info.focal_len;
        if (ai_ml_focal_mm <= 0 && ai_ml_active >= 1 && ai_ml_active <= AI_ML_SLOTS)
            ai_ml_focal_mm = ai_ml_focal[ai_ml_active - 1];
        if (ai_ml_focal_mm > 0)
            auto_ettr_max_shutter = shutterf_to_raw(1.0f / (float) ai_ml_focal_mm);
    }

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

/* --- Manual Lens Profiles: on-camera editor for manual/adapted primes ------
 * Up to AI_ML_SLOTS registered lenses (focal length + max aperture). "Edit
 * slot" picks which one "Focal length"/"Max aperture" show and edit.
 * "Active manual lens" is separate from the edit slot -- it's the one AI
 * Shutter Control actually reads, so other slots can be reviewed/edited
 * without disturbing what's currently feeding the shutter floor. */

static MENU_UPDATE_FUNC(ai_ml_menu_update)
{
    if (ai_ml_active >= 1 && ai_ml_active <= AI_ML_SLOTS && ai_ml_focal[ai_ml_active - 1] > 0)
    {
        int idx = ai_ml_active - 1;
        int ax10 = ai_ml_aperture_x10[COERCE(ai_ml_ap_idx[idx], 0, AI_ML_APERTURES - 1)];
        MENU_SET_VALUE("Slot %d: %d mm f/%d.%d", ai_ml_active, ai_ml_focal[idx], ax10/10, ax10%10);
    }
    else
    {
        MENU_SET_VALUE("None active");
    }
}

static MENU_UPDATE_FUNC(ai_ml_slot_update)
{
    int idx = COERCE(ai_ml_slot, 1, AI_ML_SLOTS) - 1;
    if (ai_ml_focal[idx] > 0)
    {
        int ax10 = ai_ml_aperture_x10[COERCE(ai_ml_ap_idx[idx], 0, AI_ML_APERTURES - 1)];
        MENU_SET_VALUE("%d (%d mm f/%d.%d)", ai_ml_slot, ai_ml_focal[idx], ax10/10, ax10%10);
    }
    else
    {
        MENU_SET_VALUE("%d (empty)", ai_ml_slot);
    }
    if (ai_ml_slot == ai_ml_active)
        MENU_SET_RINFO("ACTIVE");
}

static MENU_UPDATE_FUNC(ai_ml_focal_update)
{
    int idx = COERCE(ai_ml_slot, 1, AI_ML_SLOTS) - 1;
    if (ai_ml_focal[idx] > 0)
        MENU_SET_VALUE("%d mm", ai_ml_focal[idx]);
    else
        MENU_SET_VALUE("(unset)");
}

/* 1 mm/click -- primes are usually specified to the mm (24, 35, 50, 85...) */
static MENU_SELECT_FUNC(ai_ml_focal_toggle)
{
    int idx = COERCE(ai_ml_slot, 1, AI_ML_SLOTS) - 1;
    menu_numeric_toggle(&ai_ml_focal[idx], delta, 0, 800);
}

static MENU_UPDATE_FUNC(ai_ml_aperture_update)
{
    int idx = COERCE(ai_ml_slot, 1, AI_ML_SLOTS) - 1;
    int ax10 = ai_ml_aperture_x10[COERCE(ai_ml_ap_idx[idx], 0, AI_ML_APERTURES - 1)];
    MENU_SET_VALUE("f/%d.%d", ax10/10, ax10%10);
}

static MENU_SELECT_FUNC(ai_ml_aperture_toggle)
{
    int idx = COERCE(ai_ml_slot, 1, AI_ML_SLOTS) - 1;
    menu_numeric_toggle(&ai_ml_ap_idx[idx], delta, 0, AI_ML_APERTURES - 1);
}

static MENU_SELECT_FUNC(ai_ml_clear_select)
{
    int idx = COERCE(ai_ml_slot, 1, AI_ML_SLOTS) - 1;
    ai_ml_focal[idx] = 0;
    ai_ml_ap_idx[idx] = 0;
    NotifyBox(2000, "Slot %d cleared", ai_ml_slot);
}

static MENU_UPDATE_FUNC(ai_ml_active_update)
{
    if (ai_ml_active < 1 || ai_ml_active > AI_ML_SLOTS)
    {
        MENU_SET_VALUE("None");
        return;
    }
    int idx = ai_ml_active - 1;
    if (ai_ml_focal[idx] > 0)
    {
        int ax10 = ai_ml_aperture_x10[COERCE(ai_ml_ap_idx[idx], 0, AI_ML_APERTURES - 1)];
        MENU_SET_VALUE("Slot %d (%d mm f/%d.%d)", ai_ml_active, ai_ml_focal[idx], ax10/10, ax10%10);
    }
    else
    {
        MENU_SET_VALUE("Slot %d", ai_ml_active);
        MENU_SET_WARNING(MENU_WARN_INFO,
            "This slot has no focal length registered yet -- edit it below first.");
    }
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
                .name = "AI Flash Mode",
                .priv = &ai_flash_mode,
                .max = 2,
                .choices = CHOICES("Off", "E-TTL", "Manual"),
                .help  = "Off never touches metering mode. E-TTL/Manual force Partial metering.",
                .help2 = "Meter the subject/background; let the flash (E-TTL or your own manual power) fill it.",
            },
            {
                .name = "AI ETTR Darken Limit",
                .priv = &ai_ec_floor_halfstop,
                .min = 1,
                .max = 10,
                .choices = CHOICES("-0.5 EV", "-1.0 EV", "-1.5 EV", "-2.0 EV", "-2.5 EV",
                                    "-3.0 EV", "-3.5 EV", "-4.0 EV", "-4.5 EV", "-5.0 EV"),
                .help  = "P/Av/Tv only: how far ETTR may darken vs. Canon's own metering.",
                .help2 = "Safety cap -- the highlight-only meter has no midtone floor yet.",
            },
            {
                .name = "AI Shutter Control",
                .priv = &ai_shutter_reciprocal,
                .max = 1,
                .help  = "Sets 'Slowest shutter' live from zoom: never slower than 1/focal_length.",
                .help2 = "Tracks zoom in real time. No effect on primes/manual lenses w/o CPU contacts.",
            },
            {
                .name = "Manual Lens Profiles",
                .update = ai_ml_menu_update,
                .select = menu_open_submenu,
                .icon_type = IT_SUBMENU,
                .help  = "Register up to 10 manual/adapted prime lenses (focal + max aperture).",
                .help2 = "Feeds AI Shutter Control's reciprocal rule when the lens reports no focal length.",
                .children = (struct menu_entry[]) {
                    {
                        .name = "Edit slot",
                        .priv = &ai_ml_slot,
                        .update = ai_ml_slot_update,
                        .min = 1, .max = AI_ML_SLOTS,
                        .help = "Which profile slot the entries below show/edit.",
                    },
                    {
                        .name = "Focal length",
                        .update = ai_ml_focal_update,
                        .select = ai_ml_focal_toggle,
                        .min = 0, .max = 800,
                        .help  = "This slot's prime lens focal length, in mm.",
                        .help2 = "0 = empty slot (ignored by AI Shutter Control).",
                    },
                    {
                        .name = "Max aperture",
                        .update = ai_ml_aperture_update,
                        .select = ai_ml_aperture_toggle,
                        .min = 0, .max = AI_ML_APERTURES - 1,
                        .help  = "This slot's maximum (widest) aperture.",
                        .help2 = "Stored for reference; not yet used by any AI calculation.",
                    },
                    {
                        .name = "Clear this slot",
                        .select = ai_ml_clear_select,
                        .help = "Reset the slot selected above to empty.",
                    },
                    {
                        .name = "Active manual lens",
                        .priv = &ai_ml_active,
                        .update = ai_ml_active_update,
                        .min = 0, .max = AI_ML_SLOTS,
                        .help  = "Slot AI Shutter Control uses when the mounted lens has no CPU contacts.",
                        .help2 = "None = leave 'Slowest shutter' as your manual setting on such lenses.",
                    },
                    MENU_EOL,
                },
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
