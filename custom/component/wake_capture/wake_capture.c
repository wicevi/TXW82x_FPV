/*
 * wake_capture.c - capture-on-wake cycle test (power test demo add-on).
 *
 * TWO wake paths, one cycle shape (park -> wake -> capture -> camera
 * down -> persist -> re-sleep -> idle wake):
 *
 *   SRAM_WIFI path (default): AT+PWCAP=...,1  - checkpoint park, the
 *     task RESUMES after the wake (remote-data packets during the park
 *     wake early - the product scenario).
 *
 *   RTCC path (reboot / cold boot): AT+PWCAP=...,5 - the park wakes by
 *     REBOOT, so the capture intent is stored in the syscfg KV (flash)
 *     BEFORE parking; the NEXT boot consumes it and runs the same
 *     capture+persist sequence as a COLD-START capture (boot -> JPEG
 *     timing via ne102_boot_elapsed_ms), then re-sleeps RTCC; the boot
 *     after that is idle (intent was one-shot). AT+PWCAP=0 disarms a
 *     pending intent. The reboot wipes wc.r - the boot-cycle report
 *     lives on the serial log only.
 *
 * Cycle body (both paths):
 *   park#1 -> wake -> capture 1 JPEG (AE skip), consumer stash_only=1:
 *     the frame lands in the PSRAM capture stash, no SD traffic
 *   -> ne102_capture_deinit(): sensor rails + pipeline DOWN - the
 *     camera domain is powered only for the capture itself (user
 *     requirement: stash the JPEG to memory, then release camera)
 *   -> persist the stash per mode: SD file (mount / write / read-back
 *     CRC verify / unmount) and/or HTTP POST (wc_http; the RTCC boot
 *     path waits bounded for the post-boot IP first), both with the
 *     camera already dark; every leg is failure-tolerant: log and
 *     continue, the cycle must reach its re-sleep regardless
 *   -> park#2 = the "immediate re-sleep"; the capture intent was
 *     one-shot, so wake#2 (SRAM resume or next RTCC boot) is IDLE
 *   -> report (AT+PWCAP? - SRAM path only, RAM is lost on reboots)
 *
 * Kept out of app_power_test.c on purpose (file-size discipline); the
 * blocking sleep comes from its pt_sleep_sync()/pt_sleep_park(). The
 * worker is a resident task woken by a semaphore (mqttka pattern).
 *
 * AT commands (RTCC intent is persisted; the rest is not):
 *   AT+PWCAP=<mode 0..3>,<secs 5..3600>[,<skip 0..10>[,<type 1|5>]]
 *       mode: 0=disarm pending RTCC intent  1=SD  2=HTTP  3=both;
 *       type: 1=SRAM_WIFI resume path (default)  5=RTCC reboot path
 *   AT+PWCAP?            last report / config / RTCC intent state
 *   AT+PWCURL=<url>      upload target, "http://host[:port]/path"
 */
#include <basic_include.h>
#include <osal/sleep.h>
#include <osal/semaphore.h>
#include <lib/atcmd/libatcmd.h>
#include <lib/lmac/lmac_dsleep.h>
#include <lib/fs/fatfs/osal_file.h>
#include <fdb_syscfg.h>
#include <devid.h>
#include <dev.h>
#include <lwip/netif.h>
#include "../../app/app_lowpwr_camera.h"
#include "../../app/app_power_test/app_power_test.h"
#include "../capture/capture.h"
#include "wc_http.h"
#include "wake_capture.h"

#define WC_URL_MAX    96
#define WC_TASK_STACK 16384   /* capture pipeline + fatfs + sockets on one
                               * task; mqttka proved this size class */
#define WC_SHOT_MS    12000   /* init+skip+shot budget (mqttka envelope) */
#define WC_RESUME_MARGIN_MS 60000  /* wait past the RC deadline before
                                    * declaring a missed resume */
#define WC_IP_WAIT_MS 15000   /* RTCC boot path: bounded wait for the
                               * post-boot IP before the HTTP leg */

enum { WC_MODE_SD = 1, WC_MODE_HTTP = 2 };

/* ---- RTCC-path intent (syscfg KV, survives the reboot wake) ---- */

#define WC_KV_KEY       "ne102_pwc"
#define WC_INTENT_MAGIC 0x50574331u   /* "PWC1" */

struct wc_intent {
    uint32_t magic;
    uint8_t  armed;      /* 1 = capture at the next boot */
    uint8_t  mode;       /* WC_MODE_* mask */
    uint8_t  skip;
    uint8_t  rsvd;
    uint32_t secs;       /* re-sleep window */
    char     url[WC_URL_MAX];
};

/* task request routing (set by the AT handler / boot check) */
enum { WC_REQ_CYCLE = 1, WC_REQ_RTCC_ARM, WC_REQ_BOOT };

