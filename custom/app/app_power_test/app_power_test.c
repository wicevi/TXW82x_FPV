/*
 * app_power_test.c - sleep-mode test demo (CUSTOMER_ID=11, POWER_TEST_DEMO).
 *
 * Board bring-up (LED / camera / TF rails) stays in app_lowpwr_camera.c;
 * this file only adds the test logic. Entry: app_power_test_init() is
 * dispatched from app_lowpwr_camera_init().
 *
 * Default state after init: the status LED (PC0) blinks at 1 Hz - a
 * liveness heartbeat for bench observation. Boot prints the current
 * wake reason (the only moment a reboot-style sleep's reason is
 * readable). The LED is forced OFF
 * during a sleep test so it never adds current to a measurement.
 *
 * AT commands:
 *   AT+PWR?                     status: uptime, wake count, last reasons
 *   AT+PWRS=<type>,<secs>       one-shot sleep test, secs 5..3600
 *                               type: 1=SRAM_WIFI (checkpoint resume, wifi
 *                                     association maintained by the
 *                                     low-power program)
 *                                     2=WIFI_DSLEEP 3=SRAM_ONLY 4=LIGHT
 *                                     5=RTCC (wake = REBOOT, the test
 *                                     task does not resume - the print
 *                                     appears on the next boot instead)
 *   AT+PWRDT=<secs>[,<pad>]     DTIM-callback-timed wake (SRAM_WIFI
 *                               only): RC deadline padded <pad>s
 *                               (default 10) past the target; the
 *                               DTIM wakeup_hook counts windows,
 *                               judges time-up on the RC clock and
 *                               returns DSLEEP_ACTION_SLEEP_EXIT
 *                               itself (the RC timer wake is
 *                               AT+PWRS's job)
 *   AT+PWRLED=<0|1>             LED blink on/off (keep off for current
 *                               measurements)
 *   AT+PWRLOG=<0..7>            runtime print level (7 exposes the SDK
 *                               KERN_DEBUG traces, e.g. sdhost probe)
 *   AT+PWRDBG=<0|1>             dsleep-library debug prints (the per-DTIM
 *                               "Bingo" RC-calibration lines etc.);
 *                               REFUSED while a sleep test runs - a
 *                               dsleep ioctl inside the async entry
 *                               window corrupts the park (LVD reset,
 *                               board finding 2026-10-22)
 *   AT+PWRHK=<0|1>              dsleep hook probe: registers
 *                               wakeup/enter/exit/rx/ready/loop + all-step
 *                               sleepcb with seq+RC-timestamp prints (call
 *                               order/timing analysis; loop 1/500; NOT for
 *                               current measurement); same entry-window
 *                               refusal as PWRDBG
 *   AT+PWCAP / AT+PWCURL        wake-capture cycle test - lives in
 *                               custom/component/wake_capture/ (registered
 *                               from here); parks with this file's
 *                               pt_sleep_sync() wrapper
 *   AT+PWDWK=<0|1>              wake-on-unicast-data filter for the
 *                               keepalive parks: UDP-to-us (any) or
 *                               TCP-with-payload-to-us wakes the parked
 *                               device; bare ACKs ignored (bench: send a
 *                               UDP packet from the PC while AT+PWRS
 *                               parks - the wake report prints the
 *                               wkdet latch)
 *
 * All park paths go through the synchronous wrapper (pt_sleep_sync /
 * pt_sleep_park + pt_sleep_wait): system_sleep() is an asynchronous RPC
 * dispatch, so everything after a plain call runs in the pre-park entry
 * window - the wrapper blocks on a semaphore posted by the sleepcb
 * RESUME step and returns only after the real wake (see app_power_test.h).
 *
 * Network provisioning: use the system's built-in AT+SSID / AT+KEY
 * commands (nothing extra here).
 */
#include <basic_include.h>
#include <osal/sleep.h>
#include <osal/semaphore.h>
#include <hal/gpio.h>
#include <pin_param.h>
#include <lib/atcmd/libatcmd.h>
#include <lib/umac/ieee80211.h>
#include <lib/lmac/lmac_dsleep.h>
#include <lib/common/dsleepdata.h>
#include <lwip/netif.h>
#include <lowPower_app.h>
#include "syscfg.h"
#include "app_power_test.h"

#define PT_LED_PIN      PC_0    /* board status LED, same as the bench */

/* ---- runtime state ---- */
static volatile uint8_t pt_blink_en = 1;
static volatile uint8_t pt_sleeping;
static uint8_t  pt_wake_count;
static uint8_t  pt_cb_rsn;      /* sleepcb per-wake reason (live value) */
static volatile uint8_t pt_cb_fresh;
static uint8_t  pt_last_cb_rsn; /* reason of the previous wake (AT+PWR?) */
static uint64_t pt_rc_before;   /* guide 2.2.5: RC around the sleep */
static uint64_t pt_rc_after;
static int32_t  pt_rc_hz;       /* calibrated RC frequency (dsleep_priv) */

/* ---- synchronous sleep wrapper ----
 * system_sleep() only dispatches the park RPC and returns in ~2-3ms;
 * code after a plain call runs in the ENTRY WINDOW (the task keeps
 * running ~120ms more until the suspend chain freezes it) - the
 * fragility behind the dtim disarm bug and every timed reason-sampling
 * loop this demo used to need. The wrapper makes the call synchronous:
 * the framework sleepcb RESUME step posts pt_resume_sema (the only
 * wake signal that has never been wrong or early - mqttka board
 * finding 2026-10-20), and pt_sleep_wait() blocks on it. Counting
 * semantics make the "resume cbs fire before the loop regains control"
 * race (mqttka) harmless: an early up() leaves the count at 1 and the
 * following down() returns immediately. */
static os_semaphore_t pt_resume_sema;
static volatile uint8_t pt_parking;  /* between park() and wait() - the
                                      * blink task keeps the LED dark */

/* ---- AT+PWDWK=<0|1>: wake-on-unicast-data filter (dsleep wkdet) ----
 * Registers the dsleep user wake-detector for the keepalive parks: a
 * parked SRAM_WIFI device wakes as soon as an inbound IPv4 packet is
 * addressed to OUR unicast IP and is either UDP (any datagram) or TCP
 * WITH payload. Bare TCP ACKs (NAT/broker keepalive probes) are
 * ignored - mqttka measured ~4 spurious data wakes / 6 min otherwise
 * (board finding 2026-10-15). Broadcast/multicast/beacon traffic is
 * never unicast-to-us and never wakes.
 *
 * Callback context = the low-power park program (mqttka-proven rules):
 * single-byte latch write only, os_memcpy for the header field is ok,
 * NO prints/libc. The latch lives in the always-on dsleep section so
 * the park-context write survives to the app-side wake report. */
