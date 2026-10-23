#ifndef __WAKE_CAPTURE_H__
#define __WAKE_CAPTURE_H__

/*
 * wake_capture - capture-on-wake cycle test for the power test demo
 * (CUSTOMER_ID=11). Registered from app_power_test_init(); see
 * wake_capture.c for the cycle layout and AT commands.
 */

#include <stdint.h>

/* Registers AT+PWCAP / AT+PWCURL (idempotent, no task until used). */
int wake_capture_init(void);

/* 1 while a PWCAP cycle owns the sleep framework (guards AT+PWRS /
 * AT+PWRDT against a second park in the same window). */
uint8_t wake_capture_busy(void);

#endif /* __WAKE_CAPTURE_H__ */