/* last-cycle report (AT+PWCAP?) - zeroed at every cycle start; lost
 * with the RAM on RTCC reboots (serial log is the record there) */
struct wc_report {
    int32_t  park1, park2;    /* pt_sleep_sync returns */
    uint8_t  wake1, wake2;    /* live sleepcb reasons */
    int32_t  cap_rc;
    uint32_t cap_ms, jpg_len;
    char     path[40];
    int32_t  sd_rc;           /* RET_OK / RET_ERR (incl. verify) */
    uint32_t sd_ms;           /* whole SD leg: mount..unmount */
    int32_t  http_status;     /* >=100 = HTTP answer, <0 = transport err */
    uint32_t http_ms;
    uint32_t ip_wait_ms;      /* RTCC boot path only */
    uint32_t total_ms;        /* jiffies basis: parks frozen, awake only */
};

static struct {
    /* cycle request (set by the AT handler) */
    uint8_t  mode;
    uint32_t secs;
    uint8_t  skip;
    uint8_t  sleep_type;  /* park type: 1=SRAM_WIFI (assoc kept, data
                           * wake possible) 3=SRAM_ONLY (NO wifi keepalive
                           * duty - the SD-only low-power scenario; assoc
                           * is not maintained across parks) */
    char     url[WC_URL_MAX];
    /* runtime */
    volatile uint8_t busy;
    volatile uint8_t req;     /* enum WC_REQ_* */
    volatile uint32_t hb_ms;  /* cycle heartbeat (ne102_boot_elapsed_ms at
                               * the last leg boundary - a stale value while
                               * busy=1 means the task is hung inside that
                               * leg; AT+PWCAP? prints its age) */
    uint16_t file_seq;
    struct wc_report r;
} wc = {
    .url = "http://192.168.110.50:8080/upload",
    .skip = 3,
    .sleep_type = SYSTEM_SLEEP_TYPE_SRAM_WIFI,
};

static os_semaphore_t wc_start_sema;

/* breadcrumb: leg boundary marker + heartbeat refresh (keep prints
 * single-line short; hung-task localization, board case 2026-10-23:
 * a PWCAP=2 cycle stuck after "camera dark" with no further output and
 * busy never clearing - these markers will bracket the culprit) */
#define WC_LEG(tag)  do { \
    wc.hb_ms = ne102_boot_elapsed_ms(); \
    os_printf(KERN_INFO "WC: [%s] +%ums\n", tag, wc.hb_ms); \
} while (0)

uint8_t wake_capture_busy(void)
{
    return wc.busy;
}

static const char *wc_mode_name(uint8_t m)
{
    switch (m) {
    case WC_MODE_SD:                    return "sd";
    case WC_MODE_HTTP:                  return "http";
    case WC_MODE_SD | WC_MODE_HTTP:     return "sd+http";
    default:                            return "?";
    }
}

/* ---- SD persistence (mounted per cycle, unmounted before park#2: the
 * suspend chain right after cuts the TF rail - a live mount must never
 * meet a power cut; battery-demo sd_suspend pattern) ---- */

static uint32_t wc_crc_tab[256];
static uint8_t  wc_crc_tab_init;

static void wc_crc_tab_init_once(void)
{
    uint32_t i, k, c;

    if (wc_crc_tab_init) {
        return;
    }
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        wc_crc_tab[i] = c;
    }
    wc_crc_tab_init = 1;
}

/* one-char wake-source tag for the file name (same reason families as
 * pt_rsn_name): T=timer family, D=remote data, I=IO/PIR, L=link lost,
 * X=other/unknown */
static char wc_wake_tag(uint8_t r)
{
    switch (r) {
    case DSLEEP_WK_REASON_STA_DATA:
    case DSLEEP_WK_REASON_WK_DATA:
        return 'D';
    case DSLEEP_WK_REASON_TIMER:
    case DSLEEP_WK_REASON_TIM:
    case DSLEEP_WK_REASON_BC_TIM:
    case DSLEEP_WK_REASON_RTC_ALARM:
        return 'T';
    case DSLEEP_WK_REASON_IO0:
    case DSLEEP_WK_REASON_IO1:
    case DSLEEP_WK_REASON_IO2:
    case DSLEEP_WK_REASON_IO3:
    case DSLEEP_WK_REASON_IO4:
    case DSLEEP_WK_REASON_IO5:
    case DSLEEP_WK_REASON_PIR:
        return 'I';
    case DSLEEP_WK_REASON_BEACON_LOST:
    case DSLEEP_WK_REASON_AP_ERROR:
    case DSLEEP_WK_REASON_HB_TIMEOUT:
    case DSLEEP_WK_REASON_PS_DISCONNECT:
    case DSLEEP_WK_REASON_STA_ERROR:
    case DSLEEP_WK_REASON_SLEEPED_STA_ERROR:
    case DSLEEP_WK_REASON_WEAK_SIGNAL:
        return 'L';
    default:
        return 'X';
    }
}