static uint8_t pt_wkdet_en;
static uint32_t pt_wk_ip;      /* cached net-order w0 address, 0=unknown */

__dsleep_data static uint8_t pt_wk_latch;  /* 0xFF = no approved packet
                                            * this park = timer wake */

static int32_t pt_wkdet(void *priv, uint8_t *data, uint32_t len)
{
    /* data = IPv4 header + following bytes (same convention as the
     * lowPower_app user_check_data path mqttka verified) */
    uint32_t dst, ihl, tot, thl;

    (void)priv;
    if (len < 20 || (data[0] >> 4) != 4 || pt_wk_ip == 0) {
        return 0;
    }
    os_memcpy(&dst, data + 16, 4);
    if (dst != pt_wk_ip) {
        return 0;   /* not our unicast IP */
    }
    if (data[9] == 0x11) {           /* UDP: any datagram to us wakes */
        pt_wk_latch = DSLEEP_WK_REASON_WK_DATA;
        return 1;
    }
    if (data[9] == 0x06) {           /* TCP: only payload-bearing */
        ihl = (data[0] & 0x0F) * 4;
        if (ihl < 20 || len < ihl + 20) {
            return 0;
        }
        tot = ((uint32_t)data[2] << 8) | data[3];
        thl = ((uint32_t)data[ihl + 12] >> 4) * 4;
        if (tot > ihl + thl) {
            pt_wk_latch = DSLEEP_WK_REASON_WK_DATA;
            return 1;
        }
    }
    return 0;
}

int32_t pt_sleep_park(uint8_t type, uint32_t sleep_ms)
{
    struct system_sleep_param sp;

    if (type < SYSTEM_SLEEP_TYPE_SRAM_WIFI || type >= SYSTEM_SLEEP_TYPE_MAX) {
        return RET_ERR;
    }
    /* wake-on-unicast-data filter (AT+PWDWK=1): refresh the IP cache
     * and re-register before every park (mqttka pattern - belt and
     * braces against slot churn), arm the no-data sentinel */
    if (pt_wkdet_en) {
        struct netif *nif = netif_find("w0");

        if (nif && nif->ip_addr.addr) {
            pt_wk_ip = nif->ip_addr.addr;
        }
        pt_wk_latch = 0xFF;
        dsleep_set_usr_wkdet_cb(NULL, pt_wkdet);
    }

    os_memset(&sp, 0, sizeof(sp));
    sp.sleep_ms = sleep_ms;  /* RC deadline: always armed, so the park can
                              * never hang even without any other source */
    /* drain a stale up() (e.g. a resume that landed after an abandoned
     * wait timed out) so the following wait() only sees THIS park's
     * resume; the up-between-dispatch-and-wait race stays correct -
     * counting semaphores hold that count for the down() */
    os_sema_eat(&pt_resume_sema);
    pt_parking = 1;
    /* 5ms: let pending log bytes leave the UART FIFO before the freeze
     * (mqttka finding: 30ms was waste, 5ms margin is enough @921600) */
    os_sleep_ms(5);
    {
        int32_t sr = system_sleep((SYSTEM_SLEEP_TYPE)type, &sp);

        if (sr != RET_OK) {
            pt_parking = 0;   /* refused: the park never starts */
        }
        return sr;
    }
}

/* Returns RET_OK once the RESUME step ran (device slept and woke);
 * RET_ERR on timeout (park refused by the framework, or the window
 * elapsed without a resume - e.g. an aborted sleep). */
int32_t pt_sleep_wait(uint32_t timeout_ms)
{
    int32_t ok = os_sema_down(&pt_resume_sema, (int32_t)timeout_ms) > 0;

    pt_parking = 0;
    return ok ? RET_OK : RET_ERR;
}

int32_t pt_sleep_sync(uint8_t type, uint32_t sleep_ms, uint32_t timeout_ms)
{
    int32_t sr = pt_sleep_park(type, sleep_ms);

    if (sr != RET_OK) {
        return sr;
    }
    return pt_sleep_wait(timeout_ms);
}

uint8_t pt_sleep_wake_reason(void)
{
    return pt_cb_rsn;   /* live sleepcb value, fresh after wait()=RET_OK */
}

uint8_t pt_sleep_active(void)
{
    return pt_sleeping;
}


static const char *pt_type_name(uint8_t t)
{
    switch (t) {
    case SYSTEM_SLEEP_TYPE_SRAM_WIFI:   return "SRAM_WIFI";
    case SYSTEM_SLEEP_TYPE_WIFI_DSLEEP: return "WIFI_DSLEEP";
    case SYSTEM_SLEEP_TYPE_SRAM_ONLY:   return "SRAM_ONLY";
    case SYSTEM_SLEEP_TYPE_LIGHT_SLEEP: return "LIGHT";
    case SYSTEM_SLEEP_TYPE_RTCC:        return "RTCC";
    default:                            return "?";
    }
}

static const char *pt_rsn_name(uint8_t r)
{
    switch (r) {
    case DSLEEP_WK_REASON_TIMER:
    case DSLEEP_WK_REASON_TIM:
    case DSLEEP_WK_REASON_BC_TIM:
    case DSLEEP_WK_REASON_RTC_ALARM:
        return "timer";
    case DSLEEP_WK_REASON_STA_DATA:
    case DSLEEP_WK_REASON_WK_DATA:
        return "remote-data";
    case DSLEEP_WK_REASON_BEACON_LOST:
    case DSLEEP_WK_REASON_AP_ERROR:
    case DSLEEP_WK_REASON_HB_TIMEOUT:
    case DSLEEP_WK_REASON_PS_DISCONNECT:
    case DSLEEP_WK_REASON_STA_ERROR:
    case DSLEEP_WK_REASON_SLEEPED_STA_ERROR:
    case DSLEEP_WK_REASON_WEAK_SIGNAL:
        return "link-lost";
    case DSLEEP_WK_REASON_IO0:
    case DSLEEP_WK_REASON_IO1:
    case DSLEEP_WK_REASON_IO2:
    case DSLEEP_WK_REASON_IO3:
    case DSLEEP_WK_REASON_IO4:
    case DSLEEP_WK_REASON_IO5:
    case DSLEEP_WK_REASON_PIR:
        return "io";
    default:
        return "other";
    }
}

/* framework sleep callback: RESUME delivers the LIVE per-wake reason
 * (the only per-wake-reliable source; sys_wakeup_reason() lags one
 * sleep - board findings 2026-10-19/21) and posts the resume semaphore
 * (pt_sleep_sync's return edge). */
