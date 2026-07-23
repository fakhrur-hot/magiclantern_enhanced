/**
 * Experimental SD UHS overclocking.
 */

#include <module.h>
#include <dryos.h>
#include <patch.h>
#include <console.h>
#include <config.h>

/* camera-specific parameters */
static uint32_t GPIO = 0;
static uint32_t GPIO_cmp = 0;
static uint32_t CID_hook = 0;
static uint32_t sd_write_clock = 0;
static uint32_t sd_read_clock = 0;
static uint32_t sd_enable_18V = 0;
static uint32_t sd_setup_mode = 0;
static uint32_t sd_setup_mode_in = 0;
static uint32_t sd_setup_mode_reg = 0xFFFFFFFF;
static uint32_t sd_set_function = 0;

static uint32_t uhs_regs[] = {0xC0400600, 0xC0400604, /*C0400608, C040060C*/ 0xC0400610, 0xC0400614, 0xC0400618, 0xC0400624, 0xC0400628, 0xC040061C, 0xC0400620}; /* register addresses */

static uint32_t sdr50_700D[] = {0x3, 0x3, 0x4, 0x1D000301, 0x0, 0x201, 0x201, 0x100, 0x4};                                                                        /* SDR50 values from 700D (96MHz) */
static uint32_t sdr_160MHz1[] = {0x3, 0x2, 0x1, 0x1D000001, 0x0, 0x201, 0x201, 0x100, 0x1};                                                                       /* Found by trial and error, little bit faster than second (original) 160 MHz */
static uint32_t sdr_160MHz2[] = {0x2, 0x3, 0x1, 0x1D000001, 0x0, 0x100, 0x100, 0x100, 0x1};                                                                       /* overclocked values: 160MHz = 96*(4+1)/(2?+1) (found by brute-forcing) */
static uint32_t sdr_192MHz[] = {0x8, 0x3, 0x4, 0x1D000301, 0x0, 0x201, 0x201, 0x100, 0x4};
static uint32_t sdr_240MHz[] = {0x8, 0x3, 0x3, 0x1D000301, 0x0, 0x201, 0x201, 0x100, 0x3};
static uint32_t sdr_240MHz2[] = {0x3, 0x3, 0x1, 0x1D000001, 0x0, 0x100, 0x100, 0x100, 0x1}; /* Works better on 100D / EOS M, also SDR104 is stable with this preset (for Write operations) */

static uint32_t uhs_vals[COUNT(uhs_regs)]; /* current values */
static int sd_setup_mode_enable = 0;
static int turned_on = 0;
static CONFIG_INT("sd.sd_overclock", sd_overclock, 0);
static CONFIG_INT("sd.sd_access_mode", access_mode, 1);

/* --------------------------------------------------------------------------
 * Auto Speed Test: cycles overclock presets from fastest to slowest across
 * reboots, using the same Canon safe-mode fallback register sd_uhs_update()
 * already checks (0xC0400614) to auto-fail any preset the card/camera itself
 * rejects. This can only ever automate "did Canon's safe-mode kick in" -- it
 * CANNOT automate "is this stable for a real sustained recording": extensive
 * community testing (e.g. Samsung PRO Plus) found cards that pass a safe-mode
 * check yet still fail during real recording. So the wizard always stops
 * after a safe-mode pass and requires the user to explicitly confirm real
 * recording tests were done before treating a preset as final. It never
 * reboots the camera itself -- the user restarts manually, same as the
 * existing "SD Frequency" menu already requires. */
#define AUTOTEST_IDLE           0
#define AUTOTEST_TESTING        1  /* preset applied, awaiting reboot + safe-mode check */
#define AUTOTEST_AWAIT_CONFIRM  2  /* preset passed safe-mode, awaiting user's real-recording verdict */
#define AUTOTEST_DONE           3
#define AUTOTEST_FAILED_ALL     4

/* Which preset autotest_state currently refers to (1=240MHz, 2=192MHz,
 * 3=160MHz -- same encoding as sd_overclock itself). Kept separate from
 * autotest_state so AWAIT_CONFIRM doesn't lose track of which preset it's
 * judging. */
static CONFIG_INT("sd.autotest_state", autotest_state, AUTOTEST_IDLE);
static CONFIG_INT("sd.autotest_preset", autotest_preset, 0);

#define AUTOTEST_LOG_PATH "ML/logs/sd_autotest.txt"

static void autotest_log(const char * msg)
{
    FIO_CreateDirectory("ML/logs");
    FILE * f = FIO_OpenFile(AUTOTEST_LOG_PATH, O_RDWR | O_CREAT | O_SYNC);
    if (!f) return;
    FIO_SeekSkipFile(f, 0, SEEK_END);
    FIO_WriteFile(f, msg, strlen(msg));
    FIO_WriteFile(f, "\n", 1);
    FIO_CloseFile(f);
}

static const char * autotest_preset_name(int preset)
{
    switch (preset)
    {
        case 3: return "240MHz";
        case 2: return "192MHz";
        case 1: return "160MHz";
        default: return "OFF";
    }
}