/* Write the stash to 0:/DCIM/WK_<wake><skip>_<seq>.JPG, fsync, then
 * read the file back and CRC-compare it against the source (storage
 * integrity proof for a bench that never pulls the card). */
static int32_t wc_sd_save(const uint8_t *buf, uint32_t len, char *path,
                          size_t psz)
{
    extern void fatfs_sd0_init(void);
    extern void sys_mount_device(uint16 dev_id, uint16 dev_type, uint8 umount);
    extern void sdhost_deinit_for_sleep(void);
    static uint8_t vbuf[4096];   /* static: keep the task stack lean */
    F_FILE *fp;
    uint32_t want_crc, crc, got_len, n, i;
    int32_t rc = RET_ERR;

    wc_crc_tab_init_once();
    want_crc = 0xFFFFFFFF;
    for (i = 0; i < len; i++) {
        want_crc = wc_crc_tab[(want_crc ^ buf[i]) & 0xFF] ^ (want_crc >> 8);
    }
    want_crc ^= 0xFFFFFFFF;

    /* vfs_fatfs_register() is what teaches the vfs layer the "fatfs"
     * type - app_sd_init() does it at boot in the demos, but this
     * cycle calls fatfs_sd0_init() directly, so register once per boot
     * here or sys_mount_device() fails with -19 before even reaching
     * f_mount (board finding 2026-10-22: card probes fine - SD v2,
     * OCR/SCR ok - yet "sd0 mount to /sd0 fail! ret=-19"). */
    {
        extern int vfs_fatfs_register(void);
        static uint8_t vfs_up;

        if (!vfs_up) {
            vfs_fatfs_register();
            vfs_up = 1;
        }
    }
    fatfs_sd0_init();   /* registers drive 0 + mounts; cycle-safe (the
                         * battery demo calls it on every resume) */
    /* overwrite protection: advance past any existing file of the same
     * name (the RAM counter resets per boot - without this probe the
         * "w+" open would truncate a previous session's photos; user
     * finding 2026-10-23). Name carries the wake-source tag and the
         * skip count: WK_<T|D|I|L|X><skip>_<seq>.JPG. Bounded: gives
     * up after 5000 collisions. */
    {
        uint32_t guard = 5000;

        do {
            wc.file_seq++;
            os_snprintf(path, psz, "0:/DCIM/WK_%c%u_%03u.JPG",
                        wc_wake_tag(wc.r.wake1), (uint32_t)wc.skip,
                        (uint32_t)wc.file_seq);
        } while (osal_fexist(path) == FR_OK && --guard);
        if (!guard) {
            os_printf(KERN_ERR "WC: no free WK_*.JPG slot - giving up\n");
            goto out;
        }
    }
    fp = osal_fopen_auto(path, "w+", 0);   /* auto-creates 0:/DCIM */
    if (!fp) {
        os_printf(KERN_ERR "WC: sd open %s err=%d (no card?)\n", path,
                  get_errno());
        goto out;
    }
    /* osal_fwrite returns the BYTE count written (sdfunc_work checks it
     * as truthy), not the nmemb count - a "!= 1" check misfired here
     * and failed every write with err=0 */
    if (osal_fwrite((void *)buf, len, 1, fp) < len) {
        os_printf(KERN_ERR "WC: sd write err=%d\n", get_errno());
        osal_fclose(fp);
        goto out;
    }
    osal_fsync(fp);
    osal_fclose(fp);

    /* read-back verify: file bytes must length+CRC-match the source */
    fp = osal_fopen(path, "r");
    if (!fp) {
        os_printf(KERN_ERR "WC: sd reopen err=%d\n", get_errno());
        goto out;
    }
    crc = 0xFFFFFFFF;
    got_len = 0;
    while ((n = osal_fread(vbuf, 1, sizeof(vbuf), fp)) > 0) {
        for (i = 0; i < n; i++) {
            crc = wc_crc_tab[(crc ^ vbuf[i]) & 0xFF] ^ (crc >> 8);
        }
        got_len += n;
    }
    osal_fclose(fp);
    crc ^= 0xFFFFFFFF;
    if (got_len != len || crc != want_crc) {
        os_printf(KERN_ERR "WC: sd verify FAIL len %u/%u crc %08x/%08x\n",
                  got_len, len, crc, want_crc);
        goto out;
    }
    rc = RET_OK;
    os_printf(KERN_INFO "WC: sd saved %s (%uB crc ok)\n", path, len);
out:
    /* unmount + sdhost down BEFORE the rail cut rides park#2's
     * suspend chain */
    sys_mount_device(HG_SD0_DEVID, DEV_TYPE_SD, 1);
    sdhost_deinit_for_sleep();
    return rc;
}

