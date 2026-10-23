#ifndef __CUSTOM_APP_LOW_PWR_CAMERA_H__
#define __CUSTOM_APP_LOW_PWR_CAMERA_H__

/*
 * custom/app - board bring-up for the MQTT keepalive bench.
 *
 * Call chain (project/txw82xApp/main.c):
 *   main() -> app_board_power_init()   status LED + board power switches
 *   usr_app_init() -> app_lowpwr_camera_init() -> ne102_mqttka_init()
 *
 * Bench logic lives in custom/component/mqttka; this header exposes the
 * board-level helpers shared by the components.
 */

void app_board_power_init(void);
int app_lowpwr_camera_init(void);

/* Sensor power rails (AVDD/IOVDD via chip LDOs + DVDD via CAM_EN PB6).
 * on:  DVDD low -> LDOs on -> 10ms -> DVDD high -> 20ms (AVDD first,
 *      DVDD last, hardware requirement);
 * off: DVDD low -> LDOs off. Safe to call repeatedly. */
void app_camera_rails_power(unsigned char on);

/* TF card supply switch (PIN_TF_PWR_EN PB7). */
void ne102_board_tf_power(unsigned char on);

/* Milliseconds elapsed since app_board_power_init() (main entry),
 * for boot-latency instrumentation across modules. */
unsigned int ne102_boot_elapsed_ms(void);

#endif /* __CUSTOM_APP_LOW_PWR_CAMERA_H__ */
