/*
 * fae_demo.c - Minimal wake-reason reproduction for TXW82x FAE.
 *
 * Single file, no MQTT, no camera. Reproduces three issues documented
 * in custom/doc/NE102 FAE 唤醒原因问题包.md:
 *
 *   1. sys_wakeup_reason() appears to return the PREVIOUS park's
 *      reason "right after wake" - actually the +0ms read executes
 *      BEFORE the park: system_sleep() returns asynchronously (the
 *      sleep RPC is dispatched, the park starts ~120ms later), so
 *      code right after the call runs in the ENTRY WINDOW. The true
 *      reason lands when the resume chain finishes (~180ms after
 *      revival, delivered by the RESUME sleepcb).
 *
 *   2. dsleep_set_wakeup_hook: registered correctly (verified by
 *      dumping dcfg+0x41c). The old "NEVER fires" evidence
 *      (hook_latch stays 0xEE) is unreliable: the latch lives in
 *      __dsleep_data, which is reloaded from flash at every park
 *      entry, wiping hook writes (2026-10-22 revision).
 *
 *   3. dsleep_set_enter_hook / dsleep_set_exit_hook: no call sites
 *      exist anywhere in the linked image (dead code).
 *
 * Lifecycle instrumentation (2026-10-22), one report line per cycle:
 *   - call->ret / ret->rsn0 / rsn0->suspcb : entry-window proof
 *     (rsn0->suspcb > 0 means the +0 read ran BEFORE sleeping)
 *   - park_span (jiffies) / rc_span (RC)   : how long we really slept
 *   - run->cb                               : software-running-again
 *     until framework resume completes
 * The physical wake instant is visible in the serial log (the ULP
 * "WK:"/"cpu0 sleep_exit" lines); the "FAE: software running again"
 * print gives its counterpart - the wall-clock gap between those two
 * lines is the wake->software-running latency.
 *
 * Usage:
 *   - AT+FAE=30       -> enter 30-second SRAM_WIFI sleep cycles.
 *   - AT+FAE=0        -> stop after the current cycle.
 *   - AT+FAE?         -> dump the evidence counters.
 *   - Send a TCP packet to the device (e.g. netcat) while it sleeps
 *     -> data wake -> observe the reason lag and hook silence.
 */
#include <basic_include.h>
#include <osal/sleep.h>
#include <lib/atcmd/libatcmd.h>
#include <lib/lmac/lmac_dsleep.h>
#include <lwip/netif.h>
#include <syscfg.h>

/* ---- wake-reason evidence (always-on section survives parks) ---- */
__dsleep_data static volatile uint8_t fae_hook_latch = 0xEE;

/* dcfg struct base - the park program reads these fields (offsets
 * verified by disassembly of the final image, 2026-10-19) */
#define FAE_DCFG ((uint8_t *)bgn_dsleep)
#define FAE_OFF_REASON 0x3a9  /* sys_wakeup_reason() reads this */
#define FAE_OFF_HOOK   0x41c  /* dsleep_wakeup_hook pointer */
#define FAE_OFF_WKDET  0x410  /* usr_wkdet_cb pointer */

/* ---- state ---- */
static uint8_t fae_running;
static uint8_t fae_wake_count;
static uint8_t fae_cb_reason;
static uint8_t fae_cb_seen;

/* ---- lifecycle stamps (2026-10-22) ----
 * system_sleep() returns asynchronously (sleep RPC dispatched, park
 * starts ~120ms later) - code after the call runs in the ENTRY
 * WINDOW. jiffies/RC freeze during the park and are compensated
 * during the resume chain, so a stamp only reflects post-park time
 * after the compensation jump. */
static uint64_t fae_j_call;    /* just before system_sleep */
static uint64_t fae_j_ret;     /* system_sleep returned (entry window) */
static uint64_t fae_j_rsn0;    /* the "+0ms" reason read (entry window) */
static uint64_t fae_j_susp;    /* last SUSPEND sleepcb = really sleeping */
static uint64_t fae_j_park;    /* the 10ms sleep the park hid inside */
static uint64_t fae_j_run;     /* first task code after the park */
static uint64_t fae_j_cb;      /* first RESUME sleepcb */
static uint32_t fae_rc_ret, fae_rc_susp, fae_rc_run;
static uint8_t  fae_parked;