/* ---- HTTP leg hard watchdog (board case 2026-10-23) ----
 * A PWCAP=2 cycle wedged forever after "camera dark" (user finding:
 * suspected inside the network upload; no "WC: http" line ever
 * printed, busy stayed 1 until a reset). Every step of wc_http_post
 * has an SO_*TIMEO, but lwip's global core lock / DNS edge paths can
 * outlive socket-level timeouts. So the POST now runs on a dedicated
 * worker and the cycle waits BOUNDED: a wedged upload is abandoned
 * (-100), the cycle still reaches its re-sleep (the red line), and
 * the leaked worker+socket is the visible bench cost. */
#define WC_HTTP_GUARD_MS     30000  /* SRAM path (upload ~0.1-1s) */
#define WC_HTTP_GUARD_BOOT_MS 45000 /* RTCC boot path (+15s ip wait) */

static struct {
    const uint8_t *body;
    uint32_t len;
    uint8_t wait_ip;              /* RTCC boot path: bounded IP wait */
    struct wc_http_result hr;
} wc_http_job;
static os_semaphore_t wc_http_sema;
static os_semaphore_t wc_http_done_sema;   /* posted on completion -
                                            * event-driven wake, the
                                            * happy path adds ~0ms vs
                                            * the old in-place call */
static volatile uint8_t wc_http_done;

static void wc_http_worker(void *arg)
{
    for (;;) {
        os_sema_down(&wc_http_sema, osWaitForever);
        if (wc_http_job.wait_ip) {
            struct netif *nif;
            uint32_t tw = 0;

            /* readiness = IP AND netmask (the RTCC cold-boot race,
             * board finding 2026-10-23: polling only ip_addr fires
             * mid-DHCP-bind while mask/gw are still being installed -
             * the connect then runs on a half-configured interface and
             * aborts, soerr=103 ECONNABORTED; the keepalive path never
             * sees this because its netif was bound long before). No
             * settle delay on purpose - user ruling: never add hard
             * delays to the capture path; the connect retry in
             * wc_http_post covers the residual window. */
            while (tw < WC_IP_WAIT_MS) {
                nif = netif_find("w0");
                if (nif && nif->ip_addr.addr && nif->netmask.addr) {
                    break;
                }
                os_sleep_ms(200);
                tw += 200;
            }
            wc.r.ip_wait_ms = tw;
            if (tw) {
                nif = netif_find("w0");
                os_printf(KERN_INFO "WC: ip wait %ums (%s)\n", tw,
                          (nif && nif->ip_addr.addr) ? "up" : "TIMEOUT");
            }
        }
        wc.r.http_status = wc_http_post(wc.url, wc_http_job.body,
                                        wc_http_job.len, &wc_http_job.hr);
        wc_http_done = 1;
        os_sema_up(&wc_http_done_sema);
    }
}

/* ---- capture + persist (shared by both wake paths; camera goes dark
 * before any storage/network IO; fills wc.r) ---- */