static MENU_SELECT_FUNC(autotest_start)
{
    autotest_preset = 3; /* start at the fastest preset, step down on failure */
    autotest_state = AUTOTEST_TESTING;
    sd_overclock = autotest_preset;
    autotest_log("AUTOTEST: starting wizard, testing 240MHz");
    NotifyBox(6000, "Auto Speed Test: set to 240MHz.\nRESTART camera to continue.");
}

/* Steps down to the next slower preset, or ends the wizard (overclock OFF)
 * if 160MHz itself is not safe/stable. Shared by the automatic safe-mode
 * failure path (sd_uhs_update) and the manual "Mark Unstable" path. */
static void autotest_advance(const char * reason)
{
    char msg[80];

    if (autotest_preset <= 1)
    {
        /* 160MHz itself failed (or was marked unstable) -- nothing left to try */
        autotest_state = AUTOTEST_FAILED_ALL;
        autotest_preset = 0;
        sd_overclock = 0;
        snprintf(msg, sizeof(msg), "AUTOTEST: %s, 160MHz was the last option, overclock disabled", reason);
        autotest_log(msg);
        NotifyBox(6000, "Auto Speed Test: no stable preset found.\nOverclock disabled. RESTART to apply.");
        return;
    }

    autotest_preset--;
    autotest_state = AUTOTEST_TESTING;
    sd_overclock = autotest_preset;
    snprintf(msg, sizeof(msg), "AUTOTEST: %s, now testing %s", reason, autotest_preset_name(autotest_preset));
    autotest_log(msg);
    NotifyBox(6000, "Auto Speed Test: stepping down.\nRESTART camera to continue.");
}

static MENU_SELECT_FUNC(autotest_confirm_pass)
{
    if (autotest_state != AUTOTEST_AWAIT_CONFIRM) return;
    char msg[80];
    autotest_state = AUTOTEST_DONE;
    snprintf(msg, sizeof(msg), "AUTOTEST: user confirmed %s stable after real recording tests, keeping it", autotest_preset_name(autotest_preset));
    autotest_log(msg);
    NotifyBox(4000, "Auto Speed Test: preset kept as final.");
}

static MENU_SELECT_FUNC(autotest_confirm_fail)
{
    if (autotest_state != AUTOTEST_AWAIT_CONFIRM) return;
    autotest_advance("user marked unstable after real recording test");
}

static MENU_SELECT_FUNC(autotest_reset)
{
    autotest_state = AUTOTEST_IDLE;
    autotest_preset = 0;
    autotest_log("AUTOTEST: wizard reset by user");
}

static MENU_UPDATE_FUNC(autotest_status_display)
{
    switch (autotest_state)
    {
        case AUTOTEST_IDLE:
            MENU_SET_VALUE("Idle");
            break;
        case AUTOTEST_TESTING:
            MENU_SET_VALUE("Testing %s...", autotest_preset_name(autotest_preset));
            MENU_SET_WARNING(MENU_WARN_INFO, "Restart camera, then reopen this menu to check safe-mode result.");
            break;
        case AUTOTEST_AWAIT_CONFIRM:
            MENU_SET_VALUE("%s passed safe-mode", autotest_preset_name(autotest_preset));
            MENU_SET_WARNING(MENU_WARN_INFO, "Record several REAL test clips now, then Confirm Stable or Mark Unstable below.");
            break;
        case AUTOTEST_DONE:
            MENU_SET_VALUE("Done: %s", autotest_preset_name(autotest_preset));
            break;
        case AUTOTEST_FAILED_ALL:
            MENU_SET_VALUE("No safe preset found");
            break;
    }
}

/* CID info hook, should work on all DIGIC 5 models */
uint32_t MID;
uint32_t OID;
uint32_t PNM_1;
uint32_t PNM_2;
uint32_t PRV;
uint32_t PSN_1;
uint32_t PSN_2;
uint32_t MDT;
uint32_t CRC;
static void GetCID(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    MID = regs[5] >> 0x18;
    OID = regs[5];
    PNM_1 = regs[5];
    PNM_2 = regs[4];
    PRV = regs[6];
    PSN_1 = regs[6];
    PSN_2 = regs[7];
    MDT = regs[7];
    CRC = regs[7];
}

/* start of the function */
static void sd_setup_mode_log(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    qprintf("sd_setup_mode(dev=%x)\n", regs[0]);

    /* this function is also used for other interfaces, such as serial flash */
    /* only enable overriding when called with dev=1 */
    sd_setup_mode_enable = (regs[0] == 1);
}

/* called right before the case switch in sd_setup_mode (not at the start of the function!) */
static void sd_setup_mode_in_log(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    qprintf("sd_setup_mode switch(mode=%x) en=%d\n", regs[sd_setup_mode_reg], sd_setup_mode_enable);

    if (sd_setup_mode_enable && regs[sd_setup_mode_reg] == 4) /* SDR50? */
    {
        /* set our register overrides */
        for (int i = 0; i < COUNT(uhs_regs); i++)
        {
            MEM(uhs_regs[i]) = uhs_vals[i];
        }

        /* set some invalid mode to bypass the case switch
         * and keep our register values only */
        regs[sd_setup_mode_reg] = 0x13;
    }
}

