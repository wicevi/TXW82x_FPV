/*
 * led.h - NE102 status LED state machine (PC0, config mode only).
 */
#ifndef __NE102_LED_H__
#define __NE102_LED_H__

/* PRD 5.1 states (fast-capture mode never initializes the LED, PRD 4.4) */
enum ne102_led_state {
    NE102_LED_OFF = 0,      /* dark */
    NE102_LED_ON,           /* solid: booting */
    NE102_LED_AP_READY,     /* slow blink 1Hz: AP up, waiting for client */
    NE102_LED_WEB_ACTIVE,   /* fast blink 4Hz: web session active */
    NE102_LED_OTA,          /* double-blink loop: upgrading (phase 5) */
    NE102_LED_ERROR,        /* 2s on / 1s off loop */
    NE102_LED_SLEEP_FADE,   /* one-shot fade-out, ends in OFF */
};

/* Configure the GPIO and start the pattern work. */
void ne102_led_init(void);

/* Switch state (any time). SLEEP_FADE ends in OFF by itself. */
void ne102_led_set(enum ne102_led_state st);

enum ne102_led_state ne102_led_get(void);

#endif /* __NE102_LED_H__ */