static void wc_capture_persist(uint8_t wait_ip)
{
    uint8_t *jpg = NULL;
    uint32_t len = 0;
    uint32_t tc0, tc1;

    /* capture: rails up -> pipeline -> 1 shot (AE skip) -> PSRAM stash
     * -> rails DOWN; no storage inside the camera window */
    tc0 = ne102_boot_elapsed_ms();
    {
        struct ne102_capture_cfg cc = {
            .skip_frames = wc.skip,
            .burst = 1,
            .stash_only = 1,
        };

        ne102_capture_set_cfg(&cc);
        wc.r.cap_rc = ne102_capture_init();
        if (wc.r.cap_rc == RET_OK) {
            wc.r.cap_rc = ne102_capture_shot(1);
        }
        if (wc.r.cap_rc == RET_OK) {
            wc.r.cap_rc = ne102_capture_wait_done(WC_SHOT_MS);
        }
        ne102_capture_get_last_jpg(&jpg, &len);
        ne102_capture_deinit();   /* stash survives the deinit by design */
    }
    tc1 = ne102_boot_elapsed_ms();
    wc.r.cap_ms = tc1 - tc0;
    wc.r.jpg_len = len;
    os_printf(KERN_INFO "WC: capture rc=%d %uB in %ums - camera dark\n",
              wc.r.cap_rc, len, wc.r.cap_ms);

    /* persist from the PSRAM stash, camera already powered down.
     * cap_rc gate: the stash survives cycles, so a failed shot would
     * otherwise persist the PREVIOUS frame and report success */
    if (wc.r.cap_rc == RET_OK && (wc.mode & WC_MODE_SD) && jpg && len) {
        uint32_t t0 = ne102_boot_elapsed_ms();

        WC_LEG("sd");
        wc.r.sd_rc = wc_sd_save(jpg, len, wc.r.path, sizeof(wc.r.path));
        wc.r.sd_ms = ne102_boot_elapsed_ms() - t0;
        os_printf(KERN_INFO "WC: sd %s in %ums (mount..verify..unmount)\n",
                  wc.r.sd_rc == RET_OK ? "ok" : "FAILED", wc.r.sd_ms);
    }
    if (wc.r.cap_rc == RET_OK && (wc.mode & WC_MODE_HTTP) && jpg && len) {
        uint32_t t0 = ne102_boot_elapsed_ms();
        uint32_t guard = wait_ip ? WC_HTTP_GUARD_BOOT_MS
                                 : WC_HTTP_GUARD_MS;
        uint32_t waited = 0;

        /* bounded upload: worker task does the POST, we wait at most
         * `guard` - a wedged network stack can no longer stall the
         * cycle past its re-sleep red line */
        WC_LEG("http");
        wc_http_job.body = jpg;
        wc_http_job.len = len;
        wc_http_job.wait_ip = wait_ip;
        os_memset(&wc_http_job.hr, 0, sizeof(wc_http_job.hr));
        wc.r.http_status = 0;
        wc.r.ip_wait_ms = 0;
        wc_http_done = 0;
        os_sema_eat(&wc_http_sema);
        os_sema_eat(&wc_http_done_sema);
        os_sema_up(&wc_http_sema);
        while (!wc_http_done && waited < guard) {
            uint32_t step = guard - waited > 200 ? 200 : guard - waited;

            /* event-driven: returns the moment the worker posts */
            if (os_sema_down(&wc_http_done_sema, (int32_t)step) > 0) {
                break;
            }
            waited += step;
            wc.hb_ms = ne102_boot_elapsed_ms();  /* heartbeat while waiting */
        }
        if (!wc_http_done) {
            wc.r.http_status = -100;   /* guard abandoned, still wedged */
            os_printf(KERN_ERR "WC: http guard %ums exceeded - upload "
                      "abandoned (worker wedged), cycle continues\n",
                      guard);
        }
        wc.r.http_ms = ne102_boot_elapsed_ms() - t0;
        os_printf(KERN_INFO "WC: http status=%d sent=%uB in %ums%s\n",
                  wc.r.http_status, wc_http_job.hr.bytes_sent,
                  wc.r.http_ms, wc_http_done ? "" : " (HUNG-abandoned)");
    }
}

/* ---- SRAM_WIFI resume path: park#1 -> capture -> park#2 ---- */

static void wc_cycle(void)
{
    uint32_t t0, timeout = wc.secs * 1000 + WC_RESUME_MARGIN_MS;
    SYSTEM_SLEEP_TYPE ptype = (SYSTEM_SLEEP_TYPE)wc.sleep_type;
    int32_t sr;

    wc.busy = 1;
    os_sema_eat(&wc_start_sema);   /* a double-fired AT would queue a
                                    * phantom second cycle otherwise */
    pt_led_hold_set(1);   /* whole cycle LED-dark (current measurement) */
    t0 = ne102_boot_elapsed_ms();
    os_memset(&wc.r, 0, sizeof(wc.r));
    wc.r.sd_rc = RET_ERR;          /* distinguish "not attempted" (-1)
                                    * from RET_OK in the report */
    os_printf(KERN_INFO "WC: cycle mode=%u(%s) secs=%us skip=%u park=%u "
              "url=%s\n",
              wc.mode, wc_mode_name(wc.mode), wc.secs, wc.skip,
              wc.sleep_type, wc.url);
    WC_LEG("park1");

    /* park #1: block until a REAL wake (the semaphore IS the sleepcb
     * RESUME edge). RC timer is the backstop; with SRAM_WIFI a remote
     * packet in the window wakes early - the product capture scenario
     * (SRAM_ONLY parks have no association to data-wake on) */
    sr = pt_sleep_sync(ptype, wc.secs * 1000, timeout);
    wc.r.park1 = sr;
    if (sr != RET_OK) {
        os_printf(KERN_ERR "WC: park#1 sr=%d - cycle aborted\n", sr);
        goto done;
    }
    wc.r.wake1 = pt_sleep_wake_reason();
    os_printf(KERN_INFO "WC: wake#1 rsn=%u +%ums - capture window\n",
              wc.r.wake1, ne102_boot_elapsed_ms() - t0);

    WC_LEG("capture");
    wc_capture_persist(0);

    /* park #2: the immediate re-sleep; the capture intent was one-shot,
     * so wake#2 is idle by construction */
    WC_LEG("park2");
    sr = pt_sleep_sync(ptype, wc.secs * 1000, timeout);
    wc.r.park2 = sr;
    if (sr == RET_OK) {
        wc.r.wake2 = pt_sleep_wake_reason();
    }
    wc.r.total_ms = ne102_boot_elapsed_ms() - t0;   /* awake time only:
                                                     * parks freeze the
                                                     * jiffies clock */
    os_printf(KERN_INFO
              "WC: cycle done: park1=%d wake1=%u | cap rc=%d %uB %ums | "
              "sd rc=%d %ums | http=%d %ums | park2=%d wake2=%u | "
              "awake=%ums\n",
              wc.r.park1, wc.r.wake1, wc.r.cap_rc, wc.r.jpg_len,
              wc.r.cap_ms, wc.r.sd_rc, wc.r.sd_ms, wc.r.http_status,
              wc.r.http_ms, wc.r.park2, wc.r.wake2, wc.r.total_ms);
done:
    pt_led_hold_set(0);
    wc.busy = 0;
}