static void sd_set_function_log(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    qprintf("sd_set_function(0x%x)\n", regs[0]);

    /* UHS-I SDR50? */
    if (regs[0] == 0xff0002)
    {
        /* force UHS-I SDR104 */
        regs[0] = 0xff0003;
    }
}

static int patch_instruction32(uintptr_t addr,
                               uint32_t old_val,
                               uint32_t new_val,
                               const char *desc)
{
    struct patch p = {
        .addr = (uint8_t *)addr,
        .old_value = old_val,
        .new_value = new_val,
        .size = 4,
        .description = desc,
        .is_instruction = 1,
    };
    return apply_patches(&p, 1);
}

// The following registers are related to SD configuration, and they affect OC stability especially at high OC (such as 240 MHz)
// More info: check "New GPIO registers" in https://www.magiclantern.fm/forum/index.php?topic=26634.msg240128#msg240128
// On 100D, EOS M and EOS M2, changing them is required to get stable 240 MHz OC, however 5D3 seem to doesn't care about them, why?
// Following values founded by trial and error, they appear to be the best on 100D and probably on EOS M/M2 too
static void GPIO_registers(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    MEM(0xC022C634) = 0x599;
    MEM(0xC022C638) = 0x555;
    MEM(0xC022C63C) = 0x555;
    MEM(0xC022C640) = 0x555;
    MEM(0xC022C644) = 0x555;
    MEM(0xC022C648) = 0x555;
}

static int overclock_task_in_progress = 0;
static void WriteClock5D3(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    while (overclock_task_in_progress) // During overclocking, call msleep in loop until we finsih. This appear to pause write/read without side effects
    {
        msleep(100);
    }

    if (sd_overclock == 2)
        memcpy(uhs_vals, sdr_192MHz, sizeof(uhs_vals));
    if (sd_overclock == 3)
        memcpy(uhs_vals, sdr_240MHz, sizeof(uhs_vals));
}

static void ReadClock5D3(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    while (overclock_task_in_progress) // During overclocking, call msleep in loop until we finsih. This appear to pause write/read without side effects
    {
        msleep(100);
    }

    memcpy(uhs_vals, sdr_160MHz2, sizeof(uhs_vals));
}

static void PauseWriteClock(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    while (overclock_task_in_progress)
    {
        msleep(100);
    }
}

static void PauseReadClock(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    while (overclock_task_in_progress)
    {
        msleep(100);
    }
}

static void WriteClock(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    memcpy(uhs_vals, sdr_240MHz2, sizeof(uhs_vals));
}

static void ReadClock(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    memcpy(uhs_vals, sdr_192MHz, sizeof(uhs_vals));
}

struct cf_device
{
    /* type b always reads from raw sectors */
    int (*read_block)(
        struct cf_device *dev,
        void *buf,
        uintptr_t block,
        size_t num_blocks);

    int (*write_block)(
        struct cf_device *dev,
        const void *buf,
        uintptr_t block,
        size_t num_blocks);
};

static void (*SD_ReConfiguration)() = 0;

static void sd_reset(struct cf_device *const dev)
{
    /* back to some safe values */
    memcpy(uhs_vals, sdr50_700D, sizeof(uhs_vals));

    /* clear error flag to allow activity after something went wrong */
    MEM((uintptr_t)dev + 80) = 0;

    /* re-initialize card */
    SD_ReConfiguration();
}