static int32_t pt_sleepcb(uint16_t type, struct sys_sleepcb_param *args,
                          void *priv)
{
    (void)type;
    (void)priv;
    if (args->action == SYS_SLEEPCB_ACTION_RESUME) {
        pt_cb_rsn = args->resume.wkreason;
        pt_cb_fresh = 1;
        os_sema_up(&pt_resume_sema);
    }
    return RET_OK;
}

/* ---- DTIM-callback-timed wake (AT+PWRDT) ----
 * Park with the RC sleep deadline padded (pt_dtim_pad_ms, default
 * 10s, command-tunable) past the target so the ENTRY deadline
 * cannot fire first; the DTIM wakeup_hook counts wake windows,
 * judges time-up on the RC clock (the only clock running during a
 * park) and RETURNS DSLEEP_ACTION_SLEEP_EXIT - the wake decision
 * is entirely the callback's, the RC timer wake path is AT+PWRS's
 * job. THIS COMMAND EXISTS TO TEST SLEEP_EXIT (user ruling
 * 2026-10-22).
 *
 * History (runs 1-8, 2026-10-21): every early run showed "zero
 * hook dispatch" - root cause was OUR OWN disarm: system_sleep()
 * returns asynchronously after the :368 RPC, and disarming the
 * hooks in that window NULLs them BEFORE the park starts (the
 * "PT: hooks disarmed" print sat between the :368/:379 entry
 * prints). With disarm moved after the sleepcb RESUME, the
 * wakeup_hook dispatches ~once per DTIM window. fae_demo's
 * "wakeup_hook never ran" record suffers the same pattern plus a
 * polluted __dsleep_data latch.
 *
 * State is ORDINARY .bss on purpose: the __dsleep_data section is
 * reloaded from flash at every park entry, so app-side writes there
 * never reach the hook and hook writes get wiped by the next
 * reload; plain globals are visible to the hook context and hook
 * writes survive the parks (the hook-probe counters proved both
 * directions 2026-10-21). */
static volatile uint8_t pt_dtim_armed;
static uint32_t pt_dtim_target_ms;
static int32_t  pt_dtim_hz;
static uint32_t pt_dtim_rc0;      /* 32-bit RC at arm = hook clock base */
static uint32_t pt_dtim_prev_rc;
static uint32_t pt_dtim_iv_min, pt_dtim_iv_max, pt_dtim_iv_sum;
static volatile uint32_t pt_dtim_cnt;
static uint32_t pt_dtim_rc_exit;  /* RC at the SLEEP_EXIT decision */
static uint32_t pt_dtim_rc_r0;    /* app RC right after system_sleep */
static uint8_t  pt_dtim_exit_by;  /* 0=none 1=rc-time 2=count-backstop */
/* what the FIRST hook call saw of the app-written state (view
 * latch - separates "never invoked" from "invoked, stale view") */
static volatile uint32_t pt_dtim_calls;
static uint8_t  pt_dtim_v_armed, pt_dtim_v_wkr;
static uint32_t pt_dtim_v_target;
static int32_t  pt_dtim_v_hz;
static uint32_t pt_dtim_pad_ms = 10 * 1000;   /* RC deadline pad */
static void pt_hooks_dump(void);   /* probe totals in the dtim report */
static void pt_hooks_set(uint8_t en);  /* arm/disarm via the proven path */

/* wakeup hook - THE time judge. Fires ~once per DTIM window during
 * the park (board-proven once the disarm-timing bug was fixed).
 * Counts windows, latches its own clock base on the first call,
 * judges time-up on the RC clock (division-free cross-multiply) and
 * returns DSLEEP_ACTION_SLEEP_EXIT - per the libdsleep disassembly
 * that return takes the standard full-resume path. DO_NOTHING
 * leaves the ULP default (RF init -> ready -> keepalive loop ->
 * re-doze). Hook context: count/latch only, NO prints. */
static int32_t pt_dtim_wakeup(uint8_t wk_reason)
{
    uint32_t rc = pmu_tmraol_get();
    uint32_t iv;

    pt_dtim_calls++;
    if (pt_dtim_calls == 1) {
        pt_dtim_rc0 = rc;            /* clock base latched hook-side */
        pt_dtim_prev_rc = rc;
        pt_dtim_v_armed = pt_dtim_armed;
        pt_dtim_v_wkr = wk_reason;
        pt_dtim_v_target = pt_dtim_target_ms;
        pt_dtim_v_hz = pt_dtim_hz;
        return DSLEEP_ACTION_DO_NOTHING;   /* no interval basis yet */
    }
    if (!pt_dtim_armed)
        return DSLEEP_ACTION_DO_NOTHING;
    pt_dtim_cnt++;
    iv = rc - pt_dtim_prev_rc;             /* wrap-safe uint32 */
    if (!pt_dtim_iv_min || iv < pt_dtim_iv_min)
        pt_dtim_iv_min = iv;
    if (iv > pt_dtim_iv_max)
        pt_dtim_iv_max = iv;
    pt_dtim_iv_sum += iv;
    pt_dtim_prev_rc = rc;
    /* time-up: the exit decision belongs to THIS callback */
    if (pt_dtim_hz > 0 && (uint64_t)(rc - pt_dtim_rc0) * 1000
        >= (uint64_t)pt_dtim_target_ms * (uint64_t)pt_dtim_hz) {
        pt_dtim_rc_exit = rc;
        pt_dtim_exit_by = 1;         /* SLEEP_EXIT on RC verdict */
        pt_dtim_armed = 0;
        return DSLEEP_ACTION_SLEEP_EXIT;
    }
    /* count backstop: if the RC ever froze, the rc test cannot fire
     * and the park would only end at the padded deadline; ~1 window
     * per second assumed with 3x margin */
    if (pt_dtim_cnt >= pt_dtim_target_ms / 1000 * 3 + 20) {
        pt_dtim_rc_exit = rc;
        pt_dtim_exit_by = 2;         /* SLEEP_EXIT on count backstop */
        pt_dtim_armed = 0;
        return DSLEEP_ACTION_SLEEP_EXIT;
    }
    return DSLEEP_ACTION_DO_NOTHING;
}

static void pt_dtim_arm(uint32_t target_ms)
{
    pt_dtim_target_ms = target_ms;
    pt_dtim_cnt = 0;
    pt_dtim_calls = 0;
    pt_dtim_v_armed = pt_dtim_v_wkr = 0;
    pt_dtim_v_target = 0;
    pt_dtim_v_hz = 0;
    /* NO app-side RC read here: reading the RC counter right before
     * system_sleep corrupted the park on this board (mqttka finding
     * 2026-10-15, re-confirmed by runs 1-5 zero-dispatch 2026-10-21);
     * the hook latches its own clock base on its first call */
    pt_dtim_rc0 = 0;
    pt_dtim_prev_rc = 0;
    pt_dtim_iv_min = pt_dtim_iv_max = pt_dtim_iv_sum = 0;
    pt_dtim_rc_exit = 0;
    pt_dtim_exit_by = 0;
    pt_dtim_armed = 1;
    /* Registration happens in the AT handler via pt_hooks_set(1)
     * plus a wakeup-slot override (see pt_atcmd_dtim); the early
     * "zero dispatch" runs were caused by the disarm-timing bug,
     * not by where registration ran (that was bisected out). */
}