/* ---- RTCC reboot path ---- */

/* AT-time arm: persist the intent, park RTCC; the wake is a REBOOT -
 * execution never resumes past the dispatch (busy stays 1, the session
 * ends when the chip powers down). */
static void wc_rtcc_arm(void)
{
    struct wc_intent it;
    int32_t sr;

    wc.busy = 1;
    os_sema_eat(&wc_start_sema);
    pt_led_hold_set(1);
    os_memset(&it, 0, sizeof(it));
    it.magic = WC_INTENT_MAGIC;
    it.armed = 1;
    it.mode = wc.mode;
    it.skip = wc.skip;
    it.secs = wc.secs;
    os_strcpy(it.url, wc.url);
    if (syscfg_set_binary(WC_KV_KEY, &it, sizeof(it)) != FDB_NO_ERR) {
        os_printf(KERN_ERR "WC: intent KV write failed - RTCC arm aborted\n");
        goto fail;
    }
    os_printf(KERN_INFO "WC: RTCC arm: intent saved (KV), parking RTCC %us"
              "- wake=REBOOT, capture runs at next boot (cold start)\n",
              wc.secs);
    sr = pt_sleep_park(SYSTEM_SLEEP_TYPE_RTCC, wc.secs * 1000);
    if (sr != RET_OK) {
        os_printf(KERN_ERR "WC: RTCC park refused sr=%d - intent cleared, "
                  "staying awake\n", sr);
        syscfg_delete(WC_KV_KEY);
        goto fail;
    }
    return;   /* the RTCC park powers down within the entry window; the
               * reboot wake never resumes this task */
fail:
    pt_led_hold_set(0);
    wc.busy = 0;
}

/* boot-time cycle (intent already consumed by the caller): cold-start
 * capture+persist, report, then re-sleep RTCC - the NEXT boot has no
 * intent and comes up idle (one-shot). The report lives on the serial
 * log only: the re-sleep reboot wipes wc.r. */
static void wc_boot_cycle(void)
{
    uint32_t t0 = ne102_boot_elapsed_ms();
    int32_t sr;

    wc.busy = 1;
    os_sema_eat(&wc_start_sema);
    pt_led_hold_set(1);
    os_memset(&wc.r, 0, sizeof(wc.r));
    wc.r.sd_rc = RET_ERR;
    os_printf(KERN_INFO "WC: cold-boot capture: mode=%u(%s) secs=%us "
              "skip=%u url=%s | boot rsn=%u +%ums\n",
              wc.mode, wc_mode_name(wc.mode), wc.secs, wc.skip, wc.url,
              sys_wakeup_reason(), t0);
    wc.r.wake1 = sys_wakeup_reason();   /* name tag source on this path */
    WC_LEG("bootcap");

    wc_capture_persist(1);   /* wait_ip: the post-boot association may
                              * still be coming up for the HTTP leg */

    os_printf(KERN_INFO
              "WC: boot cycle done: cap rc=%d %uB %ums | sd rc=%d %ums | "
              "http=%d %ums (ip wait %ums) | boot->report=%ums\n",
              wc.r.cap_rc, wc.r.jpg_len, wc.r.cap_ms, wc.r.sd_rc,
              wc.r.sd_ms, wc.r.http_status, wc.r.http_ms, wc.r.ip_wait_ms,
              ne102_boot_elapsed_ms() - t0);

    /* immediate re-sleep on the same path (intent stays consumed - the
     * next boot is the idle one) */
    os_printf(KERN_INFO "WC: re-sleep RTCC %us - next boot is IDLE\n",
              wc.secs);
    sr = pt_sleep_park(SYSTEM_SLEEP_TYPE_RTCC, wc.secs * 1000);
    if (sr != RET_OK) {
        os_printf(KERN_ERR "WC: re-sleep RTCC refused sr=%d - staying "
                  "awake (idle)\n", sr);
        pt_led_hold_set(0);
        wc.busy = 0;
    }
    /* busy stays 1: the session ends in the RTCC reboot */
}

