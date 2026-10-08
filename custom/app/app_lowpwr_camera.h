#ifndef __CUSTOM_APP_LOW_PWR_CAMERA_H__
#define __CUSTOM_APP_LOW_PWR_CAMERA_H__

/*
 * custom/app - low-power camera product application layer.
 * Entry app_lowpwr_camera_init() is called by usr_app_init() in
 * project/txw82xApp/main.c.
 * app_board_power_init() is called at the very start of main(), BEFORE
 * sys_app_init(), because the demo's SD/camera init runs inside
 * sys_app_init() and needs the board power switches already on.
 */

void app_board_power_init(void);
int app_lowpwr_camera_init(void);

#endif /* __CUSTOM_APP_LOW_PWR_CAMERA_H__ */
