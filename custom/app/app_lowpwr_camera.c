/*
 * custom/app/app_lowpwr_camera.c - board bring-up for the keepalive bench.
 *
 * Call chain: main() -> app_board_power_init() -> ... -> usr_app_init()
 *             -> app_lowpwr_camera_init() -> ne102_mqttka_init()
 *
 * This file only brings the board up (status LED + camera/TF power
 * switches from the pin-param table) and hands control to the keepalive
 * component; all bench logic lives in custom/component/mqttka.
 */
#include <basic_include.h>
#include <hal/gpio.h>
#include <chip/txw82x/pmu.h>
#include <pin_param.h>
#include "app_lowpwr_camera.h"
#include "app_power_test/app_power_test.h"

#define BOARD_LED_PIN   PC_0

static uint64_t boot_jiffies;
static uint8_t boot_jiffies_set;

unsigned int ne102_boot_elapsed_ms(void)
{
    if (!boot_jiffies_set) {
        return 0;
    }
    return (unsigned int)os_jiffies_to_msecs(os_jiffies() - boot_jiffies);
}

/*
 * Camera power tree (hardware confirmation 2026-09-30):
 *   CAM_EN (PB6)      -> sensor DVDD 1.2V external regulator enable
 *   VCAM  (chip LDO)  -> 2V8_CSI  sensor AVDD
 *   VCAM2 (chip LDO)  -> 1V8_CSI  sensor IOVDD (also the I2C pull-up rail)
 * Sensors require AVDD first and DVDD last. The keepalive bench keeps
 * both rails OFF for the whole session (the lowpower module table in
 * mqttka re-asserts the cut on every suspend); they only come up for a
 * remote-capture cycle.
 */
void app_camera_rails_power(unsigned char on)
{
    uint8_t cam_en = MACRO_PIN(PIN_CAM_EN);

    if (on) {
        if (cam_en != 0xFF) {
            gpio_set_dir(cam_en, GPIO_DIR_OUTPUT);
            gpio_set_val(cam_en, 0);
        }
        pmu_vcam_ldo_en(1, VCAM_VOL);
        pmu_vcam2_ldo_en(1, VCAM2_VOL);
        os_sleep_ms(10);
        if (cam_en != 0xFF) {
            gpio_set_val(cam_en, 1);
            os_sleep_ms(20);
        }
    } else {
        if (cam_en != 0xFF) {
            gpio_set_val(cam_en, 0);
        }
        pmu_vcam_ldo_en(0, VCAM_VOL);
        pmu_vcam2_ldo_en(0, VCAM2_VOL);
    }
}

void ne102_board_tf_power(unsigned char on)
{
    uint8_t tf_pwr = MACRO_PIN(PIN_TF_PWR_EN);

    if (tf_pwr == 0xFF) {
        return;
    }
    gpio_set_dir(tf_pwr, GPIO_DIR_OUTPUT);
    gpio_set_val(tf_pwr, on ? 1 : 0);
    if (on) {
        os_sleep_ms(10);
    }
}

void app_board_power_init(void)
{
    boot_jiffies = os_jiffies();
    boot_jiffies_set = 1;

    /* Status LED solid on at main() entry (converted to the keepalive
     * idle blink by ne102_mqttka_init). */
    gpio_set_dir(BOARD_LED_PIN, GPIO_DIR_OUTPUT);
    gpio_set_val(BOARD_LED_PIN, 1);

    app_camera_rails_power(1);
    ne102_board_tf_power(1);
}

int app_lowpwr_camera_init(void)
{
#ifdef NE102_PRODUCT
    /* keepalive bench entry (was lost in the power-test reshuffle -
     * the branch sat empty and the mqttka firmware could not start;
     * restored 2026-10-22) */
    {
        extern void ne102_mqttka_init(void);

        ne102_mqttka_init();
    }
#endif
#ifdef POWER_TEST_DEMO
    app_power_test_init();
#endif
    return RET_OK;
}
