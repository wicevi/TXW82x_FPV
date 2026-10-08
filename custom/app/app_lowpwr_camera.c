/*
 * custom/app/app_lowpwr_camera.c - low-power camera product application entry
 * (orchestration layer).
 *
 * Call chain:
 *   main() -> app_board_power_init()      (board power switches + status LED,
 *                                          must run before the demo hardware init)
 *   usr_app_init() -> app_lowpwr_camera_init()
 *
 * Role: board power sequencing + status LED heartbeat + initialize the
 * modules under custom/component/ in dependency order + register the
 * low-power module table. This file only orchestrates; component logic
 * lives in custom/component/<component>/.
 */
#include "basic_include.h"
#include "hal/gpio.h"
#include "chip/txw82x/pmu.h"
#include "osal/work.h"
#include "pin_param.h"
#include "app_lowpwr_camera.h"

/*
 * NE102 status LED (LED1, blue, net LED_NET/SYS on PC0).
 * Schematic page 5: PC0 -> R15(1K) -> Q3(MMBT3904 NPN) base, LED anode from
 * 3V3_SYS to the collector => PC0 high = LED on. R10/R11 (10K) pull the base
 * down, so the LED stays off while the pin floats (reset state).
 */
#define BOARD_LED_PIN   PC_0

/* Milliseconds per heartbeat toggle (2 Hz blink) */
#define BOARD_LED_BLINK_MS  250

static struct os_work led_heart_wk;

/* Heartbeat: toggles the LED and re-arms itself on the system workqueue.
 * Started in app_lowpwr_camera_init(), i.e. only after the whole demo init
 * succeeded - LED solid on = reached main(), LED blinking = init completed
 * and the system is alive. */
static int32_t led_heartbeat_work(struct os_work *work)
{
    static uint8_t on = 0;

    on = !on;
    gpio_set_val(BOARD_LED_PIN, on);
    os_run_work_delay(work, BOARD_LED_BLINK_MS);
    return 0;
}

/*
 * NE102 board power switches, driven from the pin-param table (config.cfg).
 *
 *   PIN_CAM_EN    PB6: camera power master switch - gates the external
 *                     sensor 1V2 DVDD rail (schematic page 8). Must be high
 *                     before sensor_info_init()/MIPI config.
 *   PIN_TF_PWR_EN PB7: TF card supply switch (net SDIO_PWR_EN). Must be
 *                     high before app_sd_init() probes the card.
 *
 * Chip-internal rails (VCAM 2V8_CSI, VCAM2 1V8_CSI, VCC_SD 1.8V domain) are
 * enabled by the demo itself in app_power_init(). WiFi-Halow module power
 * (Module_PWR/PA0) and IR-CUT/fill-light are intentionally left untouched
 * here (module off, IR controlled by the PMU coprocessor for now).
 *
 * Unconfigured params read back as 0xFF and are skipped, so this firmware
 * stays portable to other board configs (e.g. the learning board).
 * Polarity assumption: both switches are active-high (to be confirmed on
 * the first board bring-up).
 */
/*
 * NE102 sensor power tree (hardware confirmation 2026-09-30):
 *   CAM_EN (PB6)      -> sensor DVDD 1.2V external regulator enable
 *   VCAM  (chip LDO)  -> 2V8_CSI  sensor AVDD
 *   VCAM2 (chip LDO)  -> 1V8_CSI  sensor IOVDD (also the I2C pull-up rail)
 * Sensors require AVDD first and DVDD last; the original code raised
 * CAM_EN at main() entry, i.e. DVDD led AVDD/IOVDD by ~60 ms and the
 * sensor never completed power-on reset (I2C stayed dead).
 */
