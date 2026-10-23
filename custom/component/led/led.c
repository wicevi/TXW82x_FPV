/*
 * led.c - NE102 status LED state machine (PC0, active high via Q3 NPN).
 *
 * One 100ms system-workqueue tick drives all patterns; states map to a
 * (period_ms, on_ms) blink description, with special cases for the
 * double-blink OTA pattern and the one-shot sleep fade.
 */
#include <basic_include.h>
#include <hal/gpio.h>
#include <osal/work.h>
#include "led.h"

#define NE102_LED_PIN          PC_0
#define NE102_LED_TICK_MS      100

static struct os_work led_wk;
static enum ne102_led_state led_state = NE102_LED_OFF;
static uint32_t led_tick;

/* on/off window (ms) per 100ms tick, derived from each pattern */
static void led_apply(enum ne102_led_state st, uint32_t t)
{
    uint8_t on = 0;

    switch (st) {
    case NE102_LED_OFF:
        on = 0;
        break;
    case NE102_LED_ON:
        on = 1;
        break;
    case NE102_LED_AP_READY:        /* 1Hz: 500 on / 500 off */
        on = (t % 1000) < 500;
        break;
    case NE102_LED_WEB_ACTIVE:      /* 4Hz: 125 on / 125 off */
        on = (t % 250) < 125;
        break;
    case NE102_LED_OTA: {           /* double blink per 1.6s */
        uint32_t m = t % 1600;
        on = (m < 150) || (m >= 300 && m < 450);
        break;
    }
    case NE102_LED_ERROR:           /* 2s on / 1s off */
        on = (t % 3000) < 2000;
        break;
    case NE102_LED_SLEEP_FADE: {    /* one-shot: 900ms on shrinking to 0 */
        uint32_t elapsed = t;
        if (elapsed >= 1800) {
            led_state = NE102_LED_OFF; /* falls through to OFF below */
            on = 0;
        } else {
            /* duty drops 50% every 300ms: 300@100%, 300@50%, ... */
            uint32_t duty = 100 - (elapsed / 300) * 20;
            on = (elapsed % 300) < (300 * duty / 100);
        }
        break;
    }
    default:
        on = 0;
        break;
    }
    gpio_set_val(NE102_LED_PIN, on);
}

static int32_t led_work(struct os_work *work)
{
    led_tick += NE102_LED_TICK_MS;
    led_apply(led_state, led_tick);
    os_run_work_delay(work, NE102_LED_TICK_MS);
    return 0;
}

void ne102_led_init(void)
{
    gpio_set_dir(NE102_LED_PIN, GPIO_DIR_OUTPUT);
    gpio_set_val(NE102_LED_PIN, 0);
    led_state = NE102_LED_OFF;
    led_tick = 0;
    OS_WORK_INIT(&led_wk, led_work, 0);
    os_run_work_delay(&led_wk, NE102_LED_TICK_MS);
}

void ne102_led_set(enum ne102_led_state st)
{
    if (st != led_state) {
        led_state = st;
        led_tick = 0; /* restart pattern phase */
        led_apply(st, 0);
    }
}

enum ne102_led_state ne102_led_get(void)
{
    return led_state;
}
