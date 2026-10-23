#ifndef __APP_POWER_TEST_H__
#define __APP_POWER_TEST_H__

/*
 * app_power_test - sleep-mode test demo entry (CUSTOMER_ID=11,
 * POWER_TEST_DEMO). Called from app_lowpwr_camera_init() after the
 * board power init; see app_power_test.c for the test commands.
 */

#include <stdint.h>

int app_power_test_init(void);

/*
 * Synchronous sleep wrapper over the asynchronous system_sleep() (the
 * RPC dispatch returns in ~2-3ms, long before the physical park).
 *
 *   pt_sleep_park()  dispatch the park RPC, return in the entry window
 *                    (RET_OK = accepted; caller is NOT parked yet)
 *   pt_sleep_wait()  block until the framework sleepcb RESUME step has
 *                    run (full checkpoint resume chain done) or timeout
 *   pt_sleep_sync()  park + wait in one call - returns only when the
 *                    device has actually slept and woken again
 *
 * Between park() and wait() the caller runs in the pre-park entry
 * window: prints/reads only, never teardown (board lesson 2026-10-21,
 * the dtim disarm bug). pt_sleep_wake_reason() returns the live
 * sleepcb reason once wait() came back with RET_OK. pt_sleep_active()
 * reports whether a PWRS/PWRDT test session owns the framework.
 */
int32_t pt_sleep_park(uint8_t type, uint32_t sleep_ms);
int32_t pt_sleep_wait(uint32_t timeout_ms);
int32_t pt_sleep_sync(uint8_t type, uint32_t sleep_ms, uint32_t timeout_ms);
uint8_t pt_sleep_wake_reason(void);
uint8_t pt_sleep_active(void);

/* Force the status LED dark and pause the blink heartbeat for an
 * external cycle (the wake-capture component holds it across its
 * awake windows so they stay current-measurable). */
void pt_led_hold_set(uint8_t on);

#endif  /* __APP_POWER_TEST_H__ */