static int GPIO_patch_on = 0;
static void sd_overclock_task()
{
    if (is_camera("5D3", "1.1.3") || is_camera("5D3", "1.2.3"))
    {
        overclock_task_in_progress = 1; // Enable SD Overclocking task flag

        NotifyBox(10000, "SD Overclocking..."); // The camera would be a little unresponsive due to pausing SD reads/writes, add a warning to inform the user

        /* Patch sdReadBlk and sdWriteBlk Now! for pausing SD reads/writes operations  */
        /* Also, will use hybrid clock speed when 192 MHz or 240 MHz are selected      */
        /* Hybrid clock speed or "The Magic Trick" was explained here:
           https://www.magiclantern.fm/forum/index.php?topic=26634.msg240128#msg240128 */
        patch_hook_function(sd_read_clock, MEM(sd_read_clock), ReadClock5D3, "R_Clock");
        patch_hook_function(sd_write_clock, MEM(sd_write_clock), WriteClock5D3, "W_Clock");

        /* All GPIO regsiters are 0x755 on 5D3, overriding them didn't have an effect?
           Override them just in case */
        MEM(0xC022C634) = 0x555;
        MEM(0xC022C638) = 0x555;
        MEM(0xC022C63C) = 0x555;
        MEM(0xC022C640) = 0x555;
        MEM(0xC022C644) = 0x555;
        MEM(0xC022C648) = 0x555;

        /* These are used on 100D/700D, and it seems to affect SD clock stability, override them on 5D3 just in case
            On 5D3: 0xC040046C = 0x1, both 0xC0400450/0xC0400454 = 0x0
            On 100D/700D: when swtiching to 48 MHz, 0xC040046C becomes 0x1 */
        MEM(0xC0400450) = 0x1001;
        MEM(0xC0400454) = 0x1001;
        MEM(0xC040046C) = 0x2016E06;

        /* install the hack */
        memcpy(uhs_vals, sdr50_700D, sizeof(uhs_vals));

        if (sd_enable_18V)
        {
            patch_instruction32(sd_enable_18V, 0xe3a00000, 0xe3a00001, "SD 1.8V");
        }

        patch_hook_function(sd_setup_mode, MEM(sd_setup_mode), sd_setup_mode_log, "SD UHS");
        patch_hook_function(sd_setup_mode_in, MEM(sd_setup_mode_in), sd_setup_mode_in_log, "SD UHS");

        /* power-cycle and reconfigure the SD card */
        SD_ReConfiguration();

        /* enable SDR104 */
        if (access_mode)
        {
            patch_hook_function(sd_set_function, MEM(sd_set_function), sd_set_function_log, "SDR104");
        }

        /* Get CID info */
        patch_hook_function(CID_hook, MEM(CID_hook), GetCID, "CID");

        SD_ReConfiguration();

        if (sd_overclock == 1)
            memcpy(uhs_vals, sdr_160MHz2, sizeof(uhs_vals));
        //        if (sd_overclock == 2) memcpy(uhs_vals, sdr_192MHz, sizeof(uhs_vals)); // Not needed here, it's being applied by WriteClock5D3
        //        if (sd_overclock == 3) memcpy(uhs_vals, sdr_240MHz, sizeof(uhs_vals)); // Not needed here, it's being applied by WriteClock5D3

        overclock_task_in_progress = 0; // Disable SD Overclocking task flag

        /* Not needed anymore for 160 MHz preset */
        if (sd_overclock == 1)
        {
            msleep(100); // the card become inaccessible without a delay
            unpatch_memory(sd_read_clock);
            unpatch_memory(sd_write_clock);
        }

        /* CID hook isn't needed anymore */
        unpatch_memory(CID_hook);

        NotifyBoxHide();
    }

    else if (is_camera("100D", "1.0.1"))
    {
        /* install the hack */
        memcpy(uhs_vals, sdr50_700D, sizeof(uhs_vals));
        patch_hook_function(sd_setup_mode, MEM(sd_setup_mode), sd_setup_mode_log, "SD UHS");
        patch_hook_function(sd_setup_mode_in, MEM(sd_setup_mode_in), sd_setup_mode_in_log, "SD UHS");

        /* enable SDR104 */
        if (access_mode)
        {
            patch_hook_function(sd_set_function, MEM(sd_set_function), sd_set_function_log, "SDR104");
        }

        /* Get CID info */
        patch_hook_function(CID_hook, MEM(CID_hook), GetCID, "CID");

        SD_ReConfiguration();

        /* CID hook isn't needed anymore */
        unpatch_memory(CID_hook);

        // Patch cmp instruction to avoid loading default GPIO registers values
        patch_instruction32(GPIO_cmp, 0xe3540001, 0xe3540008, "GPIO_cmp");

        patch_hook_function(GPIO, MEM(GPIO), GPIO_registers, "GPIO");    // Set our GPIO values
        SD_ReConfiguration();
        GPIO_patch_on = 1;

        if (sd_overclock == 3) // Hybrid clock speed for 240 MHz
        {
            patch_hook_function(sd_read_clock, MEM(sd_read_clock), ReadClock, "R_Clock");
            patch_hook_function(sd_write_clock, MEM(sd_write_clock), WriteClock, "W_Clock");
        }

        /* 160MHz1 doesn't work on 70D, 160MHz1 acts as 192 MHz on 6D / 5D3 which is weird */
        if (sd_overclock == 1)
            memcpy(uhs_vals, sdr_160MHz2, sizeof(uhs_vals));
        if (sd_overclock == 2)
            memcpy(uhs_vals, sdr_192MHz, sizeof(uhs_vals));
        //        if (sd_overclock == 3) memcpy(uhs_vals, sdr_240MHz, sizeof(uhs_vals)); // Not needed here, it's being applied by WriteClock

        /* Not needed anymore */
        if (GPIO_patch_on)
        {
            msleep(100);
            unpatch_memory(GPIO_cmp);
            unpatch_memory(GPIO);
            GPIO_patch_on = 0;
        }
    }

    else if (is_camera("EOSM", "2.0.2") || is_camera("EOSM2", "1.0.3"))
    {
        /* install the hack */
        memcpy(uhs_vals, sdr50_700D, sizeof(uhs_vals));
        patch_hook_function(sd_setup_mode, MEM(sd_setup_mode), sd_setup_mode_log, "SD UHS");
        patch_hook_function(sd_setup_mode_in, MEM(sd_setup_mode_in), sd_setup_mode_in_log, "SD UHS");

        /* enable SDR104 */
        if (access_mode)
        {
            patch_hook_function(sd_set_function, MEM(sd_set_function), sd_set_function_log, "SDR104");
        }

        /* Get CID info */
        patch_hook_function(CID_hook, MEM(CID_hook), GetCID, "CID");

        SD_ReConfiguration();

        /* CID hook isn't needed anymore */
        unpatch_memory(CID_hook);

        // Patch cmp instruction to avoid loading default GPIO registers values
        patch_instruction32(GPIO_cmp, 0xe3550001, 0xe3550008, "GPIO_cmp");

        patch_hook_function(GPIO, MEM(GPIO), GPIO_registers, "GPIO");    // Set our GPIO values
        SD_ReConfiguration();
        GPIO_patch_on = 1;

        if (sd_overclock == 3) // Hybrid clock speed for 240 MHz
        {
            patch_hook_function(sd_read_clock, MEM(sd_read_clock), ReadClock, "R_Clock");
            patch_hook_function(sd_write_clock, MEM(sd_write_clock), WriteClock, "W_Clock");
        }

        if (sd_overclock == 1)
            memcpy(uhs_vals, sdr_160MHz1, sizeof(uhs_vals));
        if (sd_overclock == 2)
            memcpy(uhs_vals, sdr_192MHz, sizeof(uhs_vals));
        //        if (sd_overclock == 3) memcpy(uhs_vals, sdr_240MHz, sizeof(uhs_vals)); //Not needed here, it's being applied by WriteClock

        /* Not needed anymore */
        if (GPIO_patch_on)
        {
            msleep(100);
            unpatch_memory(GPIO_cmp);
            unpatch_memory(GPIO);
            GPIO_patch_on = 0;
        }
    }

    else if ((is_camera("6D", "1.1.6"))
                || (is_camera("70D", "1.1.2"))
                || (is_camera("650D", "1.0.4"))
                || (is_camera("700D", "1.1.5"))
            )
    {
        overclock_task_in_progress = 1;

        /* Patch sdReadBlk and sdWriteBlk Now! for pausing SD reads/writes operations on 6D */
        if (is_camera("6D", "1.1.6") || is_camera("70D", "1.1.2"))
        {
            patch_hook_function(sd_read_clock, MEM(sd_read_clock), PauseReadClock, "R_Clock");
            patch_hook_function(sd_write_clock, MEM(sd_write_clock), PauseWriteClock, "W_Clock");
        }

        /* install the hack */
        memcpy(uhs_vals, sdr50_700D, sizeof(uhs_vals));
        patch_hook_function(sd_setup_mode, MEM(sd_setup_mode), sd_setup_mode_log, "SD UHS");
        patch_hook_function(sd_setup_mode_in, MEM(sd_setup_mode_in), sd_setup_mode_in_log, "SD UHS");

        /* enable SDR104 */
        if (access_mode)
        {
            patch_hook_function(sd_set_function, MEM(sd_set_function), sd_set_function_log, "SDR104");
        }

        /* Get CID info */
        patch_hook_function(CID_hook, MEM(CID_hook), GetCID, "CID");

        SD_ReConfiguration();
        SD_ReConfiguration();

        /* CID hook isn't needed anymore */
        unpatch_memory(CID_hook);

        /* 160MHz1 doesn't work on 70D, 160MHz1 acts as 192 MHz on 6D / 5D3 which is weird*/
        if (sd_overclock == 1 && !is_camera("70D", "1.1.2") && !is_camera("6D", "1.1.6"))
            memcpy(uhs_vals, sdr_160MHz1, sizeof(uhs_vals));
        if (sd_overclock == 1 && (is_camera("70D", "1.1.2") || is_camera("6D", "1.1.6")))
            memcpy(uhs_vals, sdr_160MHz2, sizeof(uhs_vals));
        if (sd_overclock == 2)
            memcpy(uhs_vals, sdr_192MHz, sizeof(uhs_vals));
        if (sd_overclock == 3)
            memcpy(uhs_vals, sdr_240MHz, sizeof(uhs_vals));

        overclock_task_in_progress = 0;

        // Overclocking is done, these are not needed anymore
        if (is_camera("6D", "1.1.6") || is_camera("70D", "1.1.2"))
        {
            msleep(100);
            unpatch_memory(sd_read_clock);
            unpatch_memory(sd_write_clock);
        }
    }

    else
    {  //unsupported cam
    }
}