static void pt_dtim_disarm(void)
{
    pt_dtim_armed = 0;
    /* disarm through the proven probe path (NULLs all six hooks and
     * restores the quiet sleepcb) */
    pt_hooks_set(0);
}

/* wake window: DTIM-timing verdict (task context, safe to print) */
static void pt_dtim_report(uint32_t target_ms)
{
    uint32_t n = pt_dtim_cnt ? pt_dtim_cnt - 1 : 0;   /* intervals */
    uint32_t avg = n ? pt_dtim_iv_sum / n : 0;

    os_printf(KERN_INFO "  dtim: wakeup-calls=%u first-call view: "
              "armed=%u wkr=%u target=%u hz=%d (app set: armed=1 "
              "target=%u hz=%d)\n",
              pt_dtim_calls, pt_dtim_v_armed, pt_dtim_v_wkr,
              pt_dtim_v_target, pt_dtim_v_hz, target_ms, pt_dtim_hz);
    if (!pt_dtim_exit_by) {
        os_printf(KERN_INFO "  dtim: hook never decided an exit "
                  "(calls=%u cnt=%u) - no windows, dispatch broken, "
                  "or the padded RC deadline fired first\n",
                  pt_dtim_calls, pt_dtim_cnt);
        pt_hooks_dump();
        return;
    }
    if (pt_dtim_hz > 0) {
        uint64_t ticks = pt_dtim_rc_exit - pt_dtim_rc0;   /* wrap-safe */
        uint32_t elapsed = (uint32_t)(ticks * 1000 / (uint64_t)pt_dtim_hz);

        os_printf(KERN_INFO
                  "  dtim: wins=%u iv min/avg/max=%u/%u/%ums\n"
                  "  dtim: exit_by=%s(SLEEP_EXIT) hook-elapsed=%ums "
                  "target=%ums err=%+dms (rc deadline was padded "
                  "+%us)\n",
                  pt_dtim_cnt,
                  pt_dtim_iv_min * 1000 / (uint32_t)pt_dtim_hz,
                  avg * 1000 / (uint32_t)pt_dtim_hz,
                  pt_dtim_iv_max * 1000 / (uint32_t)pt_dtim_hz,
                  pt_dtim_exit_by == 1 ? "rc-verdict" : "backstop",
                  elapsed, target_ms,
                  (int32_t)elapsed - (int32_t)target_ms,
                  pt_dtim_pad_ms / 1000);
        /* resume latency on the RC base: hook SLEEP_EXIT decision ->
         * app running. Three sample points: right after system_sleep
         * returns (+0, may predate the RC compensation - the counter
         * is frozen in the app view until the sleepcb resume
         * compensates it), after the reason-wait loop (framework
         * resume done, cb fresh), and this report print. */
        {
            uint32_t d_a0 = pt_dtim_rc_r0 - pt_dtim_rc_exit;
            uint32_t d_cb = (uint32_t)pt_rc_after - pt_dtim_rc_exit;
            uint32_t d_rep = pmu_tmraol_get() - pt_dtim_rc_exit;
            uint8_t a0_ok = (uint32_t)(pt_dtim_rc_r0
                            - (uint32_t)pt_rc_before)
                            > 3u * (uint32_t)pt_dtim_hz;

            os_printf(KERN_INFO "  dtim: resume: exit->app0=%u%sms "
                      "exit->cb=%ums exit->report=%ums\n",
                      a0_ok ? d_a0 * 1000 / (uint32_t)pt_dtim_hz : 0,
                      a0_ok ? "" : "(frozen) ",
                      d_cb * 1000 / (uint32_t)pt_dtim_hz,
                      d_rep * 1000 / (uint32_t)pt_dtim_hz);
        }
        pt_hooks_dump();
    } else {
        os_printf(KERN_INFO "  dtim: cnt=%u exit_by=%u hz=0 (no "
                  "clock)\n", pt_dtim_cnt, pt_dtim_exit_by);
    }
}

/* ---- LED blink thread (default liveness state) ----
 * Dark while any park window is open (pt_parking), a PWRS/PWRDT session
 * runs (pt_sleeping) or an external cycle holds it (pt_led_hold - the
 * wake-capture component does, so its awake windows stay measurable). */
static volatile uint8_t pt_led_hold;

void pt_led_hold_set(uint8_t on)
{
    pt_led_hold = on;
    if (on) {
        gpio_set_val(PT_LED_PIN, 0);
    }
}

static void pt_blink_task(void *arg)
{
    uint8_t on = 0;

    (void)arg;
    for (;;) {
        os_sleep_ms(500);
        if (!pt_blink_en || pt_sleeping || pt_parking || pt_led_hold) {
            gpio_set_val(PT_LED_PIN, 0);
            continue;
        }
        on = !on;
        gpio_set_val(PT_LED_PIN, on);
    }
}

/* ---- one-shot sleep test ---- */
static uint8_t pt_req_type;
static uint32_t pt_req_ms;
static uint8_t  pt_req_dtim;   /* 1 = AT+PWRDT run: DTIM-callback-timed */