static void wc_task(void *arg)
{
    uint8_t r;

    (void)arg;
    for (;;) {
        os_sema_down(&wc_start_sema, osWaitForever);
        r = wc.req;
        wc.req = 0;
        if (r == WC_REQ_RTCC_ARM) {
            wc_rtcc_arm();
        } else if (r == WC_REQ_BOOT) {
            wc_boot_cycle();
        } else {
            wc_cycle();
        }
    }
}

/* ---- AT handlers ---- */

static int32_t wc_atcmd_cap(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t mode, secs, skip = wc.skip, type = 1;
    struct netif *nif;
    struct wc_intent it;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        uint8_t armed = 0;

        if (syscfg_get_binary(WC_KV_KEY, &it, sizeof(it)) == sizeof(it)
            && it.magic == WC_INTENT_MAGIC && it.armed) {
            armed = 1;
        }
        atcmd_resp("busy=%u hb_age=%ums%s mode=%u(%s) secs=%us skip=%u "
                   "park=%u url=%s rtcc_intent=%u",
                   wc.busy,
                   wc.busy ? (uint32_t)(ne102_boot_elapsed_ms() - wc.hb_ms)
                           : 0,
                   (wc.busy && ne102_boot_elapsed_ms() - wc.hb_ms > 60000)
                       ? "(HUNG? last leg marker in serial log)" : "",
                   wc.mode, wc_mode_name(wc.mode), wc.secs,
                   (uint32_t)wc.skip, wc.sleep_type, wc.url, armed);
        atcmd_resp("last: park1=%d wake1=%u cap rc=%d %uB %ums sd=%d %ums "
                   "http=%d %ums park2=%d wake2=%u awake=%ums",
                   wc.r.park1, wc.r.wake1, wc.r.cap_rc, wc.r.jpg_len,
                   wc.r.cap_ms, wc.r.sd_rc, wc.r.sd_ms, wc.r.http_status,
                   wc.r.http_ms, wc.r.park2, wc.r.wake2, wc.r.total_ms);
        return 0;
    }
    if (argc < 2 || !argv[0][0] || !argv[1][0]) {
        atcmd_resp("usage: AT+PWCAP=<mode 0..3>,<secs 5..3600>[,<skip "
"0..10>[,<type 1|3|5>]] (mode 0=disarm RTCC intent 1=SD 2=HTTP 3=both; "
"type 1=SRAM resume 3=SRAM_ONLY no-wifi-keepalive 5=RTCC cold-boot)");
        return -1;
    }
    mode = os_atoi(argv[0]);
    secs = os_atoi(argv[1]);
    if (argc >= 3 && argv[2][0]) {
        skip = os_atoi(argv[2]);
    }
    if (argc >= 4 && argv[3][0]) {
        type = os_atoi(argv[3]);
    }
    if (mode == 0) {
        syscfg_delete(WC_KV_KEY);
        atcmd_resp("OK: RTCC intent cleared (if any)");
        return 0;
    }
    if (mode < 1 || mode > 3 || secs < 5 || secs > 3600 || skip > 10
        || (type != SYSTEM_SLEEP_TYPE_SRAM_WIFI
            && type != SYSTEM_SLEEP_TYPE_SRAM_ONLY
            && type != SYSTEM_SLEEP_TYPE_RTCC)) {
        atcmd_resp("ERR: mode 1..3, secs 5..3600, skip 0..10, type 1|3|5");
        return -1;
    }
    if (wc.busy || pt_sleep_active()) {
        atcmd_resp("ERR: a sleep test is already running");
        return -1;
    }
    if ((mode & WC_MODE_HTTP) && os_strncmp(wc.url, "http://", 7) != 0) {
        atcmd_resp("ERR: AT+PWCURL=http://host[:port]/path first");
        return -1;
    }
    if (type == SYSTEM_SLEEP_TYPE_SRAM_ONLY) {
        /* SRAM_ONLY parks drop the association - no remote-data wake,
         * and the HTTP leg has nothing to talk to; this type exists
         * for the SD-only low-power scenario (user request 2026-10-23:
         * "wifi-off, SD-only, no upload") */
        if (mode & WC_MODE_HTTP) {
            atcmd_resp("WARN: type 3 parks keep NO association - the "
                       "HTTP leg will fail; use mode 1 for SD-only");
        }
    }
    nif = netif_find("w0");
    if (!nif || !nif->ip_addr.addr) {
        /* warn but proceed: the park still runs (RC backstop), the
         * upload leg just fails fast (SRAM path) or waits bounded
         * (RTCC boot path) - mirrors AT+PWRS behavior */
        atcmd_resp("WARN: no IP yet - HTTP leg may wait/fail "
                   "(AT+SSID/AT+KEY)");
    }
    wc.mode = (uint8_t)mode;
    wc.secs = (uint32_t)secs;
    wc.skip = (uint8_t)skip;
    wc.sleep_type = (type == SYSTEM_SLEEP_TYPE_RTCC)
                        ? SYSTEM_SLEEP_TYPE_SRAM_WIFI : (uint8_t)type;
    wc.req = (type == SYSTEM_SLEEP_TYPE_RTCC) ? WC_REQ_RTCC_ARM
                                              : WC_REQ_CYCLE;
    os_sema_up(&wc_start_sema);
    atcmd_resp("OK: mode=%u(%s) %us skip=%u type=%u(%s)", mode,
               wc_mode_name(mode), secs, (uint32_t)wc.skip, type,
               type == 1 ? "park->wake->capture->camera off->persist->"
                           "re-sleep->idle wake"
                         : (type == 3 ? "SRAM_ONLY parks (NO wifi keepalive"
                                        ", SD-only scenario)"
                                      : "park RTCC->REBOOT wake->cold-boot "
                                        "capture->persist->re-sleep->idle "
                                        "boot"));
    return 0;
}