/* Guards the Auto Speed Test's safe-mode check to run once per boot. This is
 * a plain static (not persisted), so it naturally resets to 0 on every
 * reboot -- exactly the semantics needed, since the overclock register only
 * actually changes after a reboot. Without this guard, sd_uhs_update() (called
 * many times per second while this submenu is open) would cascade through
 * every remaining preset in a fraction of a second, since the register it
 * reads doesn't change again until the user actually restarts the camera. */
static int autotest_checked_this_boot = 0;

static MENU_UPDATE_FUNC(sd_uhs_update)
{
    /* Simple method to check if Canon safe mode get triggered (switched to 48 MHz / 21 MB/s) */
    /* Safe mode get triggered when a SD card doesn't accpet our overclocking configuration or
     * if there is an instabilty with the overclocking setting.                               */
    /* 0xC0400614 is one of the SD overclocking registers, by default Canon set it to 0x1d000601
       when using Canon 48 MHz preset */

    /* 5D3 doesn't have 48 MHz safe mode, instead of that, it locks the access to SD card directly.
     * The Auto Speed Test wizard's automatic detection relies on this register, so it only works
     * on non-5D3 models (including 6D, this project's target) -- same scope as the check below. */
    if (!is_camera("5D3", "*"))
    {
        if (turned_on)
        {
            int safe_mode_triggered = (*(uint32_t *)0xC0400614 == 0x1d000601);

            if (safe_mode_triggered)
            {
                MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Safe mode was triggered, try lower frequency or different access mode.");
            }

            if (!autotest_checked_this_boot && autotest_state == AUTOTEST_TESTING)
            {
                autotest_checked_this_boot = 1;
                if (safe_mode_triggered)
                {
                    autotest_advance("safe-mode triggered automatically");
                }
                else
                {
                    autotest_state = AUTOTEST_AWAIT_CONFIRM;
                    autotest_log("AUTOTEST: passed automatic safe-mode check, awaiting user confirmation");
                }
            }
        }
    }
}