static void pt_sleep_task(void *arg)
{
    uint8_t type = pt_req_type;
    uint32_t seg_ms = pt_req_ms;
    uint8_t dtim = pt_req_dtim;
    uint32_t deadline_ms = seg_ms + (dtim ? pt_dtim_pad_ms : 0);
    int32_t sr, wok = RET_ERR;
    uint8_t r0, rres;

    (void)arg;
    pt_sleeping = 1;
    gpio_set_val(PT_LED_PIN, 0);   /* keep the LED out of the current */
    pt_cb_fresh = 0;
    pt_wake_count++;
    os_printf(KERN_INFO "PT: sleeping type=%u(%s) %ums%s...\n", type,
              pt_type_name(type), seg_ms, dtim ? " dtim-timed" : "");

    /* guide 2.2.5: RC timer is the only clock running during a sleep
     * (systick freezes; it is compensated AFTER the wake). Sample the
     * 64-bit RC counter around the park and convert the delta via the
     * calibrated rc_hz from the LMAC sleepdata region. NOTE: the RC
     * reads here are the APP-context exception documented on this
     * board (mqttka finding 2026-10-15: reading the RC counter around
     * system_sleep corrupted the park timing baseline and the wake
     * timer never fired) - if the wake stops working, remove these
     * two reads first. */
    {
        extern void *sys_sleepdata_get(uint8 id);
        struct dsleep_priv *dsleep =
            (struct dsleep_priv *)sys_sleepdata_get(SYSTEM_SLEEPDATA_ID_LMAC);

        pt_rc_before = pmu_tmrao_get();
        pt_rc_hz = dsleep ? dsleep->rc_hz : 0;
        if (dtim) {   /* clock + hook in place before entering the park */
            pt_dtim_hz = pt_rc_hz;
            pt_dtim_arm(seg_ms);
        }
    }
    /* park: DTIM-timed runs pad the RC deadline past the target (see
     * pt_dtim_pad_ms) so the only possible wake decision is the hook's
     * own SLEEP_EXIT. Dispatch returns in the entry window. */
    sr = pt_sleep_park(type, deadline_ms);

    /* ---- entry window (async gap BEFORE the physical park): the +0
     * reason read and the RC sample below run pre-park - the reads the
     * old polling loop used to mix into the wake window. Diagnostics
     * only; NO teardown here (the dtim disarm lesson: disarming in this
     * window NULLs the hooks before the park begins). */
    r0 = sys_wakeup_reason();
    if (dtim)
        pt_dtim_rc_r0 = pmu_tmraol_get();  /* may predate compensation */

    /* ---- blocks here: the task freezes with everything else, the
     * park runs, and execution resumes only after the real wake +
     * full checkpoint resume chain posted the semaphore (types 1-4;
     * the RTCC (5) wake is a reboot and never reaches this line).
     * The old 10ms-poll/200ms-window chase is gone - this IS the
     * reliable edge. Timeout = padded deadline + resume margin. */
    if (sr == RET_OK) {
        wok = pt_sleep_wait(deadline_ms + 60000);
    }

    /* post-wake: the park is really over, disarm/reason/report are
     * structurally safe (no polling race left) */
    rres = sys_wakeup_reason();
    pt_rc_after = pmu_tmrao_get();   /* post-compensation sample */
    pt_last_cb_rsn = pt_cb_rsn;
    if (dtim)
        pt_dtim_disarm();

    os_printf(KERN_INFO "PT: wake #%u type=%u sr=%d wait=%s\n"
"  rsn: entry=%u(%s)  resumed=%u(%s)  cb=%u(%s)  wkdet=0x%02x\n",
              pt_wake_count, type, (int)sr,
              sr != RET_OK ? "no-park" : (wok == RET_OK ? "ok" : "TIMEOUT"),
              r0, pt_rsn_name(r0), rres, pt_rsn_name(rres),
              pt_last_cb_rsn, pt_rsn_name(pt_last_cb_rsn),
              pt_wk_latch);
    /* guide 2.2.5 duration: RC delta / calibrated rc_hz */
    if (pt_rc_hz > 0 && pt_rc_after >= pt_rc_before) {
        uint64_t ticks = pt_rc_after - pt_rc_before;

        os_printf(KERN_INFO "  dur: rc=%ut@%dHz=%ums (req %ums, err=%+dms)\n",
                  (uint32_t)(ticks & 0xFFFFFFFF), pt_rc_hz,
                  (uint32_t)(ticks / (uint64_t)pt_rc_hz * 1000),
                  seg_ms,
                  (int32_t)(ticks / (uint64_t)pt_rc_hz * 1000)
                  - (int32_t)seg_ms);
    } else {
        os_printf(KERN_INFO "  dur: n/a (rc_hz=%d)\n", pt_rc_hz);
    }
    if (dtim)
        pt_dtim_report(seg_ms);
    pt_sleeping = 0;
}

/* ---- lowPower module table: keep the bench rails dark ---- */

static int pt_rails_suspend(void *p1, void *p2, void *p3, void *p4)
{
    extern void app_camera_rails_power(unsigned char on);
    extern void ne102_board_tf_power(unsigned char on);
    uint32_t mask[6] = {0};
    uint8_t cam_en, tf_pwr;

    (void)p1; (void)p2; (void)p3; (void)p4;
    app_camera_rails_power(0);
    ne102_board_tf_power(0);
    /* hold the enable pins low through the sleep (unheld IOs reset) */
    cam_en = MACRO_PIN(PIN_CAM_EN);
    tf_pwr = MACRO_PIN(PIN_TF_PWR_EN);
    if (cam_en != 0xFF) mask[cam_en / 16] |= BIT(cam_en % 16);
    if (tf_pwr != 0xFF) mask[tf_pwr / 16] |= BIT(tf_pwr % 16);
    dsleep_set_user_gioa(mask[0]);
    dsleep_set_user_giob(mask[1]);
    dsleep_set_user_gioc(mask[2]);
    dsleep_set_user_giod(mask[3]);
    return RET_OK;
}

/* wake: restore the pre-sleep rail state (powered - the demo's default;
 * app_camera_rails_power(1) reapplies the AVDD-first/DVDD-last order
 * and ne102_board_tf_power(1) its 10ms enable timing) */
static int pt_rails_resume(void *p1, void *p2, void *p3, void *p4)
{
    extern void app_camera_rails_power(unsigned char on);
    extern void ne102_board_tf_power(unsigned char on);

    (void)p1; (void)p2; (void)p3; (void)p4;
    app_camera_rails_power(1);
    ne102_board_tf_power(1);
    return RET_OK;
}

static const struct lowPower_module_ops pt_modules[] = {
    { "pt_rails", pt_rails_suspend, pt_rails_resume, {NULL, NULL, NULL, NULL} },
};

/* ---- hook probe (AT+PWRHK=1): register ALL dsleep hooks to record
 * call order and timing. ---- */

/* Hook-context rule (board-proven 2026-10-21): os_printf inside the
 * dsleep hooks deadlocks the sleep (the print path takes a mutex in a
 * non-thread context - the first DTIM wakeup printed once and hung).
 * So the hooks ONLY increment counters + latch timestamps; the wake
 * window (pt_sleep_task, after system_sleep returns) prints the
 * timeline. Loop is counted but not timestamped per-call (too many) -
 * only its total and first/last RC are kept. */

static volatile uint8_t pt_hooks_en;
static uint32_t pt_hk_seq;      /* total hook invocations this arm */
static uint32_t pt_hk[7];       /* per-hook counters */
enum { HK_WAKEUP, HK_ENTER, HK_EXIT, HK_RX, HK_READY, HK_LOOP, HK_SCB };
#define PT_HK_LOG_N 32

struct pt_hk_log {
    uint8_t  id;        /* enum HK_* */
    uint8_t  aux;       /* wk_reason / rx-flag / sleepcb step */
    uint32_t rc;        /* pmu_tmraol_get at the call */
};
static struct pt_hk_log pt_hk_log[PT_HK_LOG_N];
static uint8_t pt_hk_log_n;     /* entries used (saturating) */

