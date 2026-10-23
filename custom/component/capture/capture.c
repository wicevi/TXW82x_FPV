/*
 * capture.c - NE102 fast-capture camera track implementation.
 *
 * Pipeline (all SDK blocks, wired like the verified ipc_1080p bring-up):
 *   iic -> sensor probe (GC20C3, 1 lane) -> MIPI CSI -> ISP -> VPP(BUF0)
 *   -> auto_jpg_msi (JPEG HW encoder JPGID0) -> ne102 capture consumer msi
 *   -> sd_fb_write_work (sd workqueue) -> 0:/DCIM/NE102_*.jpg
 *
 * The consumer drops cfg.skip_frames JPEGs first (AE settle, PRD 4.1),
 * saves `count` frames, then disables itself and fires the done callback.
 * Capture boot-sequence numbering keeps filenames unique across re-inits
 * until a real time source (RTC/NTP) exists.
 */
#include <basic_include.h>
#include <lib/multimedia/msi.h>
#include <lib/multimedia/framebuff.h>
#include <lib/heap/av_heap.h>
#include <lib/heap/av_psram_heap.h>
#include <sdfunc_work/sdfunc_work.h>
#include <user_work/user_work.h>
#include <user_work/sd_work.h>
#include <demo/app_common.h>
#include <demo/app_mem.h>
#include <app/app_iic/app_iic.h>
#include <lib/video/isp/isp_dev.h>
#include <lib/video/mipi_csi/mipi_csi.h>
#include <lib/video/vpp/vpp_dev.h>
#include <lib/video/dvp/jpeg/jpg.h>
#include <lib/video/dvp/jpeg/jpg_common.h>
#include <lib/scale/scale_common.h>
#include <lib/scale/scale_dev.h>
#include <video_app/video_msi.h>
#include <stream_define.h>
#include <osal/work.h>
#include "capture.h"
#include "../../app/app_lowpwr_camera.h"

/* Struct allocations follow the video_app msi modules. */
#define CAP_LIBC_MALLOC av_malloc
#define CAP_LIBC_FREE   av_free
#define CAP_LIBC_ZALLOC av_zalloc

#define NE102_DCIM_DIR      "0:/DCIM"
#define NE102_JPG_NAME_FMT  NE102_DCIM_DIR "/NE102_S%u_F%u.jpg"

struct cap_msi_s {
    struct msi *msi;
    uint8_t     src_id;
    uint16_t    skip_left;
    uint16_t    count_left;
    uint16_t    frame_seq;   /* running saved-frame counter for filenames */
    uint8_t     qlen_done;   /* size-governor target pushed this shot */
    char        name[32];
};

static struct {
    uint8_t              inited;
    uint8_t              src_sub;   /* offline scaled path (SCALER_DATA) */
    uint16_t             native_w;  /* sensor native geometry (VPP main) */
    uint16_t             native_h;
    struct msi          *jpg_producer;   /* auto_jpg or jpg_concat msi */
    struct cap_msi_s    *consumer;
    struct ne102_capture_cfg cfg;
    struct capture_result   res;
    capture_done_cb_t    done_cb;
} cap = {
    .cfg = { .skip_frames = 3, .burst = 1 },
};

/* offline scaled-shot staging: native YUV frame in PSRAM (lazy, kept) */
static uint8_t *cap_scale_yuv;
static struct os_work cap_scale_wk;
static void cap_scale_work(struct os_work *work);

struct ne102_capture_cfg *ne102_capture_get_cfg(void)
{
    return &cap.cfg;
}

void ne102_capture_set_cfg(const struct ne102_capture_cfg *cfg)
{
    if (cfg) {
        cap.cfg = *cfg;
    }
}

void ne102_capture_set_done_cb(capture_done_cb_t cb)
{
    cap.done_cb = cb;
}

struct capture_result *ne102_capture_get_result(void)
{
    return &cap.res;
}

uint8_t ne102_capture_busy(void)
{
    return cap.consumer && cap.consumer->count_left;
}

/* ---- consumer msi (modeled on takephoto.c, no thumbnail) ---- */

/* last-captured JPEG frame stash (no-SD direct upload, 2026-10-14) */
uint8_t *cap_last_jpg_v;
uint32_t cap_last_jpg_len_v;

/* Pre-reserve the PSRAM stash at boot, BEFORE any capture_init carves
 * the 7.5MB AV pool: the general PSRAM heap is fresh here so the 512KB
 * request succeeds; from the msi consumer (after pool carving) it could
 * fail and the stash stayed empty (board finding 2026-10-16). */