static int32_t wc_atcmd_url(const char *cmd, char *argv[], uint32_t argc)
{
    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("url=%s", wc.url);
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+PWCURL=http://<ip|host>[:port]/path");
        return -1;
    }
    if (os_strncmp(argv[0], "http://", 7) != 0
        || os_strlen(argv[0]) >= WC_URL_MAX) {
        atcmd_resp("ERR: must start http:// and fit %u chars", WC_URL_MAX);
        return -1;
    }
    os_strcpy(wc.url, argv[0]);
    atcmd_resp("OK: url=%s", wc.url);
    return 0;
}

int wake_capture_init(void)
{
    static uint8_t inited;
    struct wc_intent it;

    if (inited) {
        return RET_OK;
    }
    inited = 1;

    /* Bring the FlashDB KV up: nothing else in this build initializes it
     * (board finding 2026-10-22: every syscfg_* wrapper printed
     * "not inited" and the RTCC intent silently never persisted - the
     * mqttka build suffers the same latent gap). The KVDB partition is
     * the fixed 0x7F0000 slot (fal_cfg.h), clear of the tail parameter
     * sectors and the OTA slot. */
    {
        extern int flashdb_init(void);

        if (!syscfg_get_dev() && flashdb_init() != 0) {
            os_printf(KERN_ERR "WC: flashdb_init failed - RTCC intent "
                      "persistence unavailable\n");
        }
    }

    os_sema_init(&wc_start_sema, 0);
    os_sema_init(&wc_http_sema, 0);
    os_sema_init(&wc_http_done_sema, 0);
    os_task_create("wc_cycle", wc_task, NULL,
                   OS_TASK_PRIORITY_NORMAL, 0, NULL, WC_TASK_STACK);
    os_task_create("wc_http", wc_http_worker, NULL,
                   OS_TASK_PRIORITY_NORMAL, 0, NULL, 8192);
    atcmd_register("AT+PWCAP", wc_atcmd_cap);
    atcmd_register("AT+PWCURL", wc_atcmd_url);

    /* boot-time RTCC intent check: AT+PWCAP=...,5 parked with RTCC
     * (reboot wake) and left the intent in the KV - THIS boot runs the
     * cold-start capture. Consume it first (one-shot: a crash mid-
     * cycle must not re-trigger on later boots). */
    if (syscfg_get_binary(WC_KV_KEY, &it, sizeof(it)) == sizeof(it)
        && it.magic == WC_INTENT_MAGIC && it.armed) {
        syscfg_delete(WC_KV_KEY);
        wc.mode = it.mode;
        wc.skip = it.skip;
        wc.secs = it.secs;
        os_strcpy(wc.url, it.url);
        wc.req = WC_REQ_BOOT;
        os_sema_up(&wc_start_sema);
        os_printf(KERN_INFO "WC: pending RTCC capture intent found -> "
                  "cold-boot capture cycle\n");
    }
    os_printf(KERN_INFO "WC: wake-capture test ready. AT+PWCAP=<0..3>,"
              "<secs>[,<skip>[,<type 1|5>]] cycle, AT+PWCURL=<url> "
              "target (stash reserved by app_power_test_init)\n");
    return RET_OK;
}