static MENU_UPDATE_FUNC(MID_display)
{
    MENU_SET_VALUE("%#02x", MID);

    /* https://www.cameramemoryspeed.com/sd-memory-card-faq/reading-sd-card-cid-serial-psn-internal-numbers/ */
    if (MID == 0x01)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Panasonic.");
    }

    if (MID == 0x02)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Toshiba.");
    }

    if (MID == 0x03)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by SanDisk.");
    }

    if (MID == 0x1b)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Samsung.");
    }

    if (MID == 0x1d)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by AData.");
    }

    if (MID == 0x27)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Phison.");
    }

    if (MID == 0x28)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Lexar.");
    }

    if (MID == 0x31)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Silicon Power.");
    }

    if (MID == 0x41)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Kingston.");
    }

    if (MID == 0x74)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Transcend.");
    }

    if (MID == 0x76)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Patriot.");
    }

    if (MID == 0x82)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Sony.");
    }

    if (MID == 0x9C)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Manufactured by Angelbird.");
    }
}

static MENU_UPDATE_FUNC(OID_display)
{
    MENU_SET_VALUE("%#02x%02x", (OID << 0x8) >> 0x18, (OID << 0x10) >> 0x18);
}

static MENU_UPDATE_FUNC(PNM_display)
{
    MENU_SET_VALUE("%1c%1c%1c%1c%1c", PNM_1 & 0xFF, PNM_2 >> 0x18, (PNM_2 << 0x8) >> 0x18, (PNM_2 << 0x10) >> 0x18, PNM_2 & 0xFF);
}

static MENU_UPDATE_FUNC(PRV_display)
{
    MENU_SET_VALUE("%01d.%01d", PRV >> 0x1c, (PRV << 0x4) >> 0x1c);
}

static MENU_UPDATE_FUNC(PSN_display)
{
    MENU_SET_VALUE("%#02x%02x%02x%02x", (PSN_1 << 0x8) >> 0x18, (PSN_1 << 0x10) >> 0x18, PSN_1 & 0xFF, PSN_2 >> 0x18);
}

static MENU_UPDATE_FUNC(MDT_display)
{
    MENU_SET_VALUE("%04d/%02d", ((MDT << 0xc) >> 0x18) + 2000, (MDT << 0x14) >> 0x1c);
}

static MENU_UPDATE_FUNC(CRC_display)
{
    MENU_SET_VALUE("%#02x", (CRC << 0x17) >> 0x18);
}