void ne102_capture_stash_reserve(uint32_t bytes)
{
    if (!cap_last_jpg_v) {
        cap_last_jpg_v = os_malloc_psram(bytes);
    }
}

void ne102_capture_get_last_jpg(uint8_t **buf, uint32_t *len)
{
    *buf = cap_last_jpg_v;
    *len = cap_last_jpg_len_v;
}

static int32_t cap_msi_action(struct msi *msi, uint32_t cmd_id,
                              uint32_t param1, uint32_t param2)
{
    int32_t ret = RET_OK;
    struct cap_msi_s *c = (struct cap_msi_s *)msi->priv;

    switch (cmd_id) {
    case MSI_CMD_POST_DESTROY:
        CAP_LIBC_FREE(c);
        break;

    case MSI_CMD_START:
        if (param1 > 0) {
            c->count_left = param1;
            c->skip_left = cap.cfg.skip_frames;
            c->qlen_done = 0;
            cap.res.state = CAPTURE_ARMED;
            cap.res.frames_saved = 0;
            cap.res.arm_ms = ne102_boot_elapsed_ms();
            msi->enable = 1;
            os_printf("NE102: shot armed n=%u skip=%u @%ums\n",
                      (uint32_t)c->count_left, (uint32_t)c->skip_left,
                      cap.res.arm_ms);
        }
        break;

    case MSI_CMD_TRANS_FB: {
        struct framebuff *fb = (struct framebuff *)param1;
        if (fb->mtype == F_JPG && c->src_id == fb->srcID) {
            /* first frames of the shot: push the size-governor target.
             * The encoder msi (jpg_v3) is created lazily at stream start
             * and may not exist when the FIRST frame arrives - retry on
             * every frame until the command lands. Sub-command encoding:
             * msi_do_cmd(msi, MSI_CMD_HARDWARE_JPEG, MSI_JPEG_SET_LEN,
             * bytes). The governor's PID loop (qt + DQT index) then
             * steers toward the target on the following frames - the
             * skipped AE-settle frames double as warmup. */
            if (!c->qlen_done && cap.cfg.target_len) {
                extern struct msi *g_jpg_msi[]; /* jpg_v3_msi.c instances */

                if (g_jpg_msi[JPGID0]) {
                    c->qlen_done = 1;
                    msi_do_cmd(g_jpg_msi[JPGID0], MSI_CMD_HARDWARE_JPEG,
                               MSI_JPEG_SET_LEN, cap.cfg.target_len);
                    os_printf(KERN_INFO "NE102: jpg target_len=%u "
                              "(frame #%u)\n",
                              (uint32_t)cap.cfg.target_len, c->frame_seq);
                } else if (c->frame_seq == 0) {
                    os_printf(KERN_INFO "NE102: jpg msi not ready "
                              "(target %u pending)\n",
                              (uint32_t)cap.cfg.target_len);
                }
            }
            /* stash the last frame for no-SD upload (PSRAM copy) */
            {
                extern uint8_t *cap_last_jpg_v;
                extern uint32_t cap_last_jpg_len_v;
                uint32_t len = fb->len;
                if (len && len < 512 * 1024) {
                    if (!cap_last_jpg_v) {
                        cap_last_jpg_v = os_malloc_psram(512 * 1024);
                    }
                    if (cap_last_jpg_v) {
                        os_memcpy(cap_last_jpg_v, fb->data, len);
                        cap_last_jpg_len_v = len;
                    }
                }
            }
            if (c->skip_left) {
                c->skip_left--;
            } else if (c->count_left) {
                char filename[64];

                /* stash_only: no SD write from the msi context - the
                 * frame is already in the PSRAM stash above; the caller
                 * persists it after deinit cut the rails (wake_capture) */
                if (cap.cfg.stash_only) {
                    os_snprintf(filename, sizeof(filename), "frame #%u",
                                ++c->frame_seq);
                } else {
                    os_snprintf(filename, sizeof(filename),
                                NE102_JPG_NAME_FMT,
                                (uint32_t)cap.res.arm_ms, ++c->frame_seq);
                    sd_fb_write_work(fb, filename, 0);
                    os_strncpy(cap.res.last_path, filename,
                               sizeof(cap.res.last_path) - 1);
                }
                c->count_left--;
                cap.res.frames_saved++;
                cap.res.last_ms = ne102_boot_elapsed_ms();
                os_printf("NE102: %s %s @%ums (%u/%u left)\n",
                          cap.cfg.stash_only ? "stashed" : "saved",
                          filename, cap.res.last_ms,
                          (uint32_t)cap.res.frames_saved,
                          (uint32_t)c->count_left);
                if (!c->count_left) {
                    msi->enable = 0;
                    cap.res.state = CAPTURE_DONE;
                    if (cap.done_cb) {
                        cap.done_cb(&cap.res);
                    }
                }
            }
        }
        ret = RET_ERR;
        break;
    }

    default:
        break;
    }
    return ret;
}