/* ---- sleepcb: framework resume callback (the reliable reason source).
 * Latches only (callback context): the last SUSPEND step marks "really
 * sleeping" (~1ms before the park image loads), the first RESUME step
 * marks the framework resume chain reaching our callback. ---- */
static int32_t fae_sleepcb(uint16_t type, struct sys_sleepcb_param *args,
                           void *priv)
{
    (void)type; (void)priv;
    if (args->action == SYS_SLEEPCB_ACTION_RESUME) {
        if (!fae_cb_seen) {
            fae_j_cb = os_jiffies();
            fae_cb_reason = args->resume.wkreason;
        }
        fae_cb_reason = args->resume.wkreason;
        fae_cb_seen = 1;
    } else {
        fae_j_susp = os_jiffies();      /* overwritten per step: the
                                         * APP step (last) survives */
        fae_rc_susp = pmu_tmraol_get();
    }
    return RET_OK;
}

/* ---- wakeup_hook: should fire on IO/TIMER wake per the guide §2.2.6.
 * Board finding: NEVER called on mode-1 parks. The latch byte is the
 * evidence (0xEE survives = hook did not run). ---- */
static int32_t fae_wakeup_hook(uint8_t wk_reason)
{
    fae_hook_latch = wk_reason;
    return DSLEEP_ACTION_DO_NOTHING;
}

/* ---- wkdet: wake-on-data filter (payload TCP to our IP) ---- */
static int32_t fae_wkdet(void *priv, uint8_t *data, uint32_t len)
{
    (void)priv;
    /* wake on any payload TCP */
    if (len >= 24 && (data[0] >> 4) == 4 && data[9] == 0x06) {
        return 1;
    }
    return 0;
}

/* ---- sleep loop ---- */
static uint16_t fae_cycles_left;

static void fae_sleep_task(void *arg)
{
    struct system_sleep_param sp;
    uint32_t seg_ms = (uint32_t)(uintptr_t)arg;

    (void)arg;
    os_printf(KERN_INFO "FAE: sleep loop start (seg=%ums cycles=%u)\n",
              seg_ms, fae_cycles_left);
    fae_running = 1;

    while (fae_running && fae_cycles_left--) {
        int32_t sr;
        uint8_t rsn_0, rsn_200, rsn_500;
        uint32_t t;

        os_memset(&sp, 0, sizeof(sp));
        sp.sleep_ms = seg_ms;

        fae_hook_latch = 0xEE;
        fae_cb_seen = 0;
        fae_parked = 0;
        fae_j_susp = 0;
        fae_j_cb = 0;
        fae_j_run = 0;
        dsleep_set_usr_wkdet_cb(NULL, fae_wkdet);
        dsleep_set_wakeup_hook(fae_wakeup_hook);

        /* Everything up to the suspend sleepcb runs in the ENTRY
         * WINDOW: system_sleep() has only DISPATCHED the sleep. */
        fae_j_call = os_jiffies();
        sr = system_sleep(SYSTEM_SLEEP_TYPE_SRAM_WIFI, &sp);
        fae_j_ret = os_jiffies();          /* still pre-park */
        fae_rc_ret = pmu_tmraol_get();

        rsn_0 = sys_wakeup_reason();       /* executed BEFORE sleeping */
        fae_j_rsn0 = os_jiffies();

        /* wait for the RESUME sleepcb; the park hides inside one of
         * these 10ms sleeps - its overshoot (>50ms) stamps the first
         * post-park task code, i.e. "software running again" */
        for (t = 0; t < 200 && !fae_cb_seen; t += 10) {
            uint64_t j0 = os_jiffies();

            fae_j_park = j0;
            os_sleep_ms(10);
            if (!fae_parked && os_jiffies() - j0 > 50) {
                fae_parked = 1;
                fae_j_run = os_jiffies();
                fae_rc_run = pmu_tmraol_get();
                os_printf(KERN_INFO "FAE: software running again "
                          "(j=%u rc=%u) - physical wake is the WK/cpu0 "
                          "sleep_exit line above\n",
                          (uint32_t)fae_j_run, fae_rc_run);
            }
        }
        rsn_200 = sys_wakeup_reason();
        for (; t < 500; t += 10) {
            os_sleep_ms(10);
        }
        rsn_500 = sys_wakeup_reason();

        fae_wake_count++;
        os_printf(KERN_INFO "FAE: wake #%u sr=%d\n"
                  "  rsn: +0ms=%u  +200ms=%u  +500ms=%u  cb=%u(%u)\n"
                  "  hook_latch=0x%02X (0xEE=never ran; see header "
                  "caveat)\n",
                  fae_wake_count, (int)sr,
                  rsn_0, rsn_200, rsn_500,
                  fae_cb_seen, fae_cb_reason,
                  fae_hook_latch);
        {
            int32_t hz = bgn_dsleep ? bgn_dsleep->rc_hz : 0;

            os_printf(KERN_INFO
                "FAE life: call->ret=%ums ret->rsn0=%ums "
                "rsn0->suspcb=%ums (rsn0 PRE-PARK if >0) | "
                "park_rc=%ums jiffies_gap=%ums (jiffies LOSE the "
                "park time) | run->cb=%dms cb->report=%ums\n",
                (uint32_t)(fae_j_ret - fae_j_call),
                (uint32_t)(fae_j_rsn0 - fae_j_ret),
                (uint32_t)(fae_j_susp - fae_j_rsn0),
                (fae_parked && hz > 0)
                    ? (uint32_t)((uint64_t)(fae_rc_run - fae_rc_susp)
                                 * 1000 / (uint64_t)hz) : 0,
                fae_parked ? (uint32_t)(fae_j_run - fae_j_park) : 0,
                (int32_t)(fae_j_cb - fae_j_run),
                (uint32_t)(os_jiffies() - fae_j_cb));
        }

        mcu_watchdog_feed();
    }

    dsleep_set_wakeup_hook(NULL);
    dsleep_set_usr_wkdet_cb(NULL, NULL);
    os_printf(KERN_INFO "FAE: sleep loop end\n");
}

