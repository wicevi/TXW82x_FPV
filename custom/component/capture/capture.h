/*
 * capture.h - NE102 fast-capture camera track.
 *
 * Owns the camera pipeline for BOTH boot modes' capture needs:
 *   ne102_capture_init()  : sensor -> MIPI -> ISP -> VPP -> JPEG producer msi
 *                     (+ skip-frame consumer msi writing 0:/DCIM)
 *   ne102_capture_shot(n) : arm skip-frames + n shots (non-blocking, MSI based)
 *   ne102_capture_deinit(): tear the msi chain + pipeline down and cut the
 *                     sensor power rails (AVDD/IOVDD/DVDD), so the module
 *                     can be cycled around every sleep/wake iteration.
 *
 * The pipeline reuses SDK blocks (mipi_csi/vpp/auto_jpg_msi/sdfunc_work);
 * nothing here patches SDK sources.
 */
#ifndef __NE102_CAPTURE_H__
#define __NE102_CAPTURE_H__

#include <stdint.h>

struct ne102_capture_cfg {
    uint8_t skip_frames; /* frames dropped for AE settle before 1st shot (0..10) */
    uint8_t burst;       /* default photos per capture job (1..5) */
    uint16_t width;      /* 0 = sensor native (resolution knob, step 2) */
    uint16_t height;     /* 0 = sensor native */
    uint32_t target_len; /* JPEG size-governor target bytes; 0 = SDK default
                          * (50KB). Pushed via MSI_JPEG_SET_LEN on the first
                          * frame of each shot (the encoder msi is created
                          * lazily at stream start). */
    uint8_t stash_only;  /* 1 = consumer does NOT write SD: frames land in
                          * the PSRAM stash only and the CALLER persists
                          * them after ne102_capture_deinit() cut the
                          * camera rails (wake_capture's power shape);
                          * 0 = consumer writes 0:/DCIM files itself */
};

/* Bring the camera track up. Idempotent. RET_OK or first error. */
int32_t ne102_capture_init(void);

/* Arm a shot of `count` JPEGs (uses cfg.skip_frames). Non-blocking;
 * progress/completion via capture_get_state() / done callback. */
int32_t ne102_capture_shot(uint8_t count);

enum capture_state {
    CAPTURE_IDLE = 0,
    CAPTURE_ARMED,
    CAPTURE_DONE,
    CAPTURE_FAILED,
};

struct capture_result {
    uint8_t  state;      /* enum capture_state */
    uint8_t  frames_saved;
    uint32_t arm_ms;     /* elapsed ms since boot when armed */
    uint32_t last_ms;    /* elapsed ms since boot when last frame saved */
    char     last_path[64]; /* newest saved file (upload input, phase 3) */
};

struct capture_result *ne102_capture_get_result(void);
uint8_t ne102_capture_busy(void);

/* Poll until the armed job finishes or timeout. Bounded blocking. */
int32_t ne102_capture_wait_done(uint32_t timeout_ms);

/* Tear everything down incl. sensor rails; re-init cycles are supported. */
int32_t ne102_capture_deinit(void);

void ne102_capture_set_cfg(const struct ne102_capture_cfg *cfg);
struct ne102_capture_cfg *ne102_capture_get_cfg(void);

/* Called from msi context when the armed job completes (keep it short:
 * state update / work posting only). Argument: final result snapshot. */
typedef void (*capture_done_cb_t)(struct capture_result *res);
void ne102_capture_set_done_cb(capture_done_cb_t cb);

/* Last captured JPEG frame (PSRAM stash; valid until the next shot).
 * Used by the no-SD direct-upload path. */
void ne102_capture_get_last_jpg(uint8_t **buf, uint32_t *len);

/* Reserve the PSRAM stash early (before capture_init's AV-pool carving);
 * the no-SD capture path depends on it - call once at boot. */
void ne102_capture_stash_reserve(uint32_t bytes);

#endif /* __NE102_CAPTURE_H__ */