static struct cap_msi_s *cap_consumer_new(void)
{
    uint8_t isnew = 0;
    struct cap_msi_s *c = CAP_LIBC_ZALLOC(sizeof(struct cap_msi_s));
    struct msi *msi;

    if (!c) {
        return NULL;
    }
    os_sprintf(c->name, "ne102_cap[%05d]_%p",
               (uint32_t)os_jiffies() % 99999, c);
    c->src_id = FRAMEBUFF_SOURCE_CAMERA0;
    msi = msi_new(c->name, 0, &isnew);
    if (!msi || !isnew) {
        if (msi) {
            msi_destroy(msi);
        }
        CAP_LIBC_FREE(c);
        return NULL;
    }
    msi->priv = c;
    msi->action = cap_msi_action;
    msi->enable = 0;
    c->msi = msi;
    return c;
}

/* ---- pipeline bring-up / tear-down ---- */

static void cap_vpp_init(uint16_t w, uint16_t h)
{
    os_printf(KERN_INFO "NE102: vpp cfg w:%d h:%d\n", w, h);
    vpp_set_buf_msg(VPP_BUF0, 1, VPP_BUF_IN_SRAM, w, h);
    vpp_set_buf_msg(VPP_BUF1, SUB_STREAM_EN, VPP_BUF_IN_PSRAM,
                    SUB_STREAM_WIDTH, 0);
    vpp_set_double_psram_for_buf1(0);
    vpp_cfg(VPP_INPUT_FROM);
}