static void pt_hk_record(uint8_t id, uint8_t aux, uint32_t rc)
{
    pt_hk_seq++;
    pt_hk[id]++;
    if (pt_hk_log_n < PT_HK_LOG_N) {
        pt_hk_log[pt_hk_log_n].id = id;
        pt_hk_log[pt_hk_log_n].aux = aux;
        pt_hk_log[pt_hk_log_n].rc = rc;
        pt_hk_log_n++;
    }
}

static int32_t pt_hk_wakeup(uint8_t wk_reason)
{
    pt_hk_record(HK_WAKEUP, wk_reason, pmu_tmraol_get());
    return DSLEEP_ACTION_DO_NOTHING;
}

static void pt_hk_enter(void)
{
    pt_hk_record(HK_ENTER, 0, pmu_tmraol_get());
}

static void pt_hk_exit(void)
{
    pt_hk_record(HK_EXIT, 0, pmu_tmraol_get());
}

static void pt_hk_rx(uint8_t *data, uint32_t len)
{
    (void)data;
    pt_hk_record(HK_RX, len ? 1 : 0, pmu_tmraol_get());
}

static void pt_hk_ready(void)
{
    pt_hk_record(HK_READY, 0, pmu_tmraol_get());
}

static int32_t pt_hk_loop(void)
{
    pt_hk[HK_LOOP]++;           /* too frequent to log; counter only */
    pt_hk_seq++;
    return 0;
}

static int32_t pt_hk_sleepcb(uint16_t type, struct sys_sleepcb_param *args,
                             void *priv)
{
    (void)type; (void)priv;
    /* sleepcb runs in the resume path on the app side AFTER the wake -
     * prints are safe here, but keep it symmetric: record only */
    pt_hk_record(HK_SCB, args->step, pmu_tmraol_get());
    if (args->action == SYS_SLEEPCB_ACTION_RESUME) {
        pt_cb_rsn = args->resume.wkreason;
        pt_cb_fresh = 1;
        os_sema_up(&pt_resume_sema);   /* probe sleepcb posts too */
    }
    return RET_OK;
}

/* wake window: dump the recorded hook timeline (task context, safe) */
static void pt_hooks_dump(void)
{
    static const char * const hk_names[] = {
        "wakeup", "enter", "exit", "rx", "ready", "loop", "sleepcb"
    };
    uint8_t i;

    os_printf(KERN_INFO "PT: hook timeline (%u logged of %u calls):\n",
              pt_hk_log_n, pt_hk_seq);
    for (i = 0; i < pt_hk_log_n; i++) {
        os_printf(KERN_INFO "  [%2u] %-8s aux=%u rc=%u\n",
                  i + 1, hk_names[pt_hk_log[i].id],
                  pt_hk_log[i].aux, pt_hk_log[i].rc);
    }
    os_printf(KERN_INFO "PT: totals w/ent/ext/rx/rdy/loop/scb="
              "%u/%u/%u/%u/%u/%u/%u\n",
              pt_hk[0], pt_hk[1], pt_hk[2], pt_hk[3],
              pt_hk[4], pt_hk[5], pt_hk[6]);
}

static void pt_hooks_set(uint8_t en)
{
    pt_hooks_en = en;
    if (en) {
        os_memset(pt_hk, 0, sizeof(pt_hk));
        os_memset(pt_hk_log, 0, sizeof(pt_hk_log));
        pt_hk_log_n = 0;
        pt_hk_seq = 0;
        dsleep_set_wakeup_hook(pt_hk_wakeup);
        dsleep_set_enter_hook(pt_hk_enter);
        dsleep_set_exit_hook(pt_hk_exit);
        dsleep_set_rx_hook(pt_hk_rx);
        dsleep_set_ready_hook(pt_hk_ready);
        dsleep_set_loop_hook(pt_hk_loop);
        sys_register_sleepcb(pt_hk_sleepcb, NULL);
        os_printf(KERN_INFO "PT: hooks ARMED (wakeup/enter/exit/rx/ready/"
                  "loop + sleepcb all-step; loop printed 1/500)\n");
    } else {
        dsleep_set_wakeup_hook(NULL);
        dsleep_set_enter_hook(NULL);
        dsleep_set_exit_hook(NULL);
        dsleep_set_rx_hook(NULL);
        dsleep_set_ready_hook(NULL);
        dsleep_set_loop_hook(NULL);
        sys_register_sleepcb(pt_sleepcb, NULL);  /* back to the quiet one */
        os_printf(KERN_INFO "PT: hooks disarmed\n");
    }
}

/* ---- AT handlers ---- */

static int32_t pt_atcmd(const char *cmd, char *argv[], uint32_t argc)
{
    struct netif *nif = netif_find("w0");

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("blink=%u sleeping=%u wakes=%u ip=%s",
                   pt_blink_en, pt_sleeping, pt_wake_count,
                   (nif && nif->ip_addr.addr) ? ipaddr_ntoa(&nif->ip_addr)
                                              : "none");
        atcmd_resp("last cb rsn=%u(%s)", pt_last_cb_rsn,
                   pt_rsn_name(pt_last_cb_rsn));
        return 0;
    }
    atcmd_resp("usage: AT+PWR? | AT+PWRS=<type 1..5>,<secs> | "
"AT+PWRDT=<secs> | AT+PWCAP=<mode>,<secs> | AT+PWRLED/PWRDBG/PWRHK=<0|1>");
    return 0;
}

static int32_t pt_atcmd_sleep(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t type, secs;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("sleeping=%u req: type=%u %ums", pt_sleeping,
                   pt_req_type, pt_req_ms);
        return 0;
    }
    if (argc < 2 || !argv[0][0] || !argv[1][0]) {
        atcmd_resp("usage: AT+PWRS=<type 1..5>,<secs 5..3600>  "
"(1=SRAM_WIFI 2=WIFI_DSLEEP 3=SRAM_ONLY 4=LIGHT "
                   "5=RTCC-reboot)");
        return -1;
    }
    type = os_atoi(argv[0]);
    secs = os_atoi(argv[1]);
    if (type < SYSTEM_SLEEP_TYPE_SRAM_WIFI || type >= SYSTEM_SLEEP_TYPE_MAX
        || secs < 5 || secs > 3600) {
        atcmd_resp("ERR: type 1..5, secs 5..3600");
        return -1;
    }
    if (pt_sleeping) {
        atcmd_resp("ERR: test already running");
        return -1;
    }
    {
        extern uint8_t wake_capture_busy(void);

        if (wake_capture_busy()) {
            atcmd_resp("ERR: PWCAP cycle running");
            return -1;
        }
    }
    if (type == SYSTEM_SLEEP_TYPE_SRAM_WIFI) {
        struct netif *nif = netif_find("w0");

        if (!nif || !nif->ip_addr.addr) {
            /* warn but proceed: the park still runs, it just cannot
             * maintain an association (and tends to return early) */
            atcmd_resp("WARN: no IP - SRAM_WIFI expects an association "
"(AT+SSID/AT+KEY first)");
        }
    }
    pt_req_type = (uint8_t)type;
    pt_req_ms = (uint32_t)secs * 1000;
    pt_req_dtim = 0;
    os_task_create("pt_sleep", pt_sleep_task, NULL,
                   OS_TASK_PRIORITY_NORMAL, 0, NULL, 4096);
    atcmd_resp("OK: type=%u(%s) %us%s", type, pt_type_name(type), secs,
               type == SYSTEM_SLEEP_TYPE_RTCC
                   ? " (wake=REBOOT, result prints at next boot)"
                   : "");
    return 0;
}