/* ---- AT command ---- */
int32_t ne102_atcmd_fae(const char *cmd, char *argv[], uint32_t argc)
{
    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        struct netif *nif = netif_find("w0");

        atcmd_resp("running=%u wakes=%u cb_reason=%u hook_latch=0x%02X",
                   fae_running, fae_wake_count, fae_cb_reason,
                   fae_hook_latch);
        atcmd_resp("ip=%s dcfg=%p hook@0x41c=%p want=%p wkdet@0x410=%p",
                   nif && nif->ip_addr.addr
                       ? ipaddr_ntoa(&nif->ip_addr) : "none",
                   bgn_dsleep,
                   bgn_dsleep
                       ? *(void **)(FAE_DCFG + FAE_OFF_HOOK) : NULL,
                   (void *)fae_wakeup_hook,
                   bgn_dsleep
                       ? *(void **)(FAE_DCFG + FAE_OFF_WKDET) : NULL);
        return 0;
    }
    if (argc >= 1 && argv[0][0]) {
        int32_t secs, cycles = 1;

        if (argv[0][0] == '0' && !argv[0][1]) {
            fae_running = 0;
            atcmd_resp("OK: stop after the current cycle");
            return 0;
        }
        secs = os_atoi(argv[0]);
        if (argc >= 2 && argv[1][0])
            cycles = os_atoi(argv[1]);
        if (secs < 5 || secs > 3600 || cycles < 1 || cycles > 999) {
            atcmd_resp("ERR: 5..3600 seconds, 1..999 cycles");
            return -1;
        }
        if (fae_running) {
            atcmd_resp("ERR: already running");
            return -1;
        }
        fae_cycles_left = (uint16_t)cycles;
        sys_register_sleepcb(fae_sleepcb, NULL);
        os_task_create("fae_demo", fae_sleep_task,
                       (void *)(uintptr_t)(secs * 1000),
                       OS_TASK_PRIORITY_NORMAL, 0, NULL, 4096);
        atcmd_resp("OK: FAE sleep %us x%u cycle(s), TCP payload wakes",
                   secs, cycles);
        return 0;
    }
    atcmd_resp("usage: AT+FAE=<secs 5..3600>[,<cycles 1..999>] | "
               "AT+FAE=0 (stop) | AT+FAE?");
    return 0;
}

/* ---- init/interop (called from app_power_test_init) ---- */
int fae_demo_running(void)
{
    return fae_running;
}

int fae_demo_init(void)
{
    atcmd_register("AT+FAE", ne102_atcmd_fae);
    os_printf(KERN_INFO "FAE: wake-reason demo ready (AT+FAE=<secs>, "
              "0=stop; do not run together with AT+PWRS/PWRDT)\n");
    return RET_OK;
}