int32_t ne102_capture_init(void)
{
    int32_t ret;
    uint16_t w = 0, h = 0;

    if (cap.inited) {
        return RET_OK;
    }

    /* Sensor rails are first raised by app_board_power_init() on cold boot;
     * re-raise them on capture re-init cycles (after ne102_capture_deinit). */
    app_camera_rails_power(1);

    /* AV pools + workqueues + mutexes + jpg_mem: created ONCE per boot.
     * None of them has a teardown API and ne102_capture_deinit() leaves
     * them alive (only the msi chain / sensor / isp / vpp are torn down).
     * The old per-cycle re-init was NOT harmless: the 4MB AV-pool re-carve
     * fails (previous pool not freed, only ~3.3MB remain) and the failed
     * allocation path cascades into sem/timer re-init churn, a full
     * workqueue monitor and - in some layouts - a kernel-heap collapse
     * (os_event_init assert loop, board findings 2026-10-16). Reusing the
     * boot-time instances removes the whole failure path. */
    static uint8_t pools_up;

    if (!pools_up) {
        video_psram_init(0, CONFIG_PSRAM_AVHEAP_SIZE);
        video_sram_init(0, CONFIG_AVHEAP_SIZE);
        user_workqueue_init(OS_TASK_PRIORITY_HIGH, NULL, 2048);
        sd_workqueue_init(OS_TASK_PRIORITY_HIGH, NULL, 2048);
        scale_mutex_init();
        jpg_mutex_init();
        jpg_mem_init(32);
        pools_up = 1;
    }

    /* Chip-internal LDOs (VCCSD/VCAM/VCAM2), idempotent. */
    app_power_init();

    /* MIPI/CSI clock: after a dsleep wake the async lowPower resume may
     * not have run yet when a capture cycle starts - without this clock
     * the sensor has no MCLK and its I2C probe dies in ~130ms (0 frames,
     * board finding 2026-10-14; the reference demos do the same in their
     * first resume module). Idempotent. */
    {
        void sysctrl_mipi_lcd_h264_mjpeg_clk_init(void);
        sysctrl_mipi_lcd_h264_mjpeg_clk_init();
    }

    /* iic subsystem: init ONCE per boot (same rationale as the AV pools).
     * iic_thread_init() re-creates the queue head/semaphore/thread every
     * call, and unregister_iic_queue() at deinit marks the node deleted -
     * the combination corrupted the queue across cycles (board trace
     * 2026-10-16: MA 444 -> MF x3 double-free in the sensor probe path).
     * One registration per boot; the single sensor node stays resident. */
    static uint8_t iic_up;

    if (!iic_up) {
        iic_thread_init();
        iic_up = 1;
    }
    sensor_info_init();
    mipi_csi_hardware_config(HG_MIPI_CSI_DEVID, 1, CAM_SINGLE_MASTER_MODE, 0,
                              SENSOR_TYPE_MASTER, 15, NULL);
    get_single_mipi(HG_MIPI_CSI_DEVID, &w, &h);
    isp_cfg_dev();
    cap.src_sub = (cap.cfg.width && cap.cfg.height
                   && (cap.cfg.width < w || cap.cfg.height < h)) ? 1 : 0;
    cap.native_w = w;
    cap.native_h = h;
    cap_vpp_init(w, h);

    if (cap.src_sub) {
        /* offline scaled path (takephoto_module pattern): a direct
         * jpg_concat chain from SCALER_DATA; frames are produced by
         * cap_scale_work (save one native YUV to PSRAM, close VPP,
         * software-feed scale1 into the encoder, reopen VPP) - the
         * live VPP->scale->JPEG chain has no working SDK precedent. */
        os_printf(KERN_INFO "NE102: scaled capture %ux%u (native %ux%u, "
                  "offline scale path)\n",
                  cap.cfg.width, cap.cfg.height, w, h);
        cap.jpg_producer = jpg_concat_msi_init_start(
            JPGID0, cap.cfg.width, cap.cfg.height, NULL, SCALER_DATA, 0);
        OS_WORK_INIT(&cap_scale_wk, cap_scale_work, 0);
    } else {
        /* Producer: hardware JPEG encoder msi fed from VPP_DATA0. */
        cap.jpg_producer = auto_jpg_msi_init(AUTO_JPG, JPGID0, VPP_DATA0);
    }
    if (!cap.jpg_producer) {
        os_printf(KERN_ERR "NE102: auto_jpg_msi_init failed\n");
        ret = RET_ERR;
        goto err_out;
    }

    /* Consumer: skip-frame + file writer. */
    cap.consumer = cap_consumer_new();
    if (!cap.consumer) {
        os_printf(KERN_ERR "NE102: capture consumer msi create failed\n");
        ret = RET_ERR;
        goto err_out;
    }
    msi_add_output(NULL, AUTO_JPG, cap.consumer->msi, NULL);

    cap.res.state = CAPTURE_IDLE;
    cap.inited = 1;
    os_printf("NE102: capture track up @%ums (%dx%d)\n",
              ne102_boot_elapsed_ms(), w, h);
    return RET_OK;

err_out:
    if (cap.jpg_producer) {
        msi_destroy(cap.jpg_producer);
        cap.jpg_producer = NULL;
    }
    /* Leave rails on: config mode may still want the camera; the caller
     * decides whether to power down via ne102_capture_deinit()/board helpers. */
    return ret;
}

/* offline scaled shot (takephoto_module w<=1920 pattern): save one
 * native YUV frame to PSRAM via the VPP ITP path, close VPP, software-
 * feed scale1 from the staging buffer straight into the JPEG encoder,
 * reopen VPP. One encoded frame per round; skip+1 rounds let AE settle
 * (and warm the size governor) - the consumer drops the first `skip`
 * frames exactly like the native path. */