void app_board_power_init(void)
{
    uint8_t cam_en = MACRO_PIN(PIN_CAM_EN);
    uint8_t tf_pwr = MACRO_PIN(PIN_TF_PWR_EN);

    /* NE102 FIX: the image-header SPI_CLK (0x1c) that the ROM needs for
     * reliable boot on this GD flash is ALSO used as the runtime XIP clock
     * (ll_xip_clock_init(0) reads it from fw_info). This used to run at
     * the end of boot (app_lowpwr_camera_init), leaving sys_wifi_init()
     * and the whole demo init executing XIP at ~28MHz. Raising it at the
     * first statement of main() speeds up the entire boot chain (~31ms of
     * fetch-bound init time recovered); the boot header stays at 0x1c so
     * flashing/cold-boot always works.
     * 60MHz is the settled value. Do not raise blindly: on-trial results
     * were 100MHz = 1-2 random "system restart fault" per power-up before
     * a surviving boot, 120MHz = permanent reset loop (GD25LQ64E fC spec
     * is 133MHz, so the ceiling is SoC/board-side signal margin). Above
     * 60MHz the boot chain gains nothing measurable (it becomes
     * hardware-wait-bound: sensor ready 496ms @60MHz vs 495ms @100MHz). */
#define NE102_XIP_RUNTIME_CLK_MHZ 60
    {
        extern int ll_xip_clock_init(int clk_mhz);
        ll_xip_clock_init(NE102_XIP_RUNTIME_CLK_MHZ);
    }

    /* Status LED solid on: proves PMU power, flash boot and this code all
     * reached main(). Turns into a heartbeat after demo init completes. */
    gpio_set_dir(BOARD_LED_PIN, GPIO_DIR_OUTPUT);
    gpio_set_val(BOARD_LED_PIN, 1);

    /* 1. keep sensor DVDD off while the LDO rails come up */
    if (cam_en != 0xFF) {
        gpio_set_dir(cam_en, GPIO_DIR_OUTPUT);
        gpio_set_val(cam_en, 0);
    }

    /* 2. AVDD 2.8V, then IOVDD 1.8V (chip LDOs; re-enabled later by the
     *    demo's app_power_init() - idempotent) */
    pmu_vcam_ldo_en(1, VCAM_VOL);
    pmu_vcam2_ldo_en(1, VCAM2_VOL);
    os_sleep_ms(10);

    /* 3. DVDD 1.2V last, then settle before the demo probes the sensor */
    if (cam_en != 0xFF) {
        gpio_set_val(cam_en, 1);
        os_sleep_ms(20);
    }

    if (tf_pwr != 0xFF) {
        gpio_set_dir(tf_pwr, GPIO_DIR_OUTPUT);
        gpio_set_val(tf_pwr, 1);
        /* short settle so the card is ready before app_sd_init() probes */
        os_sleep_ms(10);
    }
}

int app_lowpwr_camera_init(void)
{
    /* XIP runtime clock fix lives in app_board_power_init() (main entry),
     * so the whole boot chain runs at full speed before reaching here. */
    /*
     * Before the product configuration is finalized, CUSTOMER_ID still follows
     * the official demo selected in project_config.h (suggested baseline: 9).
     * The official demo is already initialized in sys_app_init() (which runs
     * before this function); only custom additions are hooked here.
     *
     * TODO: initialize custom components in dependency order:
     *   xxx_init();    // custom/component/<component>/
     */

    /*
     * TODO: low-power registration (reference:
     * sdk/demo/battery_camera_1080p/battery_camera_sleep_1080p_cb.c):
     *   1. Implement suspend/resume for each module and fill the
     *      struct lowPower_module_ops array (array order = power-on dependency
     *      order; suspend runs in reverse order, resume in forward order);
     *   2. lowpower_app_init(user_lowpower_modules, ARRAY_SIZE(user_lowpower_modules));
     */

    /* Start the LED heartbeat: blinking means the full demo init (SD, camera,
     * network, ...) returned to usr_app_init(). */
    OS_WORK_INIT(&led_heart_wk, led_heartbeat_work, 0);
    os_run_work_delay(&led_heart_wk, BOARD_LED_BLINK_MS);

    return RET_OK;
}