/* AT+PWRDT=<secs> - DTIM-callback-timed wake test (SRAM_WIFI only):
 * park with the RC sleep deadline padded +600s beyond the target so
 * it can never fire; the DTIM wakeup_hook itself counts wake windows,
 * judges time-up on the RC clock and returns SLEEP_EXIT. Not
 * persisted; refuses to run while the hook probe owns the wakeup
 * hook (AT+PWRHK). */
static int32_t pt_atcmd_dtim(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t secs, pad = 10;
    struct netif *nif;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("armed=%u cnt=%u calls=%u exit_by=%u target=%ums "
                   "pad=%ums", pt_dtim_armed, pt_dtim_cnt, pt_dtim_calls,
                   pt_dtim_exit_by, pt_dtim_target_ms, pt_dtim_pad_ms);
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+PWRDT=<secs 5..3600>[,<pad_secs 0..3600>] "
                   "(DTIM-callback-timed wake, SRAM_WIFI only; pad keeps "
                   "the RC deadline past the target, default 10)");
        return -1;
    }
    secs = os_atoi(argv[0]);
    if (argc >= 2 && argv[1][0])
        pad = os_atoi(argv[1]);
    if (secs < 5 || secs > 3600 || pad < 0 || pad > 3600) {
        atcmd_resp("ERR: secs 5..3600, pad 0..3600");
        return -1;
    }
    if (pt_sleeping) {
        atcmd_resp("ERR: test already running");
        return -1;
    }
    {
        extern uint8_t wake_capture_busy(void);

        if (wake_capture_busy()) {
            atcmd_resp("ERR: PWCAP cycle running");
            return -1;
        }
    }
    if (pt_hooks_en) {
        atcmd_resp("ERR: AT+PWRHK=0 first (probe owns the wakeup hook)");
        return -1;
    }
    nif = netif_find("w0");
    if (!nif || !nif->ip_addr.addr) {
        /* hard requirement here (unlike PWRS): without an association
         * there are no DTIM windows and the hook would never fire */
        atcmd_resp("ERR: no IP - DTIM windows need the association "
                   "(AT+SSID/AT+KEY first)");
        return -1;
    }
    pt_req_type = SYSTEM_SLEEP_TYPE_SRAM_WIFI;
    pt_req_ms = (uint32_t)secs * 1000;
    pt_req_dtim = 1;
    pt_dtim_pad_ms = (uint32_t)pad * 1000;
    /* arm via the probe path (full hook set + recording sleepcb -
     * its other slots keep recording for the timeline dump), then
     * put our time judge into the wakeup slot: the
     * DSLEEP_ACTION_SLEEP_EXIT it returns IS the mechanism this
     * command exists to test */
    pt_hooks_set(1);
    dsleep_set_wakeup_hook(pt_dtim_wakeup);
    os_task_create("pt_sleep", pt_sleep_task, NULL,
                   OS_TASK_PRIORITY_NORMAL, 0, NULL, 4096);
    atcmd_resp("OK: DTIM-timed %us pad+%us (SRAM_WIFI, rc deadline "
               "=%ums)", secs, pad, (uint32_t)secs * 1000
               + pt_dtim_pad_ms);
    return 0;
}

static int32_t pt_atcmd_led(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t v;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("blink=%u", pt_blink_en);
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+PWRLED=<0|1>");
        return -1;
    }
    v = os_atoi(argv[0]);
    pt_blink_en = v ? 1 : 0;
    atcmd_resp("OK: blink %s", pt_blink_en ? "on" : "off");
    return 0;
}

/* shared guard for commands that touch dsleep library state (dbg cfg,
 * hook registration): a sleep test may be inside the ~120ms async ENTRY
 * WINDOW (RPC dispatched, park not yet frozen) where the AT thread still
 * runs - an ioctl landing there corrupts the starting park program. The
 * third board case of this family: AT+PWRDBG=0 fired during a test's
 * entry window reset the chip via LVD (srp_lvd_vcc_vol_low, 2026-10-22).
 * See also the dtim disarm lesson and mqttka's PMU RC rule. */
static uint8_t pt_dsleep_reconfig_blocked(void)
{
    extern uint8_t wake_capture_busy(void);

    if (pt_sleeping || wake_capture_busy()) {
        atcmd_printf("ERR: a sleep test is running - dsleep reconfig "
                     "inside the async entry window corrupts the park "
                     "(LVD reset finding 2026-10-22); retry when "
                     "idle\r\n");
        return 1;
    }
    return 0;
}

/* AT+PWRDBG=<0|1> - dsleep library debug prints (libdsleep.a: the
 * per-DTIM "Bingo" RC-calibration lines, BKTO/null_tx/rc_hz err etc.).
 * 1 = DSLEEP_DBG_EN_PRINT, 0 = silent. NOT persisted: a fresh boot is
 * always silent unless explicitly enabled. */
static int32_t pt_atcmd_dbg(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t v;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("dsleep dbg=0x%x", dsleep_get_dbg_cfg());
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+PWRDBG=<0|1> (dsleep lib prints: Bingo/"
"BKTO/null_tx)");
        return -1;
    }
    if (pt_dsleep_reconfig_blocked()) {
        return -1;
    }
    v = os_atoi(argv[0]);
    dsleep_set_dbg_cfg(v ? DSLEEP_DBG_EN_PRINT : 0);
    atcmd_resp("OK: dsleep dbg %s", v ? "ON" : "off");
    return 0;
}

/* AT+PWRHK=<0|1> - arm/disarm the dsleep hook probe (see pt_hooks_set).
 * Not persisted; a fresh boot starts disarmed (quiet sleep tests). */