static struct menu_entry sd_uhs_menu[] =
{
    {
        .name = "SD Overclock",
        .select = menu_open_submenu,
        .children = (struct menu_entry[]){
            {
                .priv = &sd_overclock,
                .update = sd_uhs_update,
                .max = 3,
                .help = "Choose an option, restart the camera to apply.",
                .name = "SD Frequency",
                .choices = CHOICES("OFF", "160MHz", "192MHz", "240MHz"),
            },
            {
                .name = "Access Mode",
                .priv = &access_mode,
                .max = 1,
                .choices = CHOICES("SDR50", "SDR104"),
                .help = "SDR104 mode may be required for >100 MHz. It's ON by default.",
                .help2 = "However some SD cards prefer SDR50 for high frequencies.",
            },
            {
                .name = "Show CID info",
                .select = menu_open_submenu,
                .help = "Read the contents of CID register.",
                .help2 = "CID register contains some information about an SD card.",
                .icon_type = IT_ACTION,
                .children = (struct menu_entry[]){
                    {
                        .name = "MID:",
                        .update = MID_display,
                        .help = "Manufacturer ID.",
                        .icon_type = IT_ALWAYS_ON,
                    },
                    {
                        .name = "OID:",
                        .update = OID_display,
                        .help = "OEM/Application ID.",
                        .icon_type = IT_ALWAYS_ON,
                    },
                    {
                        .name = "PNM:",
                        .update = PNM_display,
                        .help = "Product Name.",
                        .icon_type = IT_ALWAYS_ON,
                    },
                    {
                        .name = "PRV:",
                        .update = PRV_display,
                        .help = "Product Revision.",
                        .icon_type = IT_ALWAYS_ON,
                    },
                    {
                        .name = "PSN:",
                        .update = PSN_display,
                        .help = "Serial Number.",
                        .icon_type = IT_ALWAYS_ON,
                    },
                    {
                        .name = "MDT:",
                        .update = MDT_display,
                        .help = "Manufacture Date Code.",
                        .icon_type = IT_ALWAYS_ON,
                    },
                    {
                        .name = "CRC:",
                        .update = CRC_display,
                        .help = "CRC7 checksum.",
                        .icon_type = IT_ALWAYS_ON,
                    },
                    MENU_EOL,
                },
            },
            {
                .name = "Auto Speed Test",
                .select = menu_open_submenu,
                .help = "Cycle presets 240->192->160MHz, auto-skip any that hit Canon safe-mode.",
                .help2 = "Does NOT replace real recording tests -- confirms only after you test.",
                .icon_type = IT_ACTION,
                .children = (struct menu_entry[]){
                    {
                        .name = "Status:",
                        .update = autotest_status_display,
                        .icon_type = IT_ALWAYS_ON,
                    },
                    {
                        .name = "Start / Restart Test",
                        .select = autotest_start,
                        .help = "Begins at 240MHz. Restart the camera after selecting this.",
                        .icon_type = IT_ACTION,
                    },
                    {
                        .name = "Confirm Stable",
                        .select = autotest_confirm_pass,
                        .help = "Only after recording several REAL test clips successfully.",
                        .icon_type = IT_ACTION,
                    },
                    {
                        .name = "Mark Unstable",
                        .select = autotest_confirm_fail,
                        .help = "Real recording failed even though safe-mode check passed; step down.",
                        .icon_type = IT_ACTION,
                    },
                    {
                        .name = "Cancel / Reset",
                        .select = autotest_reset,
                        .help = "Stop the wizard without changing the current SD Frequency setting.",
                        .icon_type = IT_ACTION,
                    },
                    MENU_EOL,
                },
            },
            MENU_EOL,
        },
    },
};