static void cap_scale_work(struct os_work *work)
{
    extern volatile uint8 itp_done; /* vpp_dev.c, set by ITP_DONE_ISR */
    struct vpp_device *vpp = (struct vpp_device *)dev_get(HG_VPP_DEVID);
    struct scale_device *scl =
        (struct scale_device *)dev_get(HG_SCALE1_DEVID);
    uint32_t rounds = (uint32_t)cap.cfg.skip_frames + 1;
    uint32_t i, t;

    (void)work;
    os_printf(KERN_INFO "NE102: scale work begin\n");
    if (!cap_scale_yuv) {
        cap_scale_yuv = os_malloc_psram(
            (uint32_t)cap.native_w * cap.native_h * 3 / 2);
        if (!cap_scale_yuv) {
            os_printf(KERN_ERR "NE102: scale yuv staging alloc failed\n");
            cap.res.state = CAPTURE_FAILED;
            return;
        }
    }
    os_printf(KERN_INFO "NE102: scale staging=%p vpp=%p scl=%p\n",
              cap_scale_yuv, (void *)vpp, (void *)scl);

    /* bring the concat chain up once (takephoto command sequence), arm
     * the consumer, then feed it one scaled frame per round */
    msi_do_cmd(cap.jpg_producer, MSI_CMD_JPEG_CONCAT, MSI_JPEG_START, 0);
    msi_do_cmd(cap.jpg_producer, MSI_CMD_JPEG_CONCAT, MSI_JPEG_FROM,
               SCALER_DATA);
    msi_do_cmd(cap.jpg_producer, MSI_CMD_JPEG_CONCAT, MSI_JPEG_MSG,
               (uint32_t)cap.cfg.width << 16 | cap.cfg.height);
    msi_do_cmd(cap.consumer->msi, MSI_CMD_START, 1, 0);
    msi_do_cmd(cap.jpg_producer, MSI_CMD_JPEG_CONCAT, MSI_JPEG_START, 1);
    os_printf(KERN_INFO "NE102: scale chain up\n");

    for (i = 0; i < rounds; i++) {
        itp_done = 0;
        vpp_itp_save_only(vpp, cap.native_w, cap.native_h,
                          (uint32_t)cap_scale_yuv);
        for (t = 0; t < 500 && !itp_done; t += 2) {
            os_sleep_ms(2);
        }
        os_printf(KERN_INFO "NE102: round %u itp %s (%ums)\n",
                  i, itp_done ? "ok" : "TIMEOUT", t);
        if (!itp_done) {
            break;
        }
        /* hardware replay path (takephoto_module w>1920 reference): the
         * software-feed variant (scale_soft_from_psram_to_enc) never
         * drains on this silicon - scale_get_heigh_cnt stays frozen and
         * the feed loop spins forever (board finding 2026-10-19, with
         * the VPP both open and closed). scale_from_soft_to_jpg instead
         * re-arms the VPP ITP engine to stream the saved PSRAM YUV
         * through the VPP line buffer into scale1; completion is the
         * scale_take_photo flag set by its FRAME_END ISR. */
        {
            extern volatile uint8 scale_take_photo; /* scale_dev_v3.c */

            scale_take_photo = 0;
            scale_from_soft_to_jpg(scl, (uint32_t)cap_scale_yuv,
                                   cap.native_w, cap.native_h,
                                   cap.cfg.width, cap.cfg.height);
            for (t = 0; t < 2000 && !scale_take_photo; t += 2) {
                os_sleep_ms(2);
            }
            os_printf(KERN_INFO "NE102: round %u scale %s (%ums)\n",
                      i, scale_take_photo ? "ok" : "TIMEOUT", t);
        }
    }
    os_printf(KERN_INFO "NE102: scale work end\n");
}

int32_t ne102_capture_shot(uint8_t count)
{
    if (!cap.inited || !cap.consumer) {
        return RET_ERR;
    }
    if (count == 0 || ne102_capture_busy()) {
        return RET_ERR;
    }
    if (cap.src_sub) {
        os_run_work_delay(&cap_scale_wk, 0);
        return RET_OK;
    }
    msi_do_cmd(cap.consumer->msi, MSI_CMD_START, count, 0);
    return RET_OK;
}

int32_t ne102_capture_wait_done(uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (ne102_capture_busy() && waited < timeout_ms) {
        os_sleep_ms(20);
        waited += 20;
    }
    return ne102_capture_busy() ? RET_ERR : RET_OK;
}

int32_t ne102_capture_deinit(void)
{
    os_printf(KERN_INFO "NE102: capture deinit (inited=%u)", cap.inited);
    if (!cap.inited) {
        return RET_OK;
    }

    /* Consumers first so nobody restarts the encoder, then the producer
     * (its PRE_DESTROY also cancels the auto_jpg worker). */
    if (cap.consumer) {
        msi_destroy(cap.consumer->msi);
        cap.consumer = NULL;
    }
    if (cap.jpg_producer) {
        msi_destroy(cap.jpg_producer);
        cap.jpg_producer = NULL;
    }
    cap.src_sub = 0;

    /* Same order as the battery demo suspend path: vpp -> isp -> mipi. */
    vpp_cfg_release();
    isp_dev_close();
    sensor_info_destory();
    /* iic queue node stays registered (see init comment): unregistering
     * here + re-running iic_thread_init next cycle double-freed nodes */
    mipi_csi_hardware_deconfig();

    app_camera_rails_power(0);

    cap.inited = 0;
    os_printf("NE102: capture track down @%ums\n",
              ne102_boot_elapsed_ms());
    return RET_OK;
}