static int32_t pt_atcmd_hk(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t v;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("hooks=%u seq=%u w/ent/ext/rx/rdy/loop/scb="
"%u/%u/%u/%u/%u/%u/%u",
                   pt_hooks_en, pt_hk_seq,
                   pt_hk[0], pt_hk[1], pt_hk[2], pt_hk[3],
                   pt_hk[4], pt_hk[5], pt_hk[6]);
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+PWRHK=<0|1> (all dsleep hooks + prints; "
"loop throttled 1/500)");
        return -1;
    }
    if (pt_dsleep_reconfig_blocked()) {
        return -1;
    }
    v = os_atoi(argv[0]);
    pt_hooks_set(v ? 1 : 0);
    atcmd_resp("OK: hooks %s (run AT+PWRS to observe)", v ? "ARMED" : "off");
    return 0;
}

/* AT+PWRLOG=<level 0..7> - runtime print-level knob (print_level() +
 * CoreSetting->print_level, the mqttka pattern). The default level
 * hides the SDK's KERN_DEBUG traces (e.g. the sdhost card-probe
 * sequence) - bump to 6/7 to see them when diagnosing storage issues.
 * Not persisted. */
static int32_t pt_atcmd_log(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t v;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("print level=%d", (int32_t)CoreSetting->print_level);
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+PWRLOG=<0..7> (default hides KERN_DEBUG; "
                   "6/7 expose SDK traces like the sdhost probe)");
        return -1;
    }
    v = os_atoi(argv[0]);
    if (v < 0 || v > 7) {
        atcmd_resp("ERR: 0..7");
        return -1;
    }
    print_level(v);
    CoreSetting->print_level = v;
    atcmd_resp("OK: print level=%d", v);
    return 0;
}

/* AT+PWDWK=<0|1> - arm/disarm the wake-on-unicast-data filter for the
 * keepalive parks (see pt_wkdet): 1 = any UDP to our IP, or TCP with
 * payload, wakes the parked device; bare TCP ACKs stay ignored. Not
 * persisted; requires an association at arm time. */
static int32_t pt_atcmd_wkdet(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t v;
    struct netif *nif;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        nif = netif_find("w0");
        atcmd_resp("wkdet=%u ip=%s latch=0x%02x", pt_wkdet_en,
                   (nif && nif->ip_addr.addr) ? ipaddr_ntoa(&nif->ip_addr)
                                               : "none",
                   pt_wk_latch);
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+PWDWK=<0|1> (wake parked device on UDP-to-us"
                   " or TCP-with-payload-to-us; bare ACKs ignored)");
        return -1;
    }
    v = os_atoi(argv[0]);
    if (pt_dsleep_reconfig_blocked()) {
        return -1;
    }
    if (v) {
        nif = netif_find("w0");
        if (!nif || !nif->ip_addr.addr) {
            atcmd_resp("ERR: no IP - the filter needs our unicast address "
                       "(AT+SSID/AT+KEY first)");
            return -1;
        }
        pt_wk_ip = nif->ip_addr.addr;
        pt_wk_latch = 0xFF;
        dsleep_set_usr_wkdet_cb(NULL, pt_wkdet);
        pt_wkdet_en = 1;
        atcmd_resp("OK: wkdet ARMED ip=%s (UDP-to-us / TCP-payload wake; "
                   "run AT+PWRS/PWCAP, then poke from the bench)",
                   ipaddr_ntoa(&nif->ip_addr));
    } else {
        pt_wkdet_en = 0;
        pt_wk_ip = 0;
        dsleep_set_usr_wkdet_cb(NULL, NULL);
        atcmd_resp("OK: wkdet off");
    }
    return 0;
}

/* ---- init (called from app_lowpwr_camera_init) ---- */

int app_power_test_init(void)
{
    /* boot-time wake reason: RTCC/WDT/MCLR-style sleeps REBOOT the chip,
     * so their reason never reaches the (never-resuming) sleep task -
     * it is only readable here, at boot */
    {
        uint8_t r = sys_wakeup_reason();

        os_printf(KERN_INFO "PT: boot wake reason=%u(%s)\n", r,
                  pt_rsn_name(r));
    }

    /* rails keep their board-init state (powered) while awake; the
     * module table below cuts them on every suspend and the resume
     * callback restores them after wake */
    /* sleep framework prerequisite (registers the standard APP
     * suspend/resume chain; without it SRAM-family parks misbehave) */
    lowpower_app_init(pt_modules, ARRAY_SIZE(pt_modules));
    sys_register_sleepcb(pt_sleepcb, NULL);

    /* synchronous-sleep wrapper backend */
    os_sema_init(&pt_resume_sema, 0);

    /* wake-capture cycle test (custom/component/wake_capture): its AT
     * commands register here; the PSRAM JPEG stash MUST be reserved
     * before any capture_init carves the 7.5MB AV pool - the general
     * PSRAM heap is only guaranteed fresh this early (capture.c board
     * finding 2026-10-16) */
    {
        extern void ne102_capture_stash_reserve(uint32_t bytes);
        extern int wake_capture_init(void);

        ne102_capture_stash_reserve(512 * 1024);
        wake_capture_init();
    }

    /* default print level = INFO (mqttka-proven setting): hides the
     * SDK's KERN_DEBUG single-letter progress markers (jpg_v3_msi.c
     * prints "JD" per encode chunk, vpp prints "V"/"F" - interleaved
     * they appear as "FFJDV+..." console garbage, board finding
     * 2026-10-23). AT+PWRLOG=6/7 re-enables the traces when needed. */
    print_level(6);
    CoreSetting->print_level = 6;

    /* LED direction/value already configured by app_board_power_init */
    os_task_create("pt_blink", pt_blink_task, NULL,
                   OS_TASK_PRIORITY_NORMAL, 0, NULL, 1024);

    atcmd_register("AT+PWR", pt_atcmd);
    atcmd_register("AT+PWRS", pt_atcmd_sleep);
    atcmd_register("AT+PWRDT", pt_atcmd_dtim);
    atcmd_register("AT+PWRLED", pt_atcmd_led);
    atcmd_register("AT+PWRDBG", pt_atcmd_dbg);
    atcmd_register("AT+PWRHK", pt_atcmd_hk);
    atcmd_register("AT+PWRLOG", pt_atcmd_log);
    atcmd_register("AT+PWDWK", pt_atcmd_wkdet);
    os_printf(KERN_INFO "PT: power test demo ready. AT+PWR? for status, "
"AT+PWRS=<type 1..5>,<secs> to sleep (synchronous), AT+PWRDT=<secs> "
"DTIM-timed wake, AT+PWCAP=<mode>,<secs> wake-capture cycle, "
"AT+PWRLED/PWRDBG/PWRHK=<0|1>\n");
    return RET_OK;
}