static unsigned int sd_uhs_init()
{
    if (is_camera("5D3", "*"))
    {
        static const char *sd_choices_5d3[] = {"OFF", "160MHz", "192MHz (H)", "240MHz (H)"};
        static const char sd_choices_help2_5d3[] = "\n"
                                                   " \n"
                                                   "(H): Hybrid clock speed. Will use 192MHz for Write, 160MHz for Read.\n"
                                                   "(H): Hybrid clock speed. Will use 240MHz for Write, 160MHz for Read.\n";
        sd_uhs_menu[0].choices = sd_choices_5d3;
        sd_uhs_menu[0].help2 = sd_choices_help2_5d3;
    }

    if (is_camera("EOSM", "*") || is_camera("EOSM2", "*") || is_camera("100D", "*"))
    {
        static const char *sd_choices_others[] = {"OFF", "160MHz", "192MHz", "240MHz (H)"};
        static const char sd_choices_help2_others[] = "\n"
                                                      " \n"
                                                      " \n"
                                                      "(H): Hybrid clock speed. Will use 240MHz for Write, 192MHz for Read.\n";
        sd_uhs_menu[0].choices = sd_choices_others;
        sd_uhs_menu[0].help2 = sd_choices_help2_others;
    }

    menu_add("Prefs", sd_uhs_menu, COUNT(sd_uhs_menu));

    if (is_camera("5D3", "1.1.3"))
    {
        /* sd_setup_mode:
         * sdSendCommand: CMD%d  Retry=... ->
         * sd_configure_device(1) (called after a function without args) ->
         * sd_setup_mode(dev) if dev is 1 or 2 ->
         * logging hooks are placed both at start of sd_setup_mode and before the case switch
         */
        CID_hook = 0xff6aed04;
        sd_setup_mode = 0xFF47B4C0;    /* start of the function; not strictly needed on 5D3 */
        sd_setup_mode_in = 0xFF47B4EC; /* after loading sd_mode in R0, before the switch */
        sd_setup_mode_reg = 0;         /* switch variable is in R0 */
        sd_set_function = 0xFF6ADE34;  /* sdSetFunction */
        sd_enable_18V = 0xFF47B4B8;    /* 5D3 only (Set 1.8V Signaling) */
        sd_write_clock = 0xff6b11f4;   /* NOTE: this is sdDMAWriteBlk, not sdWriteBlk. Patching sdWriteBlk causes CACHE_COLLISION */
        sd_read_clock = 0xff6b16ac;    /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF6AFF1C;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("5D3", "1.2.3"))
    {
        CID_hook = 0xff6b9ea0;
        sd_setup_mode = 0xFF484474;
        sd_setup_mode_in = 0xFF4844A0;
        sd_setup_mode_reg = 0;
        sd_set_function = 0xFF6B8FD0;
        sd_enable_18V = 0xFF48446C;  /* 5D3 only (Set 1.8V Signaling) */
        sd_write_clock = 0xff6bc674; /* sdWriteBlk */
        sd_read_clock = 0xff6bc848;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF6BB0B8;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("6D", "1.1.6"))
    {
        CID_hook = 0xff7901e4;
        sd_setup_mode = 0xFF325A20;
        sd_setup_mode_in = 0xFF325AA8;
        sd_setup_mode_reg = 1; /* switch variable is in R1 (likely all D5 other than 5D3) */
        sd_set_function = 0xFF78F308;
        sd_write_clock = 0xff7929d4; /* sdWriteBlk */
        sd_read_clock = 0xff792cb8;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF791408;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("700D", "1.1.5"))
    {
        CID_hook = 0xff749e7c;
        sd_setup_mode = 0xFF3376E8;    /* start of the function */
        sd_setup_mode_in = 0xFF337770; /* right before the switch */
        sd_setup_mode_reg = 1;         /* switch variable is in R1 (likely all D5 other than 5D3) */
        sd_set_function = 0xFF748F18;
        sd_write_clock = 0xff74c674; /* sdWriteBlk */
        sd_read_clock = 0xff74c958;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF74B35C;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("650D", "1.0.4"))
    {
        CID_hook = 0xff740bfc;
        sd_setup_mode = 0xFF334C4C;
        sd_setup_mode_in = 0xFF334CD4;
        sd_setup_mode_reg = 1;
        sd_set_function = 0xFF73FD20;
        sd_write_clock = 0xff7433ec; /* sdWriteBlk */
        sd_read_clock = 0xff7436d0;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF7420D4;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("100D", "1.0.1"))
    {
        CID_hook = 0xff653f80;
        GPIO = 0xff335a34;
        GPIO_cmp = 0xff335a3c;
        sd_setup_mode = 0xFF3355B0;
        sd_setup_mode_in = 0xFF335648;
        sd_setup_mode_reg = 1;
        sd_set_function = 0xFF6530A4;
        sd_write_clock = 0xff656770; /* sdWriteBlk */
        sd_read_clock = 0xff656a54;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF655458;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("EOSM", "2.0.2"))
    {
        CID_hook = 0xff63fe3c;
        GPIO = 0xff3391f8;
        GPIO_cmp = 0xff339200;
        sd_setup_mode = 0xFF338D40;
        sd_setup_mode_in = 0xFF338DC8;
        sd_setup_mode_reg = 1;
        sd_set_function = 0xFF63EF60;
        sd_write_clock = 0xff64262c; /* sdWriteBlk */
        sd_read_clock = 0xff642910;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF641314;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("EOSM2", "1.0.3"))
    {
        CID_hook = 0xff693c10; // original address was ff693c58, changed it to ff693c10 to avoid cache collision
        GPIO = 0xff349a10;
        GPIO_cmp = 0xff349a18;
        sd_setup_mode = 0xff3495cc; // original address was ff349550, changed it to ff3495cc to avoid cache collision with crop_rec.mo
        sd_setup_mode_in = 0xff349624;
        sd_setup_mode_reg = 1;
        sd_set_function = 0xff692d7c;
        sd_write_clock = 0xff696164; /* NOTE: this is sdDMAWriteBlk, not sdWriteBlk. Patching sdWriteBlk causes CACHE_COLLISION */
        sd_read_clock = 0xff69672c;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xff695130;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    if (is_camera("70D", "1.1.2"))
    {
        CID_hook = 0xff7cf394;
        sd_setup_mode = 0xFF33E078;
        sd_setup_mode_in = 0xFF33E100;
        sd_setup_mode_reg = 1;
        sd_set_function = 0xFF7CE4B8;
        sd_write_clock = 0xff7d18a0; /* NOTE: this is sdDMAWriteBlk, not sdWriteBlk. Patching sdWriteBlk causes CACHE_COLLISION */
        sd_read_clock = 0xff7d1e68;  /* sdReadBlk */
        SD_ReConfiguration = (void *)0xFF7D086C;

        if (sd_overclock)
        {
            sd_overclock_task();
            turned_on = 1;
        }
    }

    return 0;
}

static unsigned int sd_uhs_deinit()
{
    return 0;
}

MODULE_INFO_START()
MODULE_INIT(sd_uhs_init)
MODULE_DEINIT(sd_uhs_deinit)
MODULE_INFO_END()

MODULE_CONFIGS_START()
MODULE_CONFIG(sd_overclock)
MODULE_CONFIG(access_mode)
MODULE_CONFIG(autotest_state)
MODULE_CONFIG(autotest_preset)
MODULE_CONFIGS_END()
